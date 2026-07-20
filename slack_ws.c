#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/rand.h>

#include "weeslack.h"
#include "slack_ws.h"
#include "slack_http.h"
#include "slack_event.h"

struct t_slack_ws *slack_ws_list = NULL;

static SSL_CTX *g_ssl_ctx = NULL;

/* Parse wss://host[:port]/path from a Slack RTM URL. Returns 0 on success. */
static int
slack_ws_parse_url(const char *url, char *host, size_t host_size,
                   char *path, size_t path_size, int *port)
{
    const char *p;
    const char *slash;
    const char *colon;
    size_t host_len;

    if (!url || !host || !path || !port)
        return -1;

    host[0] = '\0';
    path[0] = '\0';
    *port = 443;

    if (strncmp(url, "wss://", 6) == 0)
    {
        p = url + 6;
        *port = 443;
    }
    else if (strncmp(url, "ws://", 5) == 0)
    {
        p = url + 5;
        *port = 80;
    }
    else
    {
        return -1;
    }

    slash = strchr(p, '/');
    if (!slash)
        return -1;

    host_len = (size_t)(slash - p);
    if (host_len == 0 || host_len >= host_size)
        return -1;

    /* host may be host:port */
    colon = memchr(p, ':', host_len);
    if (colon)
    {
        size_t hlen = (size_t)(colon - p);
        if (hlen == 0 || hlen >= host_size)
            return -1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0)
            *port = 443;
    }
    else
    {
        memcpy(host, p, host_len);
        host[host_len] = '\0';
    }

    if (strlen(slash) >= path_size)
        return -1;
    snprintf(path, path_size, "%s", slash);
    return 0;
}

struct t_slack_ws *
slack_ws_new(struct t_weeslack_workspace *workspace)
{
    struct t_slack_ws *ws;

    ws = calloc(1, sizeof(struct t_slack_ws));
    if (!ws)
        return NULL;

    ws->workspace = workspace;
    ws->fd = -1;
    ws->connected = 0;
    ws->connecting = 0;
    ws->tls_handshake_done = 0;
    ws->handshake_done = 0;
    ws->ping_interval = 30000;
    ws->reconnect_delay = 1;
    ws->max_reconnect_delay = 60;
    ws->ssl = NULL;
    ws->ssl_ctx = NULL;
    ws->recv_buffer = NULL;
    ws->recv_buffer_len = 0;

    ws->next = slack_ws_list;
    ws->prev = NULL;
    if (slack_ws_list)
        slack_ws_list->prev = ws;
    slack_ws_list = ws;

    return ws;
}

void
slack_ws_free(struct t_slack_ws *ws)
{
    if (!ws)
        return;

    slack_ws_disconnect(ws);

    if (ws->prev)
        ws->prev->next = ws->next;
    else
        slack_ws_list = ws->next;

    if (ws->next)
        ws->next->prev = ws->prev;

    free(ws->recv_buffer);
    free(ws);
}

static void
slack_ws_send_raw(struct t_slack_ws *ws, const void *data, size_t len)
{
    if (ws->ssl)
        SSL_write(ws->ssl, data, (int)len);
    else
        send(ws->fd, data, len, 0);
}

/* RFC 6455: client-to-server frames MUST be masked. */
static int
slack_ws_send_frame(struct t_slack_ws *ws, unsigned char opcode,
                    const void *payload, size_t len)
{
    unsigned char header[14];
    unsigned char mask[4];
    size_t header_len;
    size_t i;
    unsigned char *masked = NULL;

    if (!ws || (!ws->ssl && ws->fd < 0))
        return -1;

    if (RAND_bytes(mask, 4) != 1)
    {
        /* weak fallback if RAND fails */
        mask[0] = 0x12;
        mask[1] = 0x34;
        mask[2] = 0x56;
        mask[3] = 0x78;
    }

    header[0] = (unsigned char)(0x80 | (opcode & 0x0F));
    if (len < 126)
    {
        header[1] = (unsigned char)(0x80 | len);
        header_len = 2;
    }
    else if (len < 65536)
    {
        header[1] = 0x80 | 126;
        header[2] = (unsigned char)((len >> 8) & 0xFF);
        header[3] = (unsigned char)(len & 0xFF);
        header_len = 4;
    }
    else
    {
        header[1] = 0x80 | 127;
        for (i = 0; i < 8; i++)
            header[2 + i] = (unsigned char)((len >> (56 - i * 8)) & 0xFF);
        header_len = 10;
    }

    memcpy(header + header_len, mask, 4);
    header_len += 4;

    if (len > 0)
    {
        masked = malloc(len);
        if (!masked)
            return -1;
        for (i = 0; i < len; i++)
            masked[i] = ((const unsigned char *)payload)[i] ^ mask[i % 4];
    }

    slack_ws_send_raw(ws, header, header_len);
    if (masked)
    {
        slack_ws_send_raw(ws, masked, len);
        free(masked);
    }

    return 0;
}

