/*
 * Slack HTTP — modeled on wee-slack EventRouter:
 *
 *  - Enqueue all requests (no stampede from callers).
 *  - Fast queue + slow queue (history/members); slow promotes ≤1/sec.
 *  - Max concurrent in-flight limited.
 *  - On 429: global cooldown from Retry-After, re-queue job (no hammer).
 *  - Soft failures: quadratic backoff re-queue.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <assert.h>

#include <curl/curl.h>

#include "weeslack.h"
#include "slack_http.h"
#include "slack_ws.h"

struct t_slack_http_request *slack_http_requests = NULL;

/* Integer constant expressions (C11-friendly; prefer over #define). */
enum
{
    SLACK_HTTP_MAX_TRIES         = 3,
    /* 2 felt too slow once API + binary shared the multi budget; 4 is a
     * better default. Still far below unlimited stampede. */
    SLACK_HTTP_MAX_CONCURRENT    = 4,
    SLACK_HTTP_TICK_MS           = 100,
    SLACK_HTTP_RETRY_DEFAULT_SEC = 8,
    SLACK_HTTP_RETRY_MAX_SEC     = 90,
    SLACK_HTTP_API_RESP_INIT     = 4096,
    SLACK_HTTP_API_RESP_MAX      = 16 * 1024 * 1024,
    SLACK_HTTP_API_HDRS_INIT     = 1024,
    SLACK_HTTP_API_HDRS_MAX      = 256 * 1024,
    SLACK_HTTP_CURL_TIMER_MS     = 50,
    SLACK_HTTP_TIMEOUT_FALLBACK_MS = 30000
};

static_assert(SLACK_HTTP_MAX_CONCURRENT >= 1, "need at least one concurrent slot");
static_assert(SLACK_HTTP_MAX_TRIES >= 1, "need at least one try");
static_assert(SLACK_HTTP_RETRY_DEFAULT_SEC >= 1, "default Retry-After must be positive");
static_assert(SLACK_HTTP_RETRY_MAX_SEC >= SLACK_HTTP_RETRY_DEFAULT_SEC,
              "max Retry-After must cover default");
static_assert(SLACK_HTTP_API_RESP_MAX >= SLACK_HTTP_API_RESP_INIT,
              "API body cap must cover initial allocation");
static_assert(SLACK_HTTP_TIMEOUT_FALLBACK_MS >= 5000,
              "fallback HTTP timeout should be at least 5s");

static int g_inflight = 0;
static time_t g_cooldown_until = 0;

static struct t_slack_http_request *g_fast_head = NULL;
static struct t_slack_http_request *g_fast_tail = NULL;
static struct t_slack_http_request *g_slow_head = NULL;
static struct t_slack_http_request *g_slow_tail = NULL;

static time_t g_slow_last_promote = 0;
static struct t_hook *g_queue_timer = NULL;

static void slack_http_queue_pump(void);

/* ---- encoding ---- */

static char *
slack_url_encode(const char *str)
{
    size_t len = strlen(str);
    char *result = malloc(len * 3 + 1);
    char *out = result;

    if (!result)
        return NULL;

    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)str[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            *out++ = c;
        else
        {
            *out++ = '%';
            *out++ = "0123456789abcdef"[c >> 4];
            *out++ = "0123456789abcdef"[c & 0x0F];
        }
    }
    *out = '\0';
    return result;
}

static char *
slack_http_encode_query_string(struct json_object *params)
{
    struct json_object_iterator it, end;
    char *result = NULL;
    size_t len = 0;

    if (!params)
        return NULL;

    it = json_object_iter_begin(params);
    end = json_object_iter_end(params);

    while (!json_object_iter_equal(&it, &end))
    {
        const char *key = json_object_iter_peek_name(&it);
        struct json_object *val = json_object_iter_peek_value(&it);
        const char *val_str = json_object_to_json_string(val);
        size_t val_len = strlen(val_str);
        char *encoded_key, *encoded_val, *out, *new_result;
        size_t key_len, enc_val_len;

        if (val_len >= 2 && val_str[0] == '"' && val_str[val_len - 1] == '"')
        {
            val_str++;
            val_len -= 2;
        }

        encoded_key = slack_url_encode(key);
        encoded_val = calloc(1, val_len * 3 + 1);
        if (encoded_val)
        {
            out = encoded_val;
            for (size_t i = 0; i < val_len; i++)
            {
                unsigned char c = (unsigned char)val_str[i];
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                    *out++ = c;
                else
                {
                    *out++ = '%';
                    *out++ = "0123456789abcdef"[c >> 4];
                    *out++ = "0123456789abcdef"[c & 0x0F];
                }
            }
            *out = '\0';
        }

        if (!encoded_key || !encoded_val)
        {
            free(encoded_key);
            free(encoded_val);
            json_object_iter_next(&it);
            continue;
        }

        key_len = strlen(encoded_key);
        enc_val_len = strlen(encoded_val);
        new_result = realloc(result, len + key_len + enc_val_len + 3);
        if (!new_result)
        {
            free(encoded_key);
            free(encoded_val);
            free(result);
            return NULL;
        }
        result = new_result;
        if (len > 0)
            result[len++] = '&';
        memcpy(result + len, encoded_key, key_len);
        len += key_len;
        result[len++] = '=';
        memcpy(result + len, encoded_val, enc_val_len);
        len += enc_val_len;
        result[len] = '\0';
        free(encoded_key);
        free(encoded_val);
        json_object_iter_next(&it);
    }
    return result;
}

int
slack_http_body_is_ratelimited(const char *body)
{
    struct json_object *json, *ok_obj, *err_obj;

    if (!body || !body[0])
        return 0;
    json = json_tokener_parse(body);
    if (!json)
        return 0;
    if (json_object_object_get_ex(json, "ok", &ok_obj) &&
        json_object_get_boolean(ok_obj))
    {
        json_object_put(json);
        return 0;
    }
    if (json_object_object_get_ex(json, "error", &err_obj))
    {
        const char *e = json_object_get_string(err_obj);
        if (e && strcmp(e, "ratelimited") == 0)
        {
            json_object_put(json);
            return 1;
        }
    }
    json_object_put(json);
    return 0;
}

