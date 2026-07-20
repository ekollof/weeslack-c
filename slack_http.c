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

#include "weeslack.h"
#include "slack_http.h"
#include "slack_ws.h"

struct t_slack_http_request *slack_http_requests = NULL;

#define SLACK_HTTP_MAX_TRIES         3
#define SLACK_HTTP_MAX_CONCURRENT    2
#define SLACK_HTTP_TICK_MS           100
#define SLACK_HTTP_RETRY_DEFAULT_SEC 8
#define SLACK_HTTP_RETRY_MAX_SEC     90

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
static void
slack_http_apply_proxy(struct t_hashtable *options)
{
    struct t_config_option *opt;
    const char *name, *type, *address, *username, *password;
    int port;
    char proxy_url[512];
    char key[128];
    const char *scheme = "http";

    if (!options)
        return;

    opt = weechat_config_get("weechat.network.proxy_curl");
    if (!opt)
        return;
    name = weechat_config_string(opt);
    if (!name || !name[0])
        return;

    snprintf(key, sizeof(key), "weechat.proxy.%s.address", name);
    opt = weechat_config_get(key);
    address = opt ? weechat_config_string(opt) : NULL;
    if (!address || !address[0])
        return;

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

    weechat_hashtable_set(options, "proxy", proxy_url);
}

static struct t_hashtable *
slack_http_build_options(struct t_weeslack_workspace *workspace,
                         struct t_slack_http_request *request)
{
    struct t_hashtable *options;
    char httpheader[640];

    options = weechat_hashtable_new(32, WEECHAT_HASHTABLE_STRING,
                                    WEECHAT_HASHTABLE_STRING, NULL, NULL);
    if (!options)
        return NULL;

    weechat_hashtable_set(options, "useragent", "weeslack-c");
    /* Do not set curl "header"=1: that prepends HTTP headers to the body in
     * output["output"], which breaks JSON parse. Response headers are already
     * available separately as output["headers"] (for Retry-After, etc.). */

    if (workspace->cookie && workspace->cookie[0])
    {
        snprintf(httpheader, sizeof(httpheader),
                 "Authorization: Bearer %s\n"
                 "Content-Type: application/x-www-form-urlencoded\n"
                 "Cookie: %s%s",
                 workspace->token,
                 (strncmp(workspace->cookie, "d=", 2) == 0) ? "" : "d=",
                 workspace->cookie);
    }
    else
    {
        snprintf(httpheader, sizeof(httpheader),
                 "Authorization: Bearer %s\n"
                 "Content-Type: application/x-www-form-urlencoded",
                 workspace->token);
    }
    weechat_hashtable_set(options, "httpheader", httpheader);
    /* WeeChat curl option constants are lowercase (see plugin API docs). */
    weechat_hashtable_set(options, "ipresolve", "v4");
    slack_http_apply_proxy(options);

    if (request && request->use_post && request->postfields)
    {
        weechat_hashtable_set(options, "post", "1");
        weechat_hashtable_set(options, "postfields", request->postfields);
    }
    return options;
}

static int slack_http_url_cb(const void *pointer, void *data,
                             const char *url, struct t_hashtable *options,
                             struct t_hashtable *output);