static ssize_t
slack_ws_recv_raw(struct t_slack_ws *ws, void *buf, size_t len)
{
    if (ws->ssl)
        return SSL_read(ws->ssl, buf, (int)len);
    return recv(ws->fd, buf, len, 0);
}

static void
slack_ws_handle_frame(struct t_slack_ws *ws, const char *frame, size_t len)
{
    (void) len;

    struct json_object *json;

    json = slack_json_decode(frame);
    if (json)
    {
        slack_event_handle(ws->workspace, json);
        json_object_put(json);
    }
}

/* Parse one or more complete WS frames from ws->recv_buffer. */
static void
slack_ws_consume_frames(struct t_slack_ws *ws)
{
    while (ws->recv_buffer_len >= 2)
    {
        unsigned char *buf = (unsigned char *)ws->recv_buffer;
        unsigned char opcode = buf[0] & 0x0F;
        int fin = (buf[0] & 0x80) != 0;
        size_t payload_offset = 2;
        size_t payload_len = buf[1] & 0x7F;
        int masked = (buf[1] & 0x80) != 0;

        if (payload_len == 126)
        {
            if (ws->recv_buffer_len < 4)
                return;
            payload_len = ((size_t)buf[2] << 8) | (size_t)buf[3];
            payload_offset = 4;
        }
        else if (payload_len == 127)
        {
            if (ws->recv_buffer_len < 10)
                return;
            payload_len = 0;
            for (int i = 0; i < 8; i++)
                payload_len = (payload_len << 8) | (size_t)buf[2 + i];
            payload_offset = 10;
        }

        if (masked)
            payload_offset += 4;

        if (ws->recv_buffer_len < payload_offset + payload_len)
            return;

        if (opcode == 0x8)
        {
            /* close frame */
            ws->connected = 0;
            slack_ws_reconnect(ws);
            return;
        }

        if (opcode == 0x9)
        {
            /* ping → pong with same payload */
            slack_ws_send_frame(ws, 0xA,
                                ws->recv_buffer + payload_offset, payload_len);
        }
        else if (opcode == 0xA)
        {
            /* pong — ignore */
        }
        else if ((opcode == 0x1 || opcode == 0x0) && fin)
        {
            char *text = malloc(payload_len + 1);
            if (text)
            {
                memcpy(text, ws->recv_buffer + payload_offset, payload_len);
                text[payload_len] = '\0';
                slack_ws_handle_frame(ws, text, payload_len);
                free(text);
            }
        }

        /* shift remaining bytes */
        {
            size_t frame_total = payload_offset + payload_len;
            size_t remain = ws->recv_buffer_len - frame_total;
            if (remain > 0)
                memmove(ws->recv_buffer, ws->recv_buffer + frame_total, remain);
            ws->recv_buffer_len = remain;
            if (remain == 0)
            {
                free(ws->recv_buffer);
                ws->recv_buffer = NULL;
            }
        }
    }
}

static void
slack_ws_send_handshake(struct t_slack_ws *ws);