void
slack_http_set_cooldown(int seconds)
{
    time_t until;

    if (seconds < 1)
        seconds = 1;
    if (seconds > SLACK_HTTP_RETRY_MAX_SEC)
        seconds = SLACK_HTTP_RETRY_MAX_SEC;
    until = time(NULL) + seconds;
    if (until > g_cooldown_until)
        g_cooldown_until = until;
}

int
slack_http_on_cooldown(void)
{
    return time(NULL) < g_cooldown_until;
}

/* ---- registry ---- */

static void
slack_http_reg_add(struct t_slack_http_request *req)
{
    req->reg_next = slack_http_requests;
    req->reg_prev = NULL;
    if (slack_http_requests)
        slack_http_requests->reg_prev = req;
    slack_http_requests = req;
}

static void
slack_http_reg_del(struct t_slack_http_request *req)
{
    if (req->reg_prev)
        req->reg_prev->reg_next = req->reg_next;
    else
        slack_http_requests = req->reg_next;
    if (req->reg_next)
        req->reg_next->reg_prev = req->reg_prev;
    req->reg_next = req->reg_prev = NULL;
}

/* ---- wait queues ---- */

static void
slack_http_q_append(struct t_slack_http_request **head,
                    struct t_slack_http_request **tail,
                    struct t_slack_http_request *req)
{
    req->q_next = NULL;
    req->q_prev = *tail;
    if (*tail)
        (*tail)->q_next = req;
    else
        *head = req;
    *tail = req;
}

static void
slack_http_q_unlink(struct t_slack_http_request **head,
                    struct t_slack_http_request **tail,
                    struct t_slack_http_request *req)
{
    if (req->q_prev)
        req->q_prev->q_next = req->q_next;
    else
        *head = req->q_next;
    if (req->q_next)
        req->q_next->q_prev = req->q_prev;
    else
        *tail = req->q_prev;
    req->q_next = req->q_prev = NULL;
}

static struct t_slack_http_request *
slack_http_q_pop_ready(struct t_slack_http_request **head,
                       struct t_slack_http_request **tail)
{
    struct t_slack_http_request *req, *next;
    time_t now = time(NULL);

    for (req = *head; req; req = next)
    {
        next = req->q_next;
        if (req->retry_not_before && now < req->retry_not_before)
            continue;
        slack_http_q_unlink(head, tail, req);
        return req;
    }
    return NULL;
}

/* Build a curl proxy URL from weechat.network.proxy_curl + weechat.proxy.* */
char *
slack_http_get_proxy_url(void)
{
    struct t_config_option *opt;
    const char *name, *type, *address, *username, *password;
    int port;
    char proxy_url[512];
    char key[128];
    const char *scheme = "http";

    opt = weechat_config_get("weechat.network.proxy_curl");
    if (!opt)
        return NULL;
    name = weechat_config_string(opt);
    if (!name || !name[0])
        return NULL;

    snprintf(key, sizeof(key), "weechat.proxy.%s.address", name);
    opt = weechat_config_get(key);
    address = opt ? weechat_config_string(opt) : NULL;
    if (!address || !address[0])
        return NULL;

    snprintf(key, sizeof(key), "weechat.proxy.%s.port", name);
    opt = weechat_config_get(key);
    port = opt ? weechat_config_integer(opt) : 0;

    snprintf(key, sizeof(key), "weechat.proxy.%s.type", name);
    opt = weechat_config_get(key);
    type = opt ? weechat_config_string(opt) : NULL;
    if (type && type[0])
    {
        if (weechat_strcasecmp(type, "socks4") == 0)
            scheme = "socks4";
        else if (weechat_strcasecmp(type, "socks5") == 0)
            scheme = "socks5";
        else if (weechat_strcasecmp(type, "http") == 0)
            scheme = "http";
    }

    snprintf(key, sizeof(key), "weechat.proxy.%s.username", name);
    opt = weechat_config_get(key);
    username = opt ? weechat_config_string(opt) : NULL;

    snprintf(key, sizeof(key), "weechat.proxy.%s.password", name);
    opt = weechat_config_get(key);
    password = opt ? weechat_config_string(opt) : NULL;

    if (username && username[0])
    {
        if (password && password[0])
            snprintf(proxy_url, sizeof(proxy_url), "%s://%s:%s@%s:%d",
                     scheme, username, password, address, port > 0 ? port : 1080);
        else
            snprintf(proxy_url, sizeof(proxy_url), "%s://%s@%s:%d",
                     scheme, username, address, port > 0 ? port : 1080);
    }
    else
        snprintf(proxy_url, sizeof(proxy_url), "%s://%s:%d",
                 scheme, address, port > 0 ? port : 1080);

    return strdup(proxy_url);
}

/* Implemented with libcurl multi (below). */
static int slack_http_api_curl_start(struct t_slack_http_request *request);
static void slack_http_api_curl_cancel(struct t_slack_http_request *request);
static int slack_http_multi_xfer_count(void);

static void
slack_http_start_request(struct t_slack_http_request *request)
{
    if (!request || !request->workspace || !request->url)
    {
        if (request)
            slack_http_request_free(request);
        return;
    }

    if (!request->workspace->token || !request->workspace->token[0])
    {
        if (request->callback)
            request->callback(request->workspace, -1, NULL, request->user_data);
        slack_http_request_free(request);
        return;
    }

    request->tries++;
    request->in_flight = 1;
    g_inflight++;
    request->hook = NULL; /* legacy; API uses libcurl multi */

    if (!slack_http_api_curl_start(request))
    {
        request->in_flight = 0;
        if (g_inflight > 0)
            g_inflight--;
        SLACK_WS_PRINTF(request->workspace,
                        "%sweeslack: libcurl start failed for %s",
                        weechat_prefix("error"),
                        request->method ? request->method : "?");
        if (request->callback)
            request->callback(request->workspace, -1, NULL, request->user_data);
        slack_http_request_free(request);
    }
}

