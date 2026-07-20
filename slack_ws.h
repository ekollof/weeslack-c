#ifndef SLACK_WS_H
#define SLACK_WS_H

#include <weechat/weechat-plugin.h>
#include <json-c/json.h>
#include <openssl/ssl.h>

struct t_weeslack_workspace;

struct t_slack_ws
{
    struct t_weeslack_workspace *workspace;
    int fd;
    int connected;
    int connecting;
    int tls_handshake_done;
    int handshake_done;
    int ping_interval;
    int reconnect_delay;
    int max_reconnect_delay;
    struct t_hook *hook_fd;
    struct t_hook *hook_ping;
    struct t_hook *hook_reconnect;
    char *recv_buffer;
    size_t recv_buffer_len;
    SSL *ssl;
    SSL_CTX *ssl_ctx;
    struct t_slack_ws *next;
    struct t_slack_ws *prev;
};

extern struct t_slack_ws *slack_ws_new(struct t_weeslack_workspace *workspace);
extern void slack_ws_free(struct t_slack_ws *ws);
extern int slack_ws_connect(struct t_slack_ws *ws, const char *url);
extern int slack_ws_disconnect(struct t_slack_ws *ws);
extern int slack_ws_send(struct t_slack_ws *ws, struct json_object *msg);
extern void slack_ws_reconnect(struct t_slack_ws *ws);

#endif