static void
slack_ws_process_handshake(struct t_slack_ws *ws, const char *data, size_t len)
{
    size_t i;

    /* append to buffer first so multi-packet HTTP responses work */
    {
        size_t new_len = ws->recv_buffer_len + len;
        char *new_buf = realloc(ws->recv_buffer, new_len + 1);
        if (!new_buf)
            return;
        ws->recv_buffer = new_buf;
        memcpy(ws->recv_buffer + ws->recv_buffer_len, data, len);
        ws->recv_buffer_len = new_len;
        ws->recv_buffer[new_len] = '\0';
    }

    for (i = 0; i + 3 < ws->recv_buffer_len; i++)
    {
        if (ws->recv_buffer[i] == '\r' && ws->recv_buffer[i + 1] == '\n' &&
            ws->recv_buffer[i + 2] == '\r' && ws->recv_buffer[i + 3] == '\n')
        {
            /* Require 101 Switching Protocols */
            if (ws->recv_buffer_len < 12 ||
                strncmp(ws->recv_buffer, "HTTP/1.1 101", 12) != 0)
            {
                char preview[120];
                size_t plen = ws->recv_buffer_len < 119 ? ws->recv_buffer_len : 119;
                memcpy(preview, ws->recv_buffer, plen);
                preview[plen] = '\0';
                SLACK_WS_PRINTF(ws->workspace,
                                "%sweeslack: websocket handshake rejected: %.80s",
                                weechat_prefix("error"), preview);
                ws->connected = 0;
                slack_ws_reconnect(ws);
                return;
            }

            ws->handshake_done = 1;

            size_t remaining = ws->recv_buffer_len - (i + 4);
            if (remaining > 0)
                memmove(ws->recv_buffer, ws->recv_buffer + i + 4, remaining);
            ws->recv_buffer_len = remaining;
            if (remaining == 0)
            {
                free(ws->recv_buffer);
                ws->recv_buffer = NULL;
            }
            else
            {
                ws->recv_buffer[remaining] = '\0';
                slack_ws_consume_frames(ws);
            }
            return;
        }
    }
}

static int
slack_ws_read_cb(const void *pointer, void *data, int fd)
{
    (void) data;
    (void) fd;

    struct t_slack_ws *ws = (struct t_slack_ws *)pointer;
    char buf[8192];
    ssize_t n;

    if (!ws || !ws->connected)
        return WEECHAT_RC_OK;

    if (!ws->tls_handshake_done)
    {
        int rc = SSL_do_handshake(ws->ssl);
        if (rc != 1)
        {
            int ssl_err = SSL_get_error(ws->ssl, rc);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE)
                return WEECHAT_RC_OK;

            SLACK_WS_PRINTF(ws->workspace,
                            "%sweeslack: TLS handshake failed for %s (ssl_err=%d)",
                            weechat_prefix("error"),
                            ws->workspace->name, ssl_err);
            ws->connected = 0;
            slack_ws_reconnect(ws);
            return WEECHAT_RC_OK;
        }

        ws->tls_handshake_done = 1;
        SLACK_WS_PRINTF(ws->workspace, "%sweeslack: TLS handshake complete for %s",
                        weechat_prefix("network"), ws->workspace->name);

        slack_ws_send_handshake(ws);
        return WEECHAT_RC_OK;
    }

    n = slack_ws_recv_raw(ws, buf, sizeof(buf));
    if (n <= 0)
    {
        int ssl_err = ws->ssl ? SSL_get_error(ws->ssl, (int)n) : 0;
        if (ws->ssl && (ssl_err == SSL_ERROR_WANT_READ ||
                        ssl_err == SSL_ERROR_WANT_WRITE))
            return WEECHAT_RC_OK;

        SLACK_WS_PRINTF(ws->workspace, "%sweeslack: connection lost to %s",
                        weechat_prefix("error"), ws->workspace->name);
        ws->connected = 0;
        slack_ws_reconnect(ws);
        return WEECHAT_RC_OK;
    }

    if (!ws->handshake_done)
    {
        slack_ws_process_handshake(ws, buf, (size_t)n);
        if (ws->handshake_done)
        {
            SLACK_WS_PRINTF(ws->workspace,
                            "%sweeslack: websocket handshake complete for %s",
                            weechat_prefix("network"), ws->workspace->name);
        }
        return WEECHAT_RC_OK;
    }

    /* append application data and parse frames */
    {
        size_t new_len = ws->recv_buffer_len + (size_t)n;
        char *new_buf = realloc(ws->recv_buffer, new_len + 1);
        if (!new_buf)
            return WEECHAT_RC_OK;
        ws->recv_buffer = new_buf;
        memcpy(ws->recv_buffer + ws->recv_buffer_len, buf, (size_t)n);
        ws->recv_buffer_len = new_len;
        ws->recv_buffer[new_len] = '\0';
        slack_ws_consume_frames(ws);
    }

    return WEECHAT_RC_OK;
}

static int
slack_ws_ping_cb(const void *pointer, void *data,
                 int remaining_calls)
{
    (void) data;
    (void) remaining_calls;

    struct t_slack_ws *ws = (struct t_slack_ws *)pointer;

    if (!ws || !ws->connected || !ws->handshake_done)
        return WEECHAT_RC_OK;

    slack_ws_send_frame(ws, 0x9, NULL, 0);

    return WEECHAT_RC_OK;
}