static void
slack_http_queue_pump(void)
{
    time_t now = time(NULL);
    struct t_slack_http_request *req;

    if (now < g_cooldown_until)
        return;

    /* slow lane: promote at most one ready job per second */
    if (g_slow_head && now > g_slow_last_promote)
    {
        req = slack_http_q_pop_ready(&g_slow_head, &g_slow_tail);
        if (req)
        {
            slack_http_q_append(&g_fast_head, &g_fast_tail, req);
            g_slow_last_promote = now;
        }
    }

    while (g_inflight < SLACK_HTTP_MAX_CONCURRENT &&
           slack_http_multi_xfer_count() < SLACK_HTTP_MAX_CONCURRENT)
    {
        req = slack_http_q_pop_ready(&g_fast_head, &g_fast_tail);
        if (!req)
            break;

        if ((req->flags & SLACK_HTTP_MARK) && now < g_cooldown_until)
        {
            if (req->callback)
                req->callback(req->workspace, -1, NULL, req->user_data);
            slack_http_request_free(req);
            continue;
        }

        slack_http_start_request(req);
    }
}

static int
slack_http_queue_timer_cb(const void *pointer, void *data, int remaining_calls)
{
    (void) pointer;
    (void) data;
    (void) remaining_calls;
    slack_http_queue_pump();
    return WEECHAT_RC_OK;
}

static int
slack_http_q_count(struct t_slack_http_request *head)
{
    int n = 0;
    for (; head; head = head->q_next)
        n++;
    return n;
}

char *
slack_http_queue_status(char *buf, size_t buflen)
{
    time_t now;
    int cool_left;

    if (!buf || buflen == 0)
        return buf;

    now = time(NULL);
    cool_left = (g_cooldown_until > now) ? (int)(g_cooldown_until - now) : 0;

    snprintf(buf, buflen,
             "api_inflight=%d multi=%d/%d fast_q=%d slow_q=%d cooldown=%ds",
             g_inflight,
             slack_http_multi_xfer_count(), SLACK_HTTP_MAX_CONCURRENT,
             slack_http_q_count(g_fast_head),
             slack_http_q_count(g_slow_head),
             cool_left);
    return buf;
}

void
slack_http_queue_init(void)
{
    if (g_queue_timer)
        return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_queue_timer = weechat_hook_timer(
        SLACK_HTTP_TICK_MS, 0, 0,
        &slack_http_queue_timer_cb, NULL, NULL);
}

void
slack_http_queue_shutdown(void)
{
    struct t_slack_http_request *req, *next;

    slack_http_curl_multi_shutdown();

    if (g_queue_timer)
    {
        weechat_unhook(g_queue_timer);
        g_queue_timer = NULL;
    }

    /* free anything still waiting */
    for (req = g_fast_head; req; req = next)
    {
        next = req->q_next;
        slack_http_request_free(req);
    }
    for (req = g_slow_head; req; req = next)
    {
        next = req->q_next;
        slack_http_request_free(req);
    }
    g_fast_head = g_fast_tail = NULL;
    g_slow_head = g_slow_tail = NULL;
    g_inflight = 0;
    g_cooldown_until = 0;
}

