#ifndef SLACK_HTTP_H
#define SLACK_HTTP_H

#include <weechat/weechat-plugin.h>
#include <json-c/json.h>
#include <time.h>

struct t_weeslack_workspace;

/* Flags for slack_http_request_new_flags */
#define SLACK_HTTP_SLOW   0x01  /* history/members: slow lane (~1 promote/sec) */
#define SLACK_HTTP_MARK   0x02  /* mark-as-read: droppable under cooldown */

struct t_slack_http_request
{
    char *id;
    char *url;
    char *method;
    char *postfields;
    int use_post;
    int flags;
    int tries;
    int max_tries;
    time_t retry_not_before;
    int in_flight;
    struct t_weeslack_workspace *workspace;
    void (*callback)(struct t_weeslack_workspace *workspace,
                     int return_code, const char *output,
                     void *user_data);
    void *user_data;
    struct t_hook *hook;

    /* global registry (cancel on disconnect) */
    struct t_slack_http_request *reg_next;
    struct t_slack_http_request *reg_prev;

    /* wait queues (fast / slow) */
    struct t_slack_http_request *q_next;
    struct t_slack_http_request *q_prev;
};

extern struct t_slack_http_request *slack_http_request_new(
    struct t_weeslack_workspace *workspace,
    const char *method,
    struct json_object *params,
    void (*callback)(struct t_weeslack_workspace *workspace,
                     int return_code, const char *output,
                     void *user_data),
    void *user_data);

extern struct t_slack_http_request *slack_http_request_new_flags(
    struct t_weeslack_workspace *workspace,
    const char *method,
    struct json_object *params,
    int flags,
    void (*callback)(struct t_weeslack_workspace *workspace,
                     int return_code, const char *output,
                     void *user_data),
    void *user_data);

extern void slack_http_request_free(struct t_slack_http_request *request);
extern void slack_http_requests_cancel(struct t_weeslack_workspace *workspace);

extern void slack_http_set_cooldown(int seconds);
extern int slack_http_on_cooldown(void);

extern struct json_object *slack_json_decode(const char *json);
extern int slack_http_body_is_ratelimited(const char *body);

extern void slack_http_queue_init(void);
extern void slack_http_queue_shutdown(void);

/* Fill buf with queue/cooldown summary (for /cslack queue). Returns buf. */
extern char *slack_http_queue_status(char *buf, size_t buflen);

/* Proxy URL from WeeChat globals (caller frees). Used by hook_url + libcurl multi. */
extern char *slack_http_get_proxy_url(void);

/*
 * libcurl multi (async, non-blocking via WeeChat timer):
 * binary PUT for upload, GET for emoji/image cache.
 * callback: ok=1 on HTTP 2xx, else 0. http_code may be 0 on transport error.
 */
typedef void (*t_slack_curl_done_cb)(void *user_data, int ok, long http_code);

/* PUT local file to url (Slack upload_url). Optional Content-Type. */
extern int slack_http_curl_put_file(const char *url, const char *file_path,
                                    const char *content_type,
                                    t_slack_curl_done_cb callback,
                                    void *user_data);

/* GET url → path (creates parent dirs).
 * authorization: "Bearer …" or NULL (public CDN).
 * cookie: raw cookie value (optional "d=…"); NULL if none. */
extern int slack_http_curl_get_file(const char *url, const char *path,
                                    const char *authorization,
                                    const char *cookie,
                                    t_slack_curl_done_cb callback,
                                    void *user_data);

/* Cancel all multi transfers (plugin end). */
extern void slack_http_curl_multi_shutdown(void);

#endif