static int
slack_ws_reconnect_cb(const void *pointer, void *data,
                      int remaining_calls)
{
    (void) data;
    (void) remaining_calls;

    struct t_slack_ws *ws = (struct t_slack_ws *)pointer;

    if (!ws)
        return WEECHAT_RC_OK;

    ws->hook_reconnect = NULL;

    if (ws->workspace->ws_url)
        slack_ws_connect(ws, ws->workspace->ws_url);

    return WEECHAT_RC_OK;
}

static void
slack_ws_send_handshake(struct t_slack_ws *ws)
{
    char host[256];
    char path[1024];
    int port;
    char handshake[2048];

    if (!ws->workspace->ws_url)
        return;

    if (slack_ws_parse_url(ws->workspace->ws_url, host, sizeof(host),
                           path, sizeof(path), &port) != 0)
    {
        SLACK_WS_PRINTF(ws->workspace,
                        "%sweeslack: invalid websocket URL: %s",
                        weechat_prefix("error"), ws->workspace->ws_url);
        return;
    }

    /* Fixed test key is fine for Slack; server does not validate challenge. */
    if (port == 443 || port == 80)
    {
        snprintf(handshake, sizeof(handshake),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                 "Sec-WebSocket-Version: 13\r\n"
                 "\r\n", path, host);
    }
    else
    {
        snprintf(handshake, sizeof(handshake),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s:%d\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                 "Sec-WebSocket-Version: 13\r\n"
                 "\r\n", path, host, port);
    }
    slack_ws_send_raw(ws, handshake, strlen(handshake));
}

static int
slack_ws_connect_cb(const void *pointer, void *data,
                    int status, int gnutls_rc, int sock,
                    const char *error, const char *ip_address)
{
    (void) data;
    (void) gnutls_rc;
    (void) ip_address;

    struct t_slack_ws *ws = (struct t_slack_ws *)pointer;
    char host[256];
    char path[1024];
    int port;

    if (!ws)
        return WEECHAT_RC_OK;

    ws->connecting = 0;

    if (status == WEECHAT_HOOK_CONNECT_OK)
    {
        ws->fd = sock;
        ws->reconnect_delay = 1;

        if (!g_ssl_ctx)
        {
            g_ssl_ctx = SSL_CTX_new(TLS_client_method());
            if (!g_ssl_ctx)
            {
                SLACK_WS_PRINTF(ws->workspace, "%sweeslack: OpenSSL context failed",
                                weechat_prefix("error"));
                close(sock);
                ws->fd = -1;
                slack_ws_reconnect(ws);
                return WEECHAT_RC_OK;
            }
            SSL_CTX_set_default_verify_paths(g_ssl_ctx);
            SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_PEER, NULL);
        }

        ws->ssl = SSL_new(g_ssl_ctx);
        if (!ws->ssl)
        {
            SLACK_WS_PRINTF(ws->workspace, "%sweeslack: OpenSSL SSL_new failed",
                            weechat_prefix("error"));
            close(sock);
            ws->fd = -1;
            slack_ws_reconnect(ws);
            return WEECHAT_RC_OK;
        }

        SSL_set_fd(ws->ssl, sock);
        SSL_set_connect_state(ws->ssl);

        /* SNI is required for Slack hosts */
        if (ws->workspace->ws_url &&
            slack_ws_parse_url(ws->workspace->ws_url, host, sizeof(host),
                               path, sizeof(path), &port) == 0)
        {
            SSL_set_tlsext_host_name(ws->ssl, host);
        }

        ws->connected = 1;
        ws->tls_handshake_done = 0;
        ws->handshake_done = 0;
        free(ws->recv_buffer);
        ws->recv_buffer = NULL;
        ws->recv_buffer_len = 0;

        ws->hook_fd = weechat_hook_fd(sock, 1, 1, 0,
                                       &slack_ws_read_cb, ws, NULL);

        ws->hook_ping = weechat_hook_timer(
            ws->ping_interval, 0, 0,
            &slack_ws_ping_cb, ws, NULL);

        SLACK_WS_PRINTF(ws->workspace, "%sweeslack: TCP connected to %s, starting TLS...",
                        weechat_prefix("network"), ws->workspace->name);

        /* Kick TLS if it can progress immediately */
        {
            int rc = SSL_do_handshake(ws->ssl);
            if (rc == 1)
            {
                ws->tls_handshake_done = 1;
                SLACK_WS_PRINTF(ws->workspace, "%sweeslack: TLS handshake complete for %s",
                                weechat_prefix("network"), ws->workspace->name);
                slack_ws_send_handshake(ws);
            }
        }
    }
    else
    {
        SLACK_WS_PRINTF(ws->workspace, "%sweeslack: failed to connect to %s: %s",
                        weechat_prefix("error"),
                        ws->workspace->name, error ? error : "unknown");
        slack_ws_reconnect(ws);
    }

    return WEECHAT_RC_OK;
}