static const char *
slack_http_find_retry_after(const char *headers)
{
    const char *p;
    const char *needle = "retry-after:";
    size_t nlen = 12;

    if (!headers)
        return NULL;
    for (p = headers; *p; p++)
    {
        size_t i;
        for (i = 0; i < nlen; i++)
        {
            char a = p[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (a != b)
                break;
        }
        if (i == nlen)
            return p + nlen;
        if (!p[i])
            break;
    }
    return NULL;
}

static int
slack_http_retry_after_from_headers(const char *headers)
{
    const char *p;
    int sec;

    if (!headers || !headers[0])
        return SLACK_HTTP_RETRY_DEFAULT_SEC;

    p = slack_http_find_retry_after(headers);
    if (!p)
        p = headers;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')
        p++;
    sec = atoi(p);
    if (sec > 0)
    {
        if (sec > SLACK_HTTP_RETRY_MAX_SEC)
            sec = SLACK_HTTP_RETRY_MAX_SEC;
        return sec;
    }
    return SLACK_HTTP_RETRY_DEFAULT_SEC;
}

static int
slack_http_requeue(struct t_slack_http_request *request, int delay_sec)
{
    if (!request || request->tries >= request->max_tries)
        return 0;

    request->in_flight = 0;
    request->hook = NULL;
    request->retry_not_before = time(NULL) + (delay_sec > 0 ? delay_sec : 1);

    if (request->flags & SLACK_HTTP_SLOW)
        slack_http_q_append(&g_slow_head, &g_slow_tail, request);
    else
        slack_http_q_append(&g_fast_head, &g_fast_tail, request);

    return 1;
}

static void
slack_http_enqueue(struct t_slack_http_request *request)
{
    if (!request)
        return;

    if (request->flags & SLACK_HTTP_SLOW)
        slack_http_q_append(&g_slow_head, &g_slow_tail, request);
    else
        slack_http_q_append(&g_fast_head, &g_fast_tail, request);

    slack_http_queue_pump();
}

struct t_slack_http_request *
slack_http_request_new_flags(struct t_weeslack_workspace *workspace,
                             const char *method,
                             struct json_object *params,
                             int flags,
                             void (*callback)(struct t_weeslack_workspace *workspace,
                                              int return_code, const char *output,
                                              void *user_data),
                             void *user_data)
{
    struct t_slack_http_request *request;
    char base_url[512];
    char *form_body = NULL;

    if (!workspace || !workspace->token || !method)
        return NULL;

    if ((flags & SLACK_HTTP_MARK) && slack_http_on_cooldown())
        return NULL;

    request = calloc(1, sizeof(struct t_slack_http_request));
    if (!request)
        return NULL;

    request->method = strdup(method);
    snprintf(base_url, sizeof(base_url), "https://slack.com/api/%s", method);
    request->url = strdup(base_url);
    request->flags = flags;
    request->max_tries = SLACK_HTTP_MAX_TRIES;
    request->tries = 0;
    request->retry_not_before = 0;
    request->workspace = workspace;
    request->callback = callback;
    request->user_data = user_data;

    if (params)
        form_body = slack_http_encode_query_string(params);
    if (form_body && form_body[0])
    {
        request->postfields = form_body;
        request->use_post = 1;
    }
    else
        free(form_body);

    request->id = weechat_string_eval_expression(
        "${info:irc_plugin_date_time}.${info:pid}", NULL, NULL, NULL);

    if (!request->url || !request->method)
    {
        slack_http_request_free(request);
        return NULL;
    }

    slack_http_reg_add(request);
    slack_http_enqueue(request);
    return request;
}

struct t_slack_http_request *
slack_http_request_new(struct t_weeslack_workspace *workspace,
                       const char *method,
                       struct json_object *params,
                       void (*callback)(struct t_weeslack_workspace *workspace,
                                        int return_code, const char *output,
                                        void *user_data),
                       void *user_data)
{
    return slack_http_request_new_flags(workspace, method, params, 0,
                                        callback, user_data);
}

void
slack_http_request_free(struct t_slack_http_request *request)
{
    if (!request)
        return;

    /* unlink from wait queues if present */
    if (request->q_next || request->q_prev ||
        request == g_fast_head || request == g_slow_head)
    {
        /* may be on either queue; unlink both safely */
        if (request == g_fast_head || (request->q_prev || request->q_next))
        {
            /* try fast then slow */
            if (request == g_fast_head || request == g_fast_tail ||
                (request->q_prev && (request->q_prev->q_next == request)) ||
                (request->q_next && (request->q_next->q_prev == request)))
            {
                /* Determine which list: scan */
            }
        }
        if (g_fast_head || g_slow_head)
        {
            struct t_slack_http_request *r;
            for (r = g_fast_head; r; r = r->q_next)
            {
                if (r == request)
                {
                    slack_http_q_unlink(&g_fast_head, &g_fast_tail, request);
                    break;
                }
            }
            for (r = g_slow_head; r; r = r->q_next)
            {
                if (r == request)
                {
                    slack_http_q_unlink(&g_slow_head, &g_slow_tail, request);
                    break;
                }
            }
        }
    }

    slack_http_reg_del(request);

    /* Cancel in-flight libcurl API transfer without invoking callback. */
    if (request->in_flight)
        slack_http_api_curl_cancel(request);

    if (request->hook)
    {
        weechat_unhook(request->hook);
        request->hook = NULL;
    }
    if (request->in_flight && g_inflight > 0)
        g_inflight--;
    request->in_flight = 0;

    free(request->id);
    free(request->url);
    free(request->method);
    free(request->postfields);
    free(request);
}

void
slack_http_requests_cancel(struct t_weeslack_workspace *workspace)
{
    struct t_slack_http_request *request, *next;

    request = slack_http_requests;
    while (request)
    {
        next = request->reg_next;
        if (request->workspace == workspace)
            slack_http_request_free(request);
        request = next;
    }
}

struct json_object *
slack_json_decode(const char *json)
{
    if (!json || !json[0])
        return NULL;
    return json_tokener_parse(json);
}

/* ============================================================
 * libcurl multi — API form POST/GET + binary PUT/GET (timer-driven)
 * ============================================================ */

enum slack_curl_op
{
    SLACK_CURL_PUT = 1,
    SLACK_CURL_GET = 2,
    SLACK_CURL_API = 3,  /* Slack web API (form body / GET) */
    SLACK_CURL_BODY = 4  /* GET body to memory (OAuth, etc.) */
};

struct t_slack_curl_xfer
{
    enum slack_curl_op op;
    CURL *easy;
    FILE *fp;
    char *url;
    char *path;
    char *content_type;
    char *authorization; /* "Bearer …" */
    char *cookie;        /* raw cookie value, optional d= prefix handled by caller */
    char errbuf[CURL_ERROR_SIZE];
    t_slack_curl_done_cb callback;
    t_slack_curl_body_cb body_cb;
    void *user_data;
    /* SLACK_CURL_API / BODY */
    struct t_slack_http_request *api_req;
    char *resp_body;
    size_t resp_len;
    size_t resp_cap;
    char *resp_hdrs;
    size_t hdrs_len;
    size_t hdrs_cap;
    struct t_slack_curl_xfer *next;
};

static CURLM *g_curl_multi = NULL;
static struct t_hook *g_curl_timer = NULL;
static struct t_slack_curl_xfer *g_curl_xfers = NULL;
static int g_curl_still_running = 0;

static void
slack_http_curl_apply_proxy_easy(CURL *easy)
{
    char *proxy;

    if (!easy)
        return;
    proxy = slack_http_get_proxy_url();
    if (!proxy)
        return;
    curl_easy_setopt(easy, CURLOPT_PROXY, proxy);
    free(proxy);
}

static void
slack_http_curl_xfer_free(struct t_slack_curl_xfer *x)
{
    if (!x)
        return;
    if (x->fp)
    {
        fclose(x->fp);
        x->fp = NULL;
    }
    if (x->easy)
    {
        if (g_curl_multi)
            curl_multi_remove_handle(g_curl_multi, x->easy);
        curl_easy_cleanup(x->easy);
        x->easy = NULL;
    }
    free(x->url);
    free(x->path);
    free(x->content_type);
    free(x->authorization);
    free(x->cookie);
    free(x->resp_body);
    free(x->resp_hdrs);
    free(x);
}

static void
slack_http_curl_unlink(struct t_slack_curl_xfer *x)
{
    struct t_slack_curl_xfer **pp;

    for (pp = &g_curl_xfers; *pp; pp = &(*pp)->next)
    {
        if (*pp == x)
        {
            *pp = x->next;
            break;
        }
    }
}

static void
slack_http_curl_check_done(void)
{
    CURLMsg *msg;
    int msgs_left;

    if (!g_curl_multi)
        return;

    while ((msg = curl_multi_info_read(g_curl_multi, &msgs_left)) != NULL)
    {
        struct t_slack_curl_xfer *x, *found = NULL;
        long http_code = 0;
        int ok = 0;

        if (msg->msg != CURLMSG_DONE)
            continue;

        for (x = g_curl_xfers; x; x = x->next)
        {
            if (x->easy == msg->easy_handle)
            {
                found = x;
                break;
            }
        }
        if (!found)
            continue;

        if (msg->data.result == CURLE_OK)
        {
            curl_easy_getinfo(found->easy, CURLINFO_RESPONSE_CODE, &http_code);
            ok = (http_code >= 200 && http_code < 300) ? 1 : 0;
        }

        if (found->fp)
        {
            fclose(found->fp);
            found->fp = NULL;
        }

        {
            struct curl_slist *hdr = NULL;
            curl_easy_getinfo(found->easy, CURLINFO_PRIVATE, (char **)&hdr);
            if (hdr)
                curl_slist_free_all(hdr);
            curl_easy_setopt(found->easy, CURLOPT_PRIVATE, NULL);
        }

        if (found->op == SLACK_CURL_BODY && found->body_cb)
        {
            char *body = found->resp_body;
            t_slack_curl_body_cb bcb = found->body_cb;
            void *ud = found->user_data;

            found->resp_body = NULL;
            found->body_cb = NULL;
            bcb(ud, ok, http_code, body ? body : "");
            free(body);
            slack_http_curl_unlink(found);
            slack_http_curl_xfer_free(found);
            continue;
        }

        if (found->op == SLACK_CURL_API && found->api_req)
        {
            struct t_slack_http_request *req = found->api_req;
            char *body = found->resp_body;
            char *hdrs = found->resp_hdrs;
            int limited, delay_sec;

            found->resp_body = NULL;
            found->resp_hdrs = NULL;
            found->api_req = NULL;

            req->in_flight = 0;
            if (g_inflight > 0)
                g_inflight--;

            limited = (http_code == 429) ||
                      (body && body[0] && slack_http_body_is_ratelimited(body));

            if (msg->data.result != CURLE_OK)
            {
                SLACK_WS_PRINTF(req->workspace,
                                "%sweeslack: HTTP transport error for %s: %s",
                                weechat_prefix("error"),
                                req->method ? req->method : "?",
                                found->errbuf[0] ? found->errbuf
                                                 : curl_easy_strerror(
                                                       msg->data.result));
                if (slack_http_requeue(req, req->tries * req->tries + 1))
                {
                    free(body);
                    free(hdrs);
                    slack_http_curl_unlink(found);
                    slack_http_curl_xfer_free(found);
                    continue;
                }
                if (req->callback)
                    req->callback(req->workspace, -1, NULL, req->user_data);
                free(body);
                free(hdrs);
                slack_http_request_free(req);
                slack_http_curl_unlink(found);
                slack_http_curl_xfer_free(found);
                continue;
            }

            if (limited)
            {
                delay_sec = slack_http_retry_after_from_headers(hdrs);
                slack_http_set_cooldown(delay_sec);
                if (req->flags & SLACK_HTTP_MARK)
                {
                    if (req->callback)
                        req->callback(req->workspace, -1, NULL,
                                      req->user_data);
                    free(body);
                    free(hdrs);
                    slack_http_request_free(req);
                    slack_http_curl_unlink(found);
                    slack_http_curl_xfer_free(found);
                    continue;
                }
                if (slack_http_requeue(req, delay_sec))
                {
                    SLACK_WS_PRINTF(req->workspace,
                                    "%sweeslack: rate limited on %s — "
                                    "cooldown %ds (try %d/%d)",
                                    weechat_prefix("network"),
                                    req->method ? req->method : "?",
                                    delay_sec, req->tries, req->max_tries);
                    free(body);
                    free(hdrs);
                    slack_http_curl_unlink(found);
                    slack_http_curl_xfer_free(found);
                    continue;
                }
                SLACK_WS_PRINTF(req->workspace,
                                "%sweeslack: rate limited on %s — giving up",
                                weechat_prefix("error"),
                                req->method ? req->method : "?");
                if (req->callback)
                    req->callback(req->workspace, 0,
                                  body && body[0] ? body : NULL,
                                  req->user_data);
                free(body);
                free(hdrs);
                slack_http_request_free(req);
                slack_http_curl_unlink(found);
                slack_http_curl_xfer_free(found);
                continue;
            }

            if (!body || !body[0])
            {
                if (slack_http_requeue(req, req->tries * req->tries + 1))
                {
                    free(body);
                    free(hdrs);
                    slack_http_curl_unlink(found);
                    slack_http_curl_xfer_free(found);
                    continue;
                }
                if (req->callback)
                    req->callback(req->workspace, -1, NULL, req->user_data);
                free(body);
                free(hdrs);
                slack_http_request_free(req);
                slack_http_curl_unlink(found);
                slack_http_curl_xfer_free(found);
                continue;
            }

            if (weechat_config_boolean(weeslack_config.record_events))
            {
                char line[1536];
                char body_snip[512];
                size_t blen, i;
                const char *meth = req->method ? req->method : "?";

                blen = strlen(body);
                if (blen > sizeof(body_snip) - 1)
                    blen = sizeof(body_snip) - 1;
                memcpy(body_snip, body, blen);
                body_snip[blen] = '\0';
                for (i = 0; i < blen; i++)
                {
                    if (body_snip[i] == '\n' || body_snip[i] == '\r' ||
                        body_snip[i] == '\t')
                        body_snip[i] = ' ';
                    else if (body_snip[i] == '"')
                        body_snip[i] = '\'';
                }
                snprintf(line, sizeof(line),
                         "{\"type\":\"http\",\"method\":\"%s\",\"status\":%ld,"
                         "\"body_len\":%zu,\"body\":\"%.480s%s\"}",
                         meth, http_code, strlen(body), body_snip,
                         (strlen(body) > 480) ? "…" : "");
                weeslack_record_json_ws(req->workspace, line);
            }

            if (req->callback)
                req->callback(req->workspace, 0, body, req->user_data);
            free(body);
            free(hdrs);
            slack_http_request_free(req);
            slack_http_curl_unlink(found);
            slack_http_curl_xfer_free(found);
            continue;
        }

        if (found->callback)
            found->callback(found->user_data, ok, http_code);

        slack_http_curl_unlink(found);
        slack_http_curl_xfer_free(found);
    }
}

static int
slack_http_curl_timer_cb(const void *pointer, void *data, int remaining_calls)
{
    (void)pointer;
    (void)data;
    (void)remaining_calls;

    if (!g_curl_multi)
        return WEECHAT_RC_OK;

    curl_multi_perform(g_curl_multi, &g_curl_still_running);
    slack_http_curl_check_done();
    /* Free multi slots may allow queued Web API jobs to start. */
    slack_http_queue_pump();

    if (g_curl_still_running <= 0 && !g_curl_xfers)
    {
        if (g_curl_timer)
        {
            weechat_unhook(g_curl_timer);
            g_curl_timer = NULL;
        }
    }
    return WEECHAT_RC_OK;
}

static void
slack_http_curl_ensure_multi(void)
{
    if (!g_curl_multi)
        g_curl_multi = curl_multi_init();
    if (!g_curl_timer && g_curl_multi)
    {
        g_curl_timer = weechat_hook_timer(
            SLACK_HTTP_CURL_TIMER_MS, 0, 0, &slack_http_curl_timer_cb,
            NULL, NULL);
    }
}

static int
slack_http_multi_xfer_count(void)
{
    struct t_slack_curl_xfer *x;
    int n = 0;

    for (x = g_curl_xfers; x; x = x->next)
        n++;
    return n;
}

static int
slack_http_curl_start(struct t_slack_curl_xfer *x)
{
    CURLMcode mc;

    if (!x || !x->easy)
        return 0;

    /*
     * Shared budget for API + binary (AGENTS: max concurrent libcurl).
     * Count handles already on the multi; do not add past the cap.
     */
    if (slack_http_multi_xfer_count() >= SLACK_HTTP_MAX_CONCURRENT)
        return 0;

    slack_http_curl_ensure_multi();
    if (!g_curl_multi)
        return 0;

    x->next = g_curl_xfers;
    g_curl_xfers = x;

    mc = curl_multi_add_handle(g_curl_multi, x->easy);
    if (mc != CURLM_OK)
    {
        slack_http_curl_unlink(x);
        return 0;
    }
    curl_multi_perform(g_curl_multi, &g_curl_still_running);
    slack_http_curl_check_done();
    return 1;
}

static size_t
slack_http_api_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct t_slack_curl_xfer *x = userdata;
    size_t n = size * nmemb;
    char *nb;

    if (!x || !ptr || n == 0)
        return n;
    if (x->resp_len + n + 1 > x->resp_cap)
    {
        size_t ncap = x->resp_cap ? x->resp_cap * 2
                                  : (size_t)SLACK_HTTP_API_RESP_INIT;
        while (ncap < x->resp_len + n + 1)
            ncap *= 2;
        if (ncap > (size_t)SLACK_HTTP_API_RESP_MAX)
            return 0;
        nb = realloc(x->resp_body, ncap);
        if (!nb)
            return 0;
        x->resp_body = nb;
        x->resp_cap = ncap;
    }
    memcpy(x->resp_body + x->resp_len, ptr, n);
    x->resp_len += n;
    x->resp_body[x->resp_len] = '\0';
    return n;
}