static void
slack_http_start_request(struct t_slack_http_request *request)
{
    struct t_hashtable *opts;

    if (!request || !request->workspace || !request->url)
    {
        if (request)
            slack_http_request_free(request);
        return;
    }

    opts = slack_http_build_options(request->workspace, request);
    if (!opts)
    {
        if (request->callback)
            request->callback(request->workspace, -1, NULL, request->user_data);
        slack_http_request_free(request);
        return;
    }

    request->tries++;
    request->in_flight = 1;
    g_inflight++;

    request->hook = weechat_hook_url(
        request->url, opts, 30000,
        &slack_http_url_cb, request, NULL);
    weechat_hashtable_free(opts);

    if (!request->hook)
    {
        request->in_flight = 0;
        g_inflight--;
        SLACK_WS_PRINTF(request->workspace,
                        "%sweeslack: hook_url failed for %s",
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

    while (g_inflight < SLACK_HTTP_MAX_CONCURRENT)
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

void
slack_http_queue_init(void)
{
    if (g_queue_timer)
        return;
    g_queue_timer = weechat_hook_timer(
        SLACK_HTTP_TICK_MS, 0, 0,
        &slack_http_queue_timer_cb, NULL, NULL);
}

void
slack_http_queue_shutdown(void)
{
    struct t_slack_http_request *req, *next;

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
slack_http_retry_after_sec(struct t_hashtable *output)
{
    const char *ra, *p;
    int sec;

    if (!output)
        return SLACK_HTTP_RETRY_DEFAULT_SEC;

    ra = weechat_hashtable_get(output, "headers");
    if (!ra || !ra[0])
        ra = weechat_hashtable_get(output, "header");
    if (!ra || !ra[0])
        ra = weechat_hashtable_get(output, "retry-after");
    if (!ra || !ra[0])
        ra = weechat_hashtable_get(output, "Retry-After");

    if (ra && ra[0])
    {
        p = slack_http_find_retry_after(ra);
        if (!p)
            p = ra;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        sec = atoi(p);
        if (sec > 0)
        {
            if (sec > SLACK_HTTP_RETRY_MAX_SEC)
                sec = SLACK_HTTP_RETRY_MAX_SEC;
            return sec;
        }
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

static int
slack_http_url_cb(const void *pointer, void *data,
                  const char *url, struct t_hashtable *options,
                  struct t_hashtable *output)
{
    struct t_slack_http_request *request =
        (struct t_slack_http_request *)pointer;
    const char *error, *body, *response_code_str;
    int http_status, limited, delay_sec;

    (void) url;
    (void) options;
    (void) data;

    if (!request)
        return WEECHAT_RC_OK;

    request->hook = NULL;
    request->in_flight = 0;
    if (g_inflight > 0)
        g_inflight--;

    if (!output)
    {
        if (slack_http_requeue(request, request->tries * request->tries + 1))
            return WEECHAT_RC_OK;
        if (request->callback)
            request->callback(request->workspace, -1, NULL, request->user_data);
        slack_http_request_free(request);
        return WEECHAT_RC_OK;
    }

    error = weechat_hashtable_get(output, "error");
    body = weechat_hashtable_get(output, "output");
    response_code_str = weechat_hashtable_get(output, "response_code");
    http_status = response_code_str ? atoi(response_code_str) : 0;
    limited = (http_status == 429) ||
              (body && body[0] && slack_http_body_is_ratelimited(body));

    if (error && error[0] && !limited)
    {
        SLACK_WS_PRINTF(request->workspace, "%sweeslack: HTTP error for %s: %s",
                        weechat_prefix("error"),
                        request->method ? request->method : "request",
                        error);
    }

    if (limited)
    {
        delay_sec = slack_http_retry_after_sec(output);
        slack_http_set_cooldown(delay_sec);

        if (request->flags & SLACK_HTTP_MARK)
        {
            if (request->callback)
                request->callback(request->workspace, -1, NULL,
                                  request->user_data);
            slack_http_request_free(request);
            return WEECHAT_RC_OK;
        }

        if (slack_http_requeue(request, delay_sec))
        {
            SLACK_WS_PRINTF(request->workspace,
                            "%sweeslack: rate limited on %s — cooldown %ds "
                            "(try %d/%d)",
                            weechat_prefix("network"),
                            request->method ? request->method : "?",
                            delay_sec,
                            request->tries,
                            request->max_tries);
            return WEECHAT_RC_OK;
        }

        SLACK_WS_PRINTF(request->workspace,
                        "%sweeslack: rate limited on %s — giving up",
                        weechat_prefix("error"),
                        request->method ? request->method : "?");
        if (request->callback)
            request->callback(request->workspace, 0,
                              body && body[0] ? body : NULL,
                              request->user_data);
        slack_http_request_free(request);
        return WEECHAT_RC_OK;
    }

    if (!body || !body[0])
    {
        if (slack_http_requeue(request, request->tries * request->tries + 1))
            return WEECHAT_RC_OK;
        if (request->callback)
            request->callback(request->workspace, -1, NULL, request->user_data);
        slack_http_request_free(request);
        return WEECHAT_RC_OK;
    }

    if (request->callback)
        request->callback(request->workspace, 0, body, request->user_data);

    slack_http_request_free(request);
    return WEECHAT_RC_OK;
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

    if (request->hook)
    {
        weechat_unhook(request->hook);
        request->hook = NULL;
        if (request->in_flight && g_inflight > 0)
            g_inflight--;
        request->in_flight = 0;
    }

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