int
slack_ws_connect(struct t_slack_ws *ws, const char *url)
{
    char host[256];
    char path[1024];
    int port;
    const char *proxy;

    if (!ws || !url || ws->connecting)
        return WEECHAT_RC_ERROR;

    if (ws->connected)
        slack_ws_disconnect(ws);

    if (slack_ws_parse_url(url, host, sizeof(host), path, sizeof(path), &port) != 0)
    {
        SLACK_WS_PRINTF(ws->workspace, "%sweeslack: invalid websocket URL: %s",
                        weechat_prefix("error"), url);
        return WEECHAT_RC_ERROR;
    }

    free(ws->workspace->ws_url);
    ws->workspace->ws_url = strdup(url);
    if (!ws->workspace->ws_url)
        return WEECHAT_RC_ERROR;

    proxy = weechat_config_string(
        weechat_config_get("weechat.network.proxy_curl"));

    ws->connecting = 1;

    /* ipv6=0 prefers IPv4 — avoids multi-second dual-stack stalls on some networks */
    weechat_hook_connect(
        (proxy && proxy[0]) ? proxy : NULL,
        host,
        port,
        0, 0,
        NULL, NULL, 0, NULL, NULL,
        &slack_ws_connect_cb, ws, NULL);

    SLACK_WS_PRINTF(ws->workspace, "%sweeslack: connecting websocket to %s:%d ...",
                    weechat_prefix("network"), host, port);

    return WEECHAT_RC_OK;
}

int
slack_ws_disconnect(struct t_slack_ws *ws)
{
    if (!ws)
        return WEECHAT_RC_ERROR;

    if (ws->hook_fd)
    {
        weechat_unhook(ws->hook_fd);
        ws->hook_fd = NULL;
    }

    if (ws->hook_ping)
    {
        weechat_unhook(ws->hook_ping);
        ws->hook_ping = NULL;
    }

    if (ws->hook_reconnect)
    {
        weechat_unhook(ws->hook_reconnect);
        ws->hook_reconnect = NULL;
    }

    if (ws->ssl)
    {
        SSL_shutdown(ws->ssl);
        SSL_free(ws->ssl);
        ws->ssl = NULL;
    }

    if (ws->fd >= 0)
    {
        close(ws->fd);
        ws->fd = -1;
    }

    ws->connected = 0;
    ws->connecting = 0;
    ws->tls_handshake_done = 0;
    ws->handshake_done = 0;
    free(ws->recv_buffer);
    ws->recv_buffer = NULL;
    ws->recv_buffer_len = 0;

    return WEECHAT_RC_OK;
}

int
slack_ws_send(struct t_slack_ws *ws, struct json_object *msg)
{
    const char *json_str;
    size_t len;

    if (!ws || !ws->connected || !ws->handshake_done || !msg)
        return WEECHAT_RC_ERROR;

    json_str = json_object_to_json_string(msg);
    len = strlen(json_str);

    if (slack_ws_send_frame(ws, 0x1, json_str, len) != 0)
        return WEECHAT_RC_ERROR;

    return WEECHAT_RC_OK;
}

void
slack_ws_reconnect(struct t_slack_ws *ws)
{
    if (!ws || ws->connecting || ws->hook_reconnect)
        return;

    slack_ws_disconnect(ws);

    SLACK_WS_PRINTF(ws->workspace, "%sweeslack: reconnecting to %s in %ds...",
                    weechat_prefix("network"),
                    ws->workspace->name, ws->reconnect_delay);

    ws->hook_reconnect = weechat_hook_timer(
        ws->reconnect_delay * 1000, 0, 1,
        &slack_ws_reconnect_cb, ws, NULL);

    ws->reconnect_delay *= 2;
    if (ws->reconnect_delay > ws->max_reconnect_delay)
        ws->reconnect_delay = ws->max_reconnect_delay;
}