static size_t
slack_http_api_header_cb(char *buf, size_t size, size_t nitems, void *userdata)
{
    struct t_slack_curl_xfer *x = userdata;
    size_t n = size * nitems;
    char *nb;

    if (!x || !buf || n == 0)
        return n;
    if (x->hdrs_len + n + 1 > x->hdrs_cap)
    {
        size_t ncap = x->hdrs_cap ? x->hdrs_cap * 2
                                  : (size_t)SLACK_HTTP_API_HDRS_INIT;
        while (ncap < x->hdrs_len + n + 1)
            ncap *= 2;
        if (ncap > (size_t)SLACK_HTTP_API_HDRS_MAX)
            return n; /* drop extra headers */
        nb = realloc(x->resp_hdrs, ncap);
        if (!nb)
            return n;
        x->resp_hdrs = nb;
        x->hdrs_cap = ncap;
    }
    memcpy(x->resp_hdrs + x->hdrs_len, buf, n);
    x->hdrs_len += n;
    x->resp_hdrs[x->hdrs_len] = '\0';
    return n;
}

static int
slack_http_api_curl_start(struct t_slack_http_request *request)
{
    struct t_slack_curl_xfer *x;
    struct t_weeslack_workspace *ws;
    struct curl_slist *hdr = NULL;
    char auth[640];
    char cookie_h[640];
    int timeout_ms;

    if (!request || !request->workspace || !request->url)
        return 0;

    ws = request->workspace;
    x = calloc(1, sizeof(*x));
    if (!x)
        return 0;

    x->op = SLACK_CURL_API;
    x->api_req = request;
    x->url = strdup(request->url);
    x->easy = curl_easy_init();
    if (!x->easy || !x->url)
    {
        slack_http_curl_xfer_free(x);
        return 0;
    }

    timeout_ms = weechat_config_integer(weeslack_config.slack_timeout);
    if (timeout_ms < 5000)
        timeout_ms = SLACK_HTTP_TIMEOUT_FALLBACK_MS;

    curl_easy_setopt(x->easy, CURLOPT_URL, x->url);
    curl_easy_setopt(x->easy, CURLOPT_USERAGENT, "weeslack/" WEESLACK_VERSION);
    curl_easy_setopt(x->easy, CURLOPT_ERRORBUFFER, x->errbuf);
    curl_easy_setopt(x->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(x->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(x->easy, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(x->easy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(x->easy, CURLOPT_WRITEFUNCTION, slack_http_api_write_cb);
    curl_easy_setopt(x->easy, CURLOPT_WRITEDATA, x);
    curl_easy_setopt(x->easy, CURLOPT_HEADERFUNCTION, slack_http_api_header_cb);
    curl_easy_setopt(x->easy, CURLOPT_HEADERDATA, x);

    if (request->use_post && request->postfields)
    {
        curl_easy_setopt(x->easy, CURLOPT_POST, 1L);
        curl_easy_setopt(x->easy, CURLOPT_POSTFIELDS, request->postfields);
        curl_easy_setopt(x->easy, CURLOPT_POSTFIELDSIZE,
                         (long)strlen(request->postfields));
    }

    snprintf(auth, sizeof(auth), "Authorization: Bearer %s",
             ws->token ? ws->token : "");
    hdr = curl_slist_append(hdr, auth);
    hdr = curl_slist_append(hdr, "Content-Type: application/x-www-form-urlencoded");
    if (ws->cookie && ws->cookie[0])
    {
        snprintf(cookie_h, sizeof(cookie_h), "Cookie: %s%s",
                 (strncmp(ws->cookie, "d=", 2) == 0) ? "" : "d=",
                 ws->cookie);
        hdr = curl_slist_append(hdr, cookie_h);
    }
    curl_easy_setopt(x->easy, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(x->easy, CURLOPT_PRIVATE, hdr);
    slack_http_curl_apply_proxy_easy(x->easy);

    if (!slack_http_curl_start(x))
    {
        {
            struct curl_slist *h = NULL;
            curl_easy_getinfo(x->easy, CURLINFO_PRIVATE, (char **)&h);
            if (h)
                curl_slist_free_all(h);
        }
        x->api_req = NULL;
        slack_http_curl_xfer_free(x);
        return 0;
    }
    return 1;
}

static void
slack_http_api_curl_cancel(struct t_slack_http_request *request)
{
    struct t_slack_curl_xfer *x, *next;

    if (!request)
        return;

    for (x = g_curl_xfers; x; x = next)
    {
        next = x->next;
        if (x->op == SLACK_CURL_API && x->api_req == request)
        {
            struct curl_slist *hdr = NULL;

            x->api_req = NULL;
            x->callback = NULL;
            if (x->easy)
            {
                curl_easy_getinfo(x->easy, CURLINFO_PRIVATE, (char **)&hdr);
                if (hdr)
                    curl_slist_free_all(hdr);
                curl_easy_setopt(x->easy, CURLOPT_PRIVATE, NULL);
            }
            slack_http_curl_unlink(x);
            slack_http_curl_xfer_free(x);
            return;
        }
    }
}

int
slack_http_curl_put_file(const char *url, const char *file_path,
                         const char *content_type,
                         t_slack_curl_done_cb callback,
                         void *user_data)
{
    struct t_slack_curl_xfer *x;
    struct stat st;
    FILE *fp;
    int timeout_ms;

    if (!url || !url[0] || !file_path || !file_path[0] || !callback)
        return 0;
    if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;

    fp = fopen(file_path, "rb");
    if (!fp)
        return 0;

    x = calloc(1, sizeof(*x));
    if (!x)
    {
        fclose(fp);
        return 0;
    }

    x->op = SLACK_CURL_PUT;
    x->fp = fp;
    x->url = strdup(url);
    x->path = strdup(file_path);
    x->content_type = (content_type && content_type[0])
                          ? strdup(content_type) : NULL;
    x->callback = callback;
    x->user_data = user_data;
    x->easy = curl_easy_init();
    if (!x->easy || !x->url || !x->path)
    {
        slack_http_curl_xfer_free(x);
        return 0;
    }

    timeout_ms = weechat_config_integer(weeslack_config.slack_timeout);
    if (timeout_ms < 5000)
        timeout_ms = 60000; /* uploads: allow longer than default API fallback */

    curl_easy_setopt(x->easy, CURLOPT_URL, x->url);
    /* Slack getUploadURLExternal: HTTP POST raw body (was curl -X POST -T). */
    curl_easy_setopt(x->easy, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(x->easy, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(x->easy, CURLOPT_READDATA, x->fp);
    curl_easy_setopt(x->easy, CURLOPT_INFILESIZE_LARGE, (curl_off_t)st.st_size);
    curl_easy_setopt(x->easy, CURLOPT_ERRORBUFFER, x->errbuf);
    curl_easy_setopt(x->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(x->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(x->easy, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(x->easy, CURLOPT_USERAGENT, "weeslack/" WEESLACK_VERSION);
    if (x->content_type)
    {
        struct curl_slist *hdr = NULL;
        char ct[256];
        snprintf(ct, sizeof(ct), "Content-Type: %s", x->content_type);
        hdr = curl_slist_append(hdr, ct);
        curl_easy_setopt(x->easy, CURLOPT_HTTPHEADER, hdr);
        /* slist freed with easy via CURLOPT_PRIVATE or we leak small;
         * attach as private for cleanup — store in content_type freed only.
         * Use curl_easy_setopt PRIVATE for the slist. */
        curl_easy_setopt(x->easy, CURLOPT_PRIVATE, hdr);
    }
    slack_http_curl_apply_proxy_easy(x->easy);

    if (!slack_http_curl_start(x))
    {
        if (callback)
            callback(user_data, 0, 0);
        /* x not linked; free carefully — callback already owns user_data */
        {
            struct curl_slist *hdr = NULL;
            curl_easy_getinfo(x->easy, CURLINFO_PRIVATE, (char **)&hdr);
            if (hdr)
                curl_slist_free_all(hdr);
        }
        x->callback = NULL;
        slack_http_curl_xfer_free(x);
        return 0;
    }
    return 1;
}

int
slack_http_curl_get_file(const char *url, const char *path,
                         const char *authorization,
                         const char *cookie,
                         t_slack_curl_done_cb callback,
                         void *user_data)
{
    struct t_slack_curl_xfer *x;
    FILE *fp;
    char dir[640];
    char *slash;
    int timeout_ms;
    struct curl_slist *hdr = NULL;

    if (!url || !url[0] || !path || !path[0] || !callback)
        return 0;

    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash && slash != dir)
    {
        *slash = '\0';
        weechat_mkdir_parents(dir, 0755);
    }

    fp = fopen(path, "wb");
    if (!fp)
        return 0;

    x = calloc(1, sizeof(*x));
    if (!x)
    {
        fclose(fp);
        return 0;
    }

    x->op = SLACK_CURL_GET;
    x->fp = fp;
    x->url = strdup(url);
    x->path = strdup(path);
    x->authorization = (authorization && authorization[0])
                           ? strdup(authorization) : NULL;
    x->cookie = (cookie && cookie[0]) ? strdup(cookie) : NULL;
    x->callback = callback;
    x->user_data = user_data;
    x->easy = curl_easy_init();
    if (!x->easy || !x->url || !x->path)
    {
        slack_http_curl_xfer_free(x);
        return 0;
    }

    timeout_ms = weechat_config_integer(weeslack_config.slack_timeout);
    if (timeout_ms < 5000)
        timeout_ms = SLACK_HTTP_TIMEOUT_FALLBACK_MS;

    curl_easy_setopt(x->easy, CURLOPT_URL, x->url);
    curl_easy_setopt(x->easy, CURLOPT_WRITEDATA, x->fp);
    curl_easy_setopt(x->easy, CURLOPT_WRITEFUNCTION, NULL); /* default fwrite */
    curl_easy_setopt(x->easy, CURLOPT_ERRORBUFFER, x->errbuf);
    curl_easy_setopt(x->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(x->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(x->easy, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(x->easy, CURLOPT_USERAGENT, "weeslack/" WEESLACK_VERSION);

    if (x->authorization)
    {
        char ah[640];
        snprintf(ah, sizeof(ah), "Authorization: %s", x->authorization);
        hdr = curl_slist_append(hdr, ah);
    }
    if (x->cookie)
    {
        char ch[640];
        /* Accept raw cookie or full "Cookie: …" */
        if (weechat_strncasecmp(x->cookie, "Cookie:", 7) == 0)
            snprintf(ch, sizeof(ch), "%s", x->cookie);
        else
            snprintf(ch, sizeof(ch), "Cookie: %s", x->cookie);
        hdr = curl_slist_append(hdr, ch);
    }
    if (hdr)
    {
        curl_easy_setopt(x->easy, CURLOPT_HTTPHEADER, hdr);
        curl_easy_setopt(x->easy, CURLOPT_PRIVATE, hdr);
    }
    slack_http_curl_apply_proxy_easy(x->easy);

    if (!slack_http_curl_start(x))
    {
        if (callback)
            callback(user_data, 0, 0);
        {
            struct curl_slist *h = NULL;
            curl_easy_getinfo(x->easy, CURLINFO_PRIVATE, (char **)&h);
            if (h)
                curl_slist_free_all(h);
        }
        x->callback = NULL;
        slack_http_curl_xfer_free(x);
        return 0;
    }
    return 1;
}

int
slack_http_curl_get_body(const char *url,
                         t_slack_curl_body_cb callback,
                         void *user_data)
{
    struct t_slack_curl_xfer *x;
    int timeout_ms;

    if (!url || !url[0] || !callback)
        return 0;

    x = calloc(1, sizeof(*x));
    if (!x)
        return 0;

    x->op = SLACK_CURL_BODY;
    x->url = strdup(url);
    x->body_cb = callback;
    x->user_data = user_data;
    x->easy = curl_easy_init();
    if (!x->easy || !x->url)
    {
        slack_http_curl_xfer_free(x);
        return 0;
    }

    timeout_ms = weechat_config_integer(weeslack_config.slack_timeout);
    if (timeout_ms < 5000)
        timeout_ms = SLACK_HTTP_TIMEOUT_FALLBACK_MS;

    curl_easy_setopt(x->easy, CURLOPT_URL, x->url);
    curl_easy_setopt(x->easy, CURLOPT_USERAGENT, "weeslack/" WEESLACK_VERSION);
    curl_easy_setopt(x->easy, CURLOPT_ERRORBUFFER, x->errbuf);
    curl_easy_setopt(x->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(x->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(x->easy, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(x->easy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(x->easy, CURLOPT_WRITEFUNCTION, slack_http_api_write_cb);
    curl_easy_setopt(x->easy, CURLOPT_WRITEDATA, x);
    slack_http_curl_apply_proxy_easy(x->easy);

    if (!slack_http_curl_start(x))
    {
        x->body_cb = NULL;
        slack_http_curl_xfer_free(x);
        return 0;
    }
    return 1;
}

void
slack_http_curl_multi_shutdown(void)
{
    struct t_slack_curl_xfer *x, *next;
    struct curl_slist *hdr;

    if (g_curl_timer)
    {
        weechat_unhook(g_curl_timer);
        g_curl_timer = NULL;
    }

    for (x = g_curl_xfers; x; x = next)
    {
        next = x->next;
        hdr = NULL;
        if (x->easy)
            curl_easy_getinfo(x->easy, CURLINFO_PRIVATE, (char **)&hdr);
        if (hdr)
            curl_slist_free_all(hdr);
        /* Do not invoke callbacks on unload — user_data may be mid-free. */
        x->callback = NULL;
        x->body_cb = NULL;
        x->api_req = NULL;
        slack_http_curl_xfer_free(x);
    }
    g_curl_xfers = NULL;

    if (g_curl_multi)
    {
        curl_multi_cleanup(g_curl_multi);
        g_curl_multi = NULL;
    }
    g_curl_still_running = 0;
    curl_global_cleanup();
}
