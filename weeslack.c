#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <regex.h>
#include <json-c/json.h>

#include "weeslack.h"
#include "slack_http.h"
#include "slack_ws.h"
#include "slack_event.h"
#include "slack_buffer.h"
#include "slack_data.h"

/* declared in slack_http.h — ensure queue lives with plugin */

WEECHAT_PLUGIN_NAME("weeslack")
WEECHAT_PLUGIN_DESCRIPTION("Slack protocol support for WeeChat (migrates from wee-slack)")
WEECHAT_PLUGIN_VERSION(WEESLACK_VERSION)
WEECHAT_PLUGIN_AUTHOR("Emiel Kollof")
WEECHAT_PLUGIN_LICENSE("BSD-2-Clause")

struct t_weechat_plugin *weechat_plugin = NULL;

struct t_weeslack_workspace *weeslack_workspaces = NULL;
struct t_weeslack_channel *weeslack_channels = NULL;

struct t_weeslack_config weeslack_config;

static struct t_upgrade_file *weeslack_upgrade_file = NULL;

static const char *
weeslack_find_weeslack_token(void)
{
    struct t_config_option *option;

    option = weechat_config_get("plugins.var.python.slack.slack_api_token");
    if (option)
        return weechat_config_string(option);

    return NULL;
}

static void
weeslack_migrate_from_weeslack(struct t_gui_buffer *buffer)
{
    const char *existing;

    existing = weechat_config_string(weeslack_config.token);
    if (existing && existing[0])
    {
        weechat_printf(buffer, "%sweeslack: token already configured, "
                        "skipping migration",
                        weechat_prefix("network"));
        return;
    }

    const char *token_raw;

    token_raw = weeslack_find_weeslack_token();
    if (!token_raw)
    {
        weechat_printf(buffer, "%sweeslack: no token found in "
                        "plugins.var.python.slack.slack_api_token",
                        weechat_prefix("error"));
        return;
    }

    char *token_list;
    token_list = weechat_string_eval_expression(token_raw, NULL, NULL, NULL);
    if (!token_list || !token_list[0])
    {
        weechat_printf(buffer, "%sweeslack: failed to evaluate token",
                        weechat_prefix("error"));
        free(token_list);
        return;
    }

    weechat_config_option_set(weeslack_config.token, token_list, 1);
    free(token_list);

    weechat_printf(buffer, "%sweeslack: migrated token from wee-slack to "
                    "weeslack.workspace.token",
                    weechat_prefix("network"));
}

static void
weeslack_rtm_connect_cb(struct t_weeslack_workspace *workspace,
                        int return_code, const char *output,
                        void *user_data)
{
    (void) user_data;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: rtm.connect failed for %s (rc=%d)",
                        weechat_prefix("error"), workspace->name, return_code);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (!json)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: failed to parse rtm.connect response",
                        weechat_prefix("error"));
        return;
    }

    if (slack_api_check_error(workspace, json, "rtm.connect"))
    {
        json_object_put(json);
        return;
    }

    struct json_object *team_obj, *self_obj;

    if (json_object_object_get_ex(json, "team", &team_obj))
    {
        struct json_object *name_obj, *domain_obj;
        const char *team_name = NULL;
        const char *domain = NULL;

        if (json_object_object_get_ex(team_obj, "domain", &domain_obj))
            domain = json_object_get_string(domain_obj);
        if (json_object_object_get_ex(team_obj, "name", &name_obj))
            team_name = json_object_get_string(name_obj);

        if (domain && domain[0])
        {
            free(workspace->domain);
            workspace->domain = strdup(domain);
        }

        /*
         * Display name: workspace.server_aliases (subdomain:alias),
         * else Slack team name, else domain.
         */
        {
            const char *aliases;
            char *chosen = NULL;

            aliases = weechat_config_string(weeslack_config.server_aliases);
            if (aliases && aliases[0] && workspace->domain)
            {
                char *copy = strdup(aliases);
                char *save = NULL, *tok;

                if (copy)
                {
                    for (tok = strtok_r(copy, ",", &save); tok;
                         tok = strtok_r(NULL, ",", &save))
                    {
                        char *colon;
                        while (*tok == ' ' || *tok == '\t')
                            tok++;
                        colon = strchr(tok, ':');
                        if (!colon || colon == tok)
                            continue;
                        *colon = '\0';
                        if (weechat_strcasecmp(tok, workspace->domain) == 0)
                        {
                            char *alias = colon + 1;
                            while (*alias == ' ' || *alias == '\t')
                                alias++;
                            if (*alias)
                                chosen = strdup(alias);
                            break;
                        }
                    }
                    free(copy);
                }
            }

            free(workspace->name);
            if (chosen)
                workspace->name = chosen;
            else if (team_name && team_name[0])
                workspace->name = strdup(team_name);
            else if (workspace->domain)
                workspace->name = strdup(workspace->domain);
            else
                workspace->name = strdup(workspace->id ? workspace->id
                                                       : "slack");
        }
    }

    if (json_object_object_get_ex(json, "self", &self_obj))
    {
        struct json_object *id_obj;
        if (json_object_object_get_ex(self_obj, "id", &id_obj))
        {
            free(workspace->my_user_id);
            workspace->my_user_id = strdup(json_object_get_string(id_obj));
        }
    }

    slack_buffer_new_server(workspace);

    struct json_object *url_obj;
    if (json_object_object_get_ex(json, "url", &url_obj))
    {
        const char *ws_url = json_object_get_string(url_obj);
        SLACK_WS_PRINTF(workspace, "%sweeslack: got websocket URL for %s",
                        weechat_prefix("network"), workspace->name);

        if (!workspace->ws)
            workspace->ws = slack_ws_new(workspace);

        if (workspace->ws)
            slack_ws_connect(workspace->ws, ws_url);
    }

    /* users.list first so history/messages can resolve U… ids to names */
    slack_event_fetch_users(workspace);

    json_object_put(json);
}

static void
weeslack_connect_workspace(struct t_weeslack_workspace *workspace,
                           struct t_gui_buffer *buffer)
{
    if (!workspace || !workspace->token || !workspace->token[0])
    {
        weechat_printf(buffer, "%sweeslack: no token configured for %s",
                        weechat_prefix("error"),
                        workspace ? workspace->name : "(null)");
        return;
    }

    weechat_printf(buffer, "%sweeslack: connecting to %s...",
                    weechat_prefix("network"), workspace->name);

    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "batch_presence_aware",
                           json_object_new_int(1));

    slack_http_request_new(workspace, "rtm.connect", params,
                           weeslack_rtm_connect_cb, buffer);

    json_object_put(params);
}

/* True if token looks usable (xox* / xoxc:cookie). Empty or malformed → 0. */
static int
weeslack_token_is_valid(const char *token_cfg)
{
    if (!token_cfg || !token_cfg[0])
        return 0;
    /* Skip leading whitespace */
    while (*token_cfg == ' ' || *token_cfg == '\t')
        token_cfg++;
    if (strncmp(token_cfg, "xox", 3) != 0)
        return 0;
    /* Session tokens need cookie: "xoxc-…:d=…" */
    if (strncmp(token_cfg, "xoxc-", 5) == 0 && !strchr(token_cfg, ':'))
        return 0;
    return 1;
}

/* Parse one "xox…:cookie?" into token/cookie parts (mutates copy). */
static void
weeslack_token_split_parts(char *token_copy, char **token_part,
                            char **cookie_part)
{
    char *colon;

    *token_part = token_copy;
    *cookie_part = NULL;
    if (!token_copy)
        return;
    colon = strchr(token_copy, ':');
    if (colon && strncmp(token_copy, "xoxc-", 5) == 0)
    {
        *colon = '\0';
        *cookie_part = colon + 1;
    }
}

/* Connect a single token under stable workspace id (e.g. default, ws1). */
static int
weeslack_connect_one_token(const char *token_raw, const char *ws_id,
                            struct t_gui_buffer *buffer)
{
    char *token_copy, *token_part, *cookie_part;
    struct t_weeslack_workspace *ws;

    if (!token_raw || !ws_id)
        return 0;
    if (!weeslack_token_is_valid(token_raw))
    {
        if (buffer)
        {
            weechat_printf(buffer,
                            "%sweeslack: skipping invalid token for %s",
                            weechat_prefix("error"), ws_id);
        }
        return 0;
    }

    token_copy = strdup(token_raw);
    if (!token_copy)
        return 0;
    weeslack_token_split_parts(token_copy, &token_part, &cookie_part);

    ws = weeslack_workspace_search(ws_id);
    if (!ws)
        ws = weeslack_workspace_new(ws_id, token_part, cookie_part);
    else
    {
        free(ws->token);
        ws->token = strdup(token_part);
        free(ws->cookie);
        ws->cookie = cookie_part ? strdup(cookie_part) : NULL;
    }
    free(token_copy);

    if (!ws)
        return 0;

    if (ws->connected)
    {
        if (buffer)
        {
            weechat_printf(buffer, "%sweeslack: already connected to %s",
                            weechat_prefix("network"),
                            ws->name ? ws->name : ws_id);
        }
        return 0;
    }

    weeslack_connect_workspace(ws, buffer);
    return 1;
}

struct t_weeslack_delayed_connect
{
    char *token;
    char *ws_id;
};

static int
weeslack_delayed_connect_cb(const void *pointer, void *data,
                             int remaining_calls)
{
    struct t_weeslack_delayed_connect *ctx =
        (struct t_weeslack_delayed_connect *)pointer;

    (void)data;
    (void)remaining_calls;

    if (ctx)
    {
        weeslack_connect_one_token(ctx->token, ctx->ws_id, NULL);
        free(ctx->token);
        free(ctx->ws_id);
        free(ctx);
    }
    return WEECHAT_RC_OK;
}

/*
 * Parse weeslack.workspace.token — comma-separated multi-team tokens
 * (wee-slack style). First token → workspace id "default", then ws1, ws2…
 * Extra teams connect staggered (3s) to avoid rate-limit storms.
 * buffer may be NULL (auto-connect). Returns number of connects started.
 */
static int
weeslack_connect_from_config(struct t_gui_buffer *buffer)
{
    const char *token_cfg;
    char *copy, *save, *tok;
    int idx = 0, started = 0;

    token_cfg = weechat_config_string(weeslack_config.token);
    if (!token_cfg || !token_cfg[0])
    {
        if (buffer)
        {
            weechat_printf(buffer,
                            "%sweeslack: no token configured. "
                            "Run /cslack migrate or set "
                            "weeslack.workspace.token",
                            weechat_prefix("error"));
        }
        return 0;
    }

    copy = strdup(token_cfg);
    if (!copy)
        return 0;

    for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
    {
        char ws_id[32];
        char *trimmed = tok;

        while (*trimmed == ' ' || *trimmed == '\t')
            trimmed++;
        if (!trimmed[0])
            continue;

        if (idx == 0)
            snprintf(ws_id, sizeof(ws_id), "default");
        else
            snprintf(ws_id, sizeof(ws_id), "ws%d", idx);

        if (idx == 0)
        {
            if (weeslack_connect_one_token(trimmed, ws_id, buffer))
                started++;
        }
        else
        {
            /* Stagger extra workspaces — full bootstrap is heavy. */
            struct t_weeslack_delayed_connect *ctx;

            if (!weeslack_token_is_valid(trimmed))
            {
                if (buffer)
                {
                    weechat_printf(buffer,
                                    "%sweeslack: skipping invalid token #%d",
                                    weechat_prefix("error"), idx + 1);
                }
                idx++;
                continue;
            }
            ctx = calloc(1, sizeof(*ctx));
            if (!ctx)
            {
                idx++;
                continue;
            }
            ctx->token = strdup(trimmed);
            ctx->ws_id = strdup(ws_id);
            if (!ctx->token || !ctx->ws_id)
            {
                free(ctx->token);
                free(ctx->ws_id);
                free(ctx);
                idx++;
                continue;
            }
            weechat_hook_timer((long long)idx * 3000, 0, 1,
                                &weeslack_delayed_connect_cb, ctx, NULL);
            started++;
            if (buffer)
            {
                weechat_printf(buffer,
                                "%sweeslack: will connect workspace %s in %ds…",
                                weechat_prefix("network"), ws_id, idx * 3);
            }
        }
        idx++;
    }
    free(copy);

    if (idx == 0 && buffer)
    {
        weechat_printf(buffer, "%sweeslack: no valid tokens in config",
                        weechat_prefix("error"));
    }
    else if (started > 1 && buffer)
    {
        weechat_printf(buffer,
                        "%sweeslack: connecting %d workspace%s "
                        "(staggered to respect rate limits)",
                        weechat_prefix("network"), started,
                        started == 1 ? "" : "s");
    }

    return started;
}

/* Deferred connect after plugin load or /upgrade (HTTP hooks ready). */
static int
weeslack_autoconnect_cb(const void *pointer, void *data, int remaining_calls)
{
    (void)pointer;
    (void)data;
    (void)remaining_calls;

    weeslack_connect_from_config(NULL);
    return WEECHAT_RC_OK;
}

/* Schedule auto-connect when config + WeeChat allow it and token is valid. */
static void
weeslack_maybe_autoconnect(void)
{
    const char *wc_auto;
    const char *tok;

    if (!weechat_config_boolean(weeslack_config.auto_connect))
        return;

    /* WeeChat -a / no-autoconnect sets info auto_connect to "0" (wee-slack). */
    wc_auto = weechat_info_get("auto_connect", "");
    if (wc_auto && strcmp(wc_auto, "0") == 0)
        return;

    tok = weechat_config_string(weeslack_config.token);
    if (!weeslack_token_is_valid(tok))
        return;

    weechat_hook_timer(500, 0, 1, &weeslack_autoconnect_cb, NULL, NULL);
}

/* Prefer the focused window buffer when /cslack is run from core
 * (debug socket, /command -buffer core, scripts). */
static struct t_gui_buffer *
weeslack_cmd_buffer(struct t_gui_buffer *buffer)
{
    const char *full;
    struct t_gui_buffer *cur;

    if (!buffer)
        return weechat_current_buffer();

    full = weechat_buffer_get_string(buffer, "full_name");
    if (full && strcmp(full, "core.weechat") == 0)
    {
        cur = weechat_current_buffer();
        if (cur)
            return cur;
    }
    return buffer;
}

static const char *
weeslack_cmd_channel_id(struct t_gui_buffer *buffer)
{
    struct t_gui_buffer *buf = weeslack_cmd_buffer(buffer);
    return weechat_buffer_get_string(buf, "localvar_slack_channel_id");
}

/* Resolve a message ts: explicit arg (ts / $hash / index), else buffer, else last. */
static char weeslack_cmd_ts_buf[64];

static const char *
weeslack_cmd_message_ts(struct t_gui_buffer *buffer, const char *channel_id,
                        const char *explicit_ts)
{
    struct t_gui_buffer *buf;
    const char *ts;
    struct t_slack_channel *ch;

    if (explicit_ts && explicit_ts[0])
    {
        /* Full Slack ts (digits.digits) — pass through */
        if (strchr(explicit_ts, '.') && explicit_ts[0] >= '0' &&
            explicit_ts[0] <= '9')
            return explicit_ts;

        if (channel_id)
        {
            ch = slack_channel_search(channel_id);
            if (ch)
            {
                struct t_slack_message *m;

                m = slack_message_from_ref(ch->messages, explicit_ts, NULL, NULL);
                if (m)
                {
                    char *s = slack_ts_to_string(m->ts);
                    if (s)
                    {
                        snprintf(weeslack_cmd_ts_buf, sizeof(weeslack_cmd_ts_buf),
                                 "%s", s);
                        free(s);
                        return weeslack_cmd_ts_buf;
                    }
                }
            }
        }
        /* Unknown hash/index — still return raw (may fail at API) */
        return explicit_ts;
    }

    buf = weeslack_cmd_buffer(buffer);
    ts = weechat_buffer_get_string(buf, "localvar_slack_timestamp");
    if (ts && ts[0])
        return ts;

    if (channel_id)
    {
        ch = slack_channel_search(channel_id);
        if (ch && ch->last_message_ts && ch->last_message_ts[0])
            return ch->last_message_ts;
    }
    return NULL;
}

/* ---- OAuth register (wee-slack compatible client id) ---- */

/* Public wee-slack app credentials (not a secret). */
#define WEESLACK_OAUTH_CLIENT_ID     "2468770254.51917335286"
#define WEESLACK_OAUTH_CLIENT_SECRET "dcb7fe380a000cba0cca3169a5fe8d70"
#define WEESLACK_OAUTH_REDIRECT_GH   "https://wee-slack.github.io/wee-slack/oauth"
#define WEESLACK_OAUTH_REDIRECT_NTP  "http://not.a.realhost/"

/*
 * Remove a workspace's token from the comma-separated config list.
 * Matches xoxp/xoxb plain tokens and xoxc token:cookie entries.
 * Returns 1 if config changed.
 */
static int
weeslack_token_config_remove(const char *token, const char *cookie)
{
    const char *cur;
    char *copy, *save, *tok;
    char rebuilt[2048];
    char entry_match[1024];
    size_t len = 0;
    int removed = 0;

    if (!token || !token[0])
        return 0;

    cur = weechat_config_string(weeslack_config.token);
    if (!cur || !cur[0])
        return 0;

    /* Full form as stored in config for xoxc */
    if (cookie && cookie[0])
        snprintf(entry_match, sizeof(entry_match), "%s:%s", token, cookie);
    else
        snprintf(entry_match, sizeof(entry_match), "%s", token);

    copy = strdup(cur);
    if (!copy)
        return 0;

    rebuilt[0] = '\0';
    for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
    {
        while (*tok == ' ' || *tok == '\t')
            tok++;
        if (!tok[0])
            continue;

        /* Exact full match, or token-only match (xoxp / leading part of xoxc) */
        if (strcmp(tok, entry_match) == 0 ||
            strcmp(tok, token) == 0 ||
            (strncmp(tok, token, strlen(token)) == 0 &&
             (tok[strlen(token)] == '\0' || tok[strlen(token)] == ':')))
        {
            removed = 1;
            continue;
        }

        if (len > 0 && len + 1 < sizeof(rebuilt))
            rebuilt[len++] = ',';
        {
            size_t tlen = strlen(tok);
            if (len + tlen >= sizeof(rebuilt))
                break;
            memcpy(rebuilt + len, tok, tlen);
            len += tlen;
            rebuilt[len] = '\0';
        }
    }
    free(copy);

    if (!removed)
        return 0;

    weechat_config_option_set(weeslack_config.token, rebuilt, 1);
    return 1;
}

/* Close all weeslack buffers belonging to workspace (by localvar id). */
static void
weeslack_workspace_close_buffers(struct t_weeslack_workspace *ws)
{
    struct t_hdata *hdata;
    struct t_gui_buffer *ptr, *next;
    const char *wid;

    if (!ws || !ws->id)
        return;

    hdata = weechat_hdata_get("buffer");
    if (!hdata)
        return;

    ptr = weechat_hdata_get_list(hdata, "gui_buffers");
    while (ptr)
    {
        next = weechat_hdata_move(hdata, ptr, 1);
        {
            const char *plugin = weechat_buffer_get_string(ptr, "plugin");
            if (plugin && strcmp(plugin, "weeslack") == 0)
            {
                wid = weechat_buffer_get_string(ptr,
                                               "localvar_slack_workspace_id");
                if (wid && strcmp(wid, ws->id) == 0)
                    weechat_buffer_close(ptr);
            }
        }
        ptr = next;
    }
    ws->server_buffer = NULL;
}

/*
 * Retire a workspace: cancel HTTP, disconnect RTM, close buffers, drop token,
 * free model. Opposite of register/connect — for leaving a company/team.
 */
static void
weeslack_workspace_unregister(struct t_weeslack_workspace *ws,
                               struct t_gui_buffer *buffer)
{
    char label[128];
    int tok_removed;

    if (!ws)
        return;

    snprintf(label, sizeof(label), "%s",
             ws->name ? ws->name
                      : (ws->domain ? ws->domain
                                    : (ws->id ? ws->id : "?")));

    slack_http_requests_cancel(ws);

    if (ws->ws)
    {
        slack_ws_free(ws->ws);
        ws->ws = NULL;
    }
    ws->connected = 0;

    weeslack_workspace_close_buffers(ws);

    /* Drop channel / user / bot / subteam / custom emoji for this team. */
    slack_channel_free_workspace(ws);
    slack_event_free_workspace_data(ws);

    tok_removed = weeslack_token_config_remove(ws->token, ws->cookie);

    weeslack_workspace_free(ws);

    weechat_printf(buffer,
                    "%sweeslack: retired workspace \"%s\"%s",
                    weechat_prefix("network"), label,
                    tok_removed ? " (token removed from config)"
                                : " (token not found in config — check manually)");
    weechat_printf(buffer,
                    "%sweeslack: this team will not auto-connect again until "
                    "you /cslack register or set a token",
                    weechat_prefix("network"));
}

static void
weeslack_token_config_append(struct t_gui_buffer *buffer, const char *token,
                              const char *team_name)
{
    const char *cur;
    char *new_tok;

    if (!token || !token[0])
        return;

    cur = weechat_config_string(weeslack_config.token);
    if (cur && cur[0])
    {
        /* Already registered? */
        if (strstr(cur, token))
        {
            weechat_printf(buffer, "%sweeslack: this token is already configured",
                            weechat_prefix("error"));
            return;
        }
        new_tok = malloc(strlen(cur) + strlen(token) + 2);
        if (!new_tok)
            return;
        snprintf(new_tok, strlen(cur) + strlen(token) + 2, "%s,%s", cur, token);
        weechat_config_option_set(weeslack_config.token, new_tok, 1);
        free(new_tok);
    }
    else
    {
        weechat_config_option_set(weeslack_config.token, token, 1);
    }

    if (team_name && team_name[0])
        weechat_printf(buffer, "%sweeslack: added team \"%s\"",
                        weechat_prefix("network"), team_name);
    else
        weechat_printf(buffer, "%sweeslack: token added to weeslack.workspace.token",
                        weechat_prefix("network"));
    weechat_printf(buffer,
                    "%sweeslack: run /cslack connect (or reload plugin) to use it",
                    weechat_prefix("network"));
}

static void
weeslack_register_oauth_done(void *user_data, int ok, long http_code,
                             const char *body)
{
    struct t_gui_buffer *buffer = user_data;
    struct json_object *json, *ok_obj, *tok_obj, *team_obj, *err_obj;
    int api_ok = 0;

    if (!ok || !body || !body[0])
    {
        weechat_printf(buffer,
                        "%sweeslack: OAuth request failed (HTTP %ld)",
                        weechat_prefix("error"), http_code);
        return;
    }

    json = json_tokener_parse(body);
    if (!json)
    {
        weechat_printf(buffer, "%sweeslack: OAuth invalid JSON",
                        weechat_prefix("error"));
        return;
    }

    if (json_object_object_get_ex(json, "ok", &ok_obj))
        api_ok = json_object_get_boolean(ok_obj);

    if (!api_ok)
    {
        const char *err = "unknown";
        if (json_object_object_get_ex(json, "error", &err_obj))
            err = json_object_get_string(err_obj);
        weechat_printf(buffer, "%sweeslack: OAuth failed: %s",
                        weechat_prefix("error"), err ? err : "unknown");
        json_object_put(json);
        return;
    }

    if (json_object_object_get_ex(json, "access_token", &tok_obj))
    {
        const char *token = json_object_get_string(tok_obj);
        const char *team = NULL;
        if (json_object_object_get_ex(json, "team_name", &team_obj))
            team = json_object_get_string(team_obj);
        weeslack_token_config_append(buffer, token, team);
    }
    else
    {
        weechat_printf(buffer, "%sweeslack: OAuth response missing access_token",
                        weechat_prefix("error"));
    }

    json_object_put(json);
}

static void
weeslack_command_register(struct t_gui_buffer *buffer, int argc, char **argv,
                           char **argv_eol)
{
    int nothirdparty = 0;
    const char *code = NULL;
    char url[1024];
    const char *redirect;

    if (argc >= 3)
    {
        if (weechat_strcasecmp(argv[2], "-nothirdparty") == 0)
        {
            nothirdparty = 1;
            if (argc >= 4)
                code = argv_eol[3];
        }
        else
            code = argv_eol[2];
    }

    redirect = nothirdparty ? WEESLACK_OAUTH_REDIRECT_NTP
                            : WEESLACK_OAUTH_REDIRECT_GH;

    if (!code || !code[0])
    {
        weechat_printf(buffer, "%s", "");
        weechat_printf(buffer,
                        "%s### Connecting to a Slack team with OAuth ###%s",
                        weechat_color("bold"), weechat_color("reset"));
        if (!nothirdparty)
        {
            weechat_printf(buffer,
                            "Note: by default GitHub Pages sees a temporary "
                            "code (not the token). Use -nothirdparty if preferred.");
        }
        weechat_printf(buffer,
                        "1) Open: "
                        "https://slack.com/oauth/authorize?client_id=%s"
                        "&scope=client&redirect_uri=%s",
                        WEESLACK_OAUTH_CLIENT_ID,
                        nothirdparty
                            ? "http%%3A%%2F%%2Fnot.a.realhost%%2F"
                            : "https%%3A%%2F%%2Fwee-slack.github.io%%2Fwee-slack%%2Foauth");
        weechat_printf(buffer,
                        "2) Select team → Authorize "
                        "(or request install at "
                        "https://my.slack.com/apps/A1HSZ9V8E-wee-slack).");
        if (nothirdparty)
        {
            weechat_printf(buffer,
                            "3) Browser will fail to load (expected). Copy "
                            "?code=… from the URL, then run:");
            weechat_printf(buffer,
                            "   /cslack register -nothirdparty <code>");
        }
        else
        {
            weechat_printf(buffer,
                            "3) The page shows /cslack register <code> — run it.");
        }
        weechat_printf(buffer,
                        "Or paste an existing token: /cslack register xoxp-…");
        weechat_printf(buffer, "%s", "");
        return;
    }

    /* Strip leading whitespace from code */
    while (*code == ' ' || *code == '\t')
        code++;

    if (strncmp(code, "xox", 3) == 0)
    {
        weeslack_token_config_append(buffer, code, NULL);
        return;
    }

    /* Percent-encode code for query string */
    {
        char code_enc[512];
        size_t i, j = 0;
        for (i = 0; code[i] && j + 4 < sizeof(code_enc); i++)
        {
            unsigned char c = (unsigned char)code[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '.' || c == '~')
                code_enc[j++] = (char)c;
            else
            {
                code_enc[j++] = '%';
                code_enc[j++] = "0123456789ABCDEF"[c >> 4];
                code_enc[j++] = "0123456789ABCDEF"[c & 0xF];
            }
        }
        code_enc[j] = '\0';

        snprintf(url, sizeof(url),
                 "https://slack.com/api/oauth.access?"
                 "client_id=%s&client_secret=%s&redirect_uri=%s&code=%s",
                 WEESLACK_OAUTH_CLIENT_ID,
                 WEESLACK_OAUTH_CLIENT_SECRET,
                 nothirdparty
                     ? "http%3A%2F%2Fnot.a.realhost%2F"
                     : "https%3A%2F%2Fwee-slack.github.io%2Fwee-slack%2Foauth",
                 code_enc);
    }

    (void)redirect; /* encoded above */

    weechat_printf(buffer, "%sweeslack: exchanging OAuth code…",
                    weechat_prefix("network"));
    if (!slack_http_curl_get_body(url, weeslack_register_oauth_done, buffer))
    {
        weechat_printf(buffer,
                        "%sweeslack: could not start OAuth request "
                        "(HTTP busy or curl error)",
                        weechat_prefix("error"));
    }
}

static int
weeslack_command_cslack(const void *pointer, void *data,
                        struct t_gui_buffer *buffer,
                        int argc, char **argv, char **argv_eol)
{
    (void) pointer;
    (void) data;

    if (argc < 2)
    {
        weechat_printf(buffer, "%sweeslack: usage: /cslack <command> [args]",
                        weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (weechat_strcasecmp(argv[1], "connect") == 0)
    {
        weeslack_connect_from_config(buffer);
    }
    else if (weechat_strcasecmp(argv[1], "reconnect") == 0)
    {
        /*
         * /cslack reconnect [all]
         * Tear down RTM and re-issue rtm.connect for one team or all.
         */
        struct t_weeslack_workspace *ws;
        int all = (argc >= 3 && argv[2] &&
                   weechat_strcasecmp(argv[2], "all") == 0);
        int n = 0;

        if (!all)
        {
            ws = weeslack_workspace_from_buffer(weeslack_cmd_buffer(buffer));
            if (ws)
            {
                slack_http_requests_cancel(ws);
                if (ws->ws)
                {
                    slack_ws_free(ws->ws);
                    ws->ws = NULL;
                }
                ws->connected = 0;
                weechat_printf(buffer, "%sweeslack: reconnecting %s…",
                                weechat_prefix("network"),
                                ws->name ? ws->name
                                         : (ws->id ? ws->id : "?"));
                weeslack_connect_workspace(ws, buffer);
                n = 1;
            }
        }

        if (all || n == 0)
        {
            for (ws = weeslack_workspaces; ws; ws = ws->next_workspace)
            {
                slack_http_requests_cancel(ws);
                if (ws->ws)
                {
                    slack_ws_free(ws->ws);
                    ws->ws = NULL;
                }
                ws->connected = 0;
                weeslack_connect_workspace(ws, buffer);
                n++;
            }
            if (n == 0)
            {
                /* No loaded workspaces — full config connect */
                weechat_printf(buffer,
                                "%sweeslack: reconnecting from config…",
                                weechat_prefix("network"));
                weeslack_connect_from_config(buffer);
            }
            else if (all)
                weechat_printf(buffer,
                                "%sweeslack: reconnecting %d workspace(s)…",
                                weechat_prefix("network"), n);
        }
    }
    else if (weechat_strcasecmp(argv[1], "register") == 0)
    {
        weeslack_command_register(buffer, argc, argv, argv_eol);
    }
    else if (weechat_strcasecmp(argv[1], "unregister") == 0 ||
             weechat_strcasecmp(argv[1], "forget") == 0)
    {
        /*
         * /cslack unregister|forget [-yes] [id|name|domain]
         * Removes token + disconnects + closes buffers for that team.
         * Requires -yes (destructive). Target defaults to current buffer's
         * workspace — use /cslack list to see ids.
         */
        struct t_weeslack_workspace *ws = NULL;
        int yes = 0;
        const char *target = NULL;
        int i;

        for (i = 2; i < argc; i++)
        {
            if (weechat_strcasecmp(argv[i], "-yes") == 0 ||
                weechat_strcasecmp(argv[i], "--yes") == 0)
                yes = 1;
            else if (!target)
                target = argv[i];
        }

        if (target)
        {
            ws = weeslack_workspace_search(target);
            if (!ws)
            {
                /* also match domain */
                struct t_weeslack_workspace *w;
                for (w = weeslack_workspaces; w; w = w->next_workspace)
                {
                    if (w->domain &&
                        weechat_strcasecmp(w->domain, target) == 0)
                    {
                        ws = w;
                        break;
                    }
                }
            }
        }
        else
            ws = weeslack_workspace_from_buffer(weeslack_cmd_buffer(buffer));

        if (!ws)
        {
            weechat_printf(buffer,
                            "%sweeslack: no workspace to unregister "
                            "(name/id/domain or use a team buffer). "
                            "See /cslack list",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        if (!yes)
        {
            weechat_printf(buffer,
                            "%sweeslack: will retire workspace \"%s\" "
                            "(id=%s%s%s%s) — disconnect, close buffers, "
                            "remove token from config",
                            weechat_prefix("error"),
                            ws->name ? ws->name : "?",
                            ws->id ? ws->id : "?",
                            ws->domain ? ", domain=" : "",
                            ws->domain ? ws->domain : "");
            weechat_printf(buffer,
                            "%sweeslack: re-run with -yes to confirm: "
                            "/cslack unregister -yes %s",
                            weechat_prefix("network"),
                            ws->id ? ws->id
                                   : (ws->name ? ws->name : ""));
            return WEECHAT_RC_OK;
        }

        weeslack_workspace_unregister(ws, buffer);
    }
    else if (weechat_strcasecmp(argv[1], "disconnect") == 0)
    {
        struct t_weeslack_workspace *ws;
        struct t_gui_buffer *buf = weeslack_cmd_buffer(buffer);
        const char *wid;
        int n = 0;
        int one = 0;

        wid = buf ? weechat_buffer_get_string(buf,
                                             "localvar_slack_workspace_id")
                  : NULL;
        /* On a weeslack buffer with workspace id: disconnect that team only. */
        if (wid && wid[0] && argc < 3)
        {
            ws = weeslack_workspace_search(wid);
            if (ws && ws->ws && ws->connected)
            {
                slack_ws_disconnect(ws->ws);
                ws->connected = 0;
                weechat_printf(buffer, "%sweeslack: disconnected from %s",
                                weechat_prefix("network"),
                                ws->name ? ws->name : ws->id);
                n = 1;
                one = 1;
            }
        }
        if (!one)
        {
            for (ws = weeslack_workspaces; ws; ws = ws->next_workspace)
            {
                if (ws->ws && ws->connected)
                {
                    slack_ws_disconnect(ws->ws);
                    ws->connected = 0;
                    weechat_printf(buffer, "%sweeslack: disconnected from %s",
                                    weechat_prefix("network"),
                                    ws->name ? ws->name : ws->id);
                    n++;
                }
            }
        }
        if (n == 0)
            weechat_printf(buffer, "%sweeslack: nothing to disconnect",
                            weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "migrate") == 0)
    {
        weeslack_migrate_from_weeslack(buffer);
    }
    else if (weechat_strcasecmp(argv[1], "list") == 0)
    {
        struct t_weeslack_workspace *ws;
        if (!weeslack_workspaces)
        {
            weechat_printf(buffer, "%sweeslack: no workspaces loaded",
                            weechat_prefix("network"));
        }
        for (ws = weeslack_workspaces; ws; ws = ws->next_workspace)
        {
            weechat_printf(buffer, "%s  %s (connected: %s)",
                            weechat_prefix("network"),
                            ws->name,
                            ws->connected ? "yes" : "no");
        }
    }
    else if (weechat_strcasecmp(argv[1], "teams") == 0)
    {
        struct t_weeslack_workspace *ws;
        int count = 0;
        for (ws = weeslack_workspaces; ws; ws = ws->next_workspace)
        {
            const char *my_name = ws->my_user_id ? ws->my_user_id : "?";
            weechat_printf(buffer, "%s  %s%s%s — %s [%s]",
                            weechat_prefix("network"),
                            weechat_color("cyan"),
                            ws->name,
                            weechat_color("reset"),
                            my_name,
                            ws->connected ? "connected" : "disconnected");
            count++;
        }
        if (count == 0)
            weechat_printf(buffer, "%sweeslack: no workspaces",
                            weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "channels") == 0)
    {
        struct t_weeslack_workspace *ws;
        struct t_slack_channel *ch;
        const char *pat = NULL;
        regex_t re;
        int use_re = 0;
        int n = 0;

        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        /* /cslack channels -refresh → API re-list */
        if (argc >= 3 &&
            (weechat_strcasecmp(argv[2], "-refresh") == 0 ||
             weechat_strcasecmp(argv[2], "-r") == 0))
        {
            slack_event_fetch_channels(ws);
            weechat_printf(buffer, "%sweeslack: refreshing channel list...",
                            weechat_prefix("network"));
            return WEECHAT_RC_OK;
        }

        /* Optional regex filter (wee-slack: /slack channels [regex]) */
        if (argc >= 3 && argv_eol[2] && argv_eol[2][0])
        {
            pat = argv_eol[2];
            if (regcomp(&re, pat, REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0)
            {
                weechat_printf(buffer, "%sweeslack: invalid channels regex",
                                weechat_prefix("error"));
                return WEECHAT_RC_OK;
            }
            use_re = 1;
        }

        weechat_printf(buffer, "%sweeslack: channels%s%s%s",
                        weechat_prefix("network"),
                        pat ? " matching \"" : "",
                        pat ? pat : "",
                        pat ? "\"" : "");

        for (ch = slack_channel_list_global(); ch; ch = ch->next)
        {
            const char *type_s;
            const char *name;

            if (ch->type == SLACK_CHANNEL_TYPE_DM ||
                ch->type == SLACK_CHANNEL_TYPE_MPDM ||
                ch->type == SLACK_CHANNEL_TYPE_THREAD)
                continue;

            name = ch->name ? ch->name : ch->id;
            if (!name)
                continue;
            if (use_re && regexec(&re, name, 0, NULL, 0) != 0)
                continue;

            if (ch->type == SLACK_CHANNEL_TYPE_GROUP)
                type_s = "group";
            else
                type_s = "channel";

            weechat_printf(buffer, "  %s%-24s%s  %s  %s  %s",
                            weechat_color("cyan"), name, weechat_color("reset"),
                            type_s,
                            ch->is_member ? "member" : "not a member",
                            ch->id ? ch->id : "");
            n++;
        }
        if (use_re)
            regfree(&re);
        weechat_printf(buffer, "%sweeslack: %d channel%s (use -refresh to re-fetch)",
                        weechat_prefix("network"), n, n == 1 ? "" : "s");
    }
    else if (weechat_strcasecmp(argv[1], "loadhistory") == 0 ||
             weechat_strcasecmp(argv[1], "rehistory") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        const char *channel_id = NULL;
        struct t_slack_channel *channel = NULL;

        if (argc >= 3)
            channel_id = argv[2];
        else
            channel_id = weeslack_cmd_channel_id(buffer);

        if (channel_id)
            channel = slack_channel_search(channel_id);

        if (!channel)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected, "
                            "switch to a Slack buffer or "
                            "/cslack loadhistory <channel_id>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        slack_event_fetch_history_force(ws, channel);
        weechat_printf(buffer, "%sweeslack: fetching history for %s...",
                        weechat_prefix("network"), channel->name);
    }
    else if (weechat_strcasecmp(argv[1], "typing") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        const char *channel_id = weeslack_cmd_channel_id(buffer);
        if (channel_id)
            slack_event_send_typing(ws, channel_id);
    }
    else if (weechat_strcasecmp(argv[1], "upload") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        if (argc < 3)
        {
            weechat_printf(buffer, "%sweeslack: usage: /cslack upload <file_path>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        const char *channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        slack_event_upload_file(ws, channel_id, argv[2], NULL);
        weechat_printf(buffer, "%sweeslack: uploading %s...",
                        weechat_prefix("network"), argv[2]);
    }
    else if (weechat_strcasecmp(argv[1], "reply") == 0)
    {
        struct t_weeslack_workspace *ws;
        const char *channel_id;
        const char *thread_ts;
        const char *msg;

        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        /*
         * /cslack reply <thread_ts> <message>
         * /cslack reply <message...>  → parent = last message ts
         * Parent ref: full ts, $hash, or N — then message text.
         */
        {
            int ref_form = 0;
            if (argc >= 4 && argv[2])
            {
                const char *p = argv[2];
                const char *dot = strchr(p, '.');
                if (dot && dot > p && dot[1])
                {
                    ref_form = 1;
                    for (; *p; p++)
                    {
                        if (*p == '.')
                            continue;
                        if (*p < '0' || *p > '9')
                        {
                            ref_form = 0;
                            break;
                        }
                    }
                }
                else if (p[0] == '$' ||
                         (p[0] >= '1' && p[0] <= '9' && !strchr(p, ' ')))
                    ref_form = 1;
            }
            if (ref_form)
            {
                thread_ts = weeslack_cmd_message_ts(buffer, channel_id, argv[2]);
                msg = argv_eol[3];
            }
            else if (argc >= 3)
            {
                thread_ts = weeslack_cmd_message_ts(buffer, channel_id, NULL);
                msg = argv_eol[2];
            }
            else
            {
                thread_ts = NULL;
                msg = NULL;
            }
        }

        if (!msg || !msg[0] || !thread_ts || !thread_ts[0])
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: /cslack reply [thread_ts] <message>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        slack_event_send_message(ws, channel_id, msg, thread_ts);
    }
    else if (weechat_strcasecmp(argv[1], "topic") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        if (argc < 3)
        {
            weechat_printf(buffer, "%sweeslack: usage: /cslack topic <text>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        const char *channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        slack_event_set_topic(ws, channel_id, argv_eol[2]);
    }
    else if (weechat_strcasecmp(argv[1], "users") == 0)
    {
        struct t_slack_user *user;
        struct t_weeslack_workspace *ws;
        int count = 0;
        const char *pat = NULL;
        regex_t re;
        int use_re = 0;

        ws = weeslack_workspace_from_buffer(buffer);

        if (argc >= 3 && argv_eol[2] && argv_eol[2][0])
        {
            pat = argv_eol[2];
            if (regcomp(&re, pat, REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0)
            {
                weechat_printf(buffer, "%sweeslack: invalid users regex",
                                weechat_prefix("error"));
                return WEECHAT_RC_OK;
            }
            use_re = 1;
        }

        for (user = slack_user_list_global(); user; user = user->next)
        {
            const char *status = user->presence ? user->presence : "unknown";
            const char *display = user->display_name && user->display_name[0]
                                  ? user->display_name : user->name;
            const char *real = user->real_name ? user->real_name : "";
            const char *uname = user->name ? user->name : "";

            /* Prefer current workspace when multi-team is loaded. */
            if (ws && user->workspace && user->workspace != ws)
                continue;

            if (use_re)
            {
                int hit = 0;
                if (display && regexec(&re, display, 0, NULL, 0) == 0)
                    hit = 1;
                else if (uname[0] && regexec(&re, uname, 0, NULL, 0) == 0)
                    hit = 1;
                else if (real[0] && regexec(&re, real, 0, NULL, 0) == 0)
                    hit = 1;
                else if (user->id && regexec(&re, user->id, 0, NULL, 0) == 0)
                    hit = 1;
                if (!hit)
                    continue;
            }

            weechat_printf(buffer, "%s  %s%s%s %s (%s) [%s]",
                            weechat_prefix("network"),
                            weechat_color("cyan"),
                            display ? display : "?",
                            weechat_color("reset"),
                            real,
                            user->id ? user->id : "?",
                            status);
            count++;
        }
        if (use_re)
            regfree(&re);
        weechat_printf(buffer, "%sweeslack: %d user%s%s%s%s",
                        weechat_prefix("network"), count,
                        count == 1 ? "" : "s",
                        pat ? " matching \"" : "",
                        pat ? pat : "",
                        pat ? "\"" : "");
    }
    else if (weechat_strcasecmp(argv[1], "usergroups") == 0)
    {
        struct t_weeslack_workspace *ws;
        struct t_slack_subteam *st;
        int count = 0;

        ws = weeslack_workspace_from_buffer(buffer);

        /* /cslack usergroups @handle → members (usergroups.users.list) */
        if (argc >= 3 && argv[2] && argv[2][0])
        {
            if (!ws || !ws->connected)
            {
                weechat_printf(buffer, "%sweeslack: not connected",
                                weechat_prefix("error"));
                return WEECHAT_RC_OK;
            }
            slack_event_usergroup_list_users(ws, argv[2],
                                              weeslack_cmd_buffer(buffer));
            return WEECHAT_RC_OK;
        }

        for (st = slack_subteam_list_global(); st; st = st->next)
        {
            weechat_printf(buffer, "%s  %s%s%s @%s%s%s (%s)%s%s",
                            weechat_prefix("network"),
                            weechat_color("cyan"),
                            st->name ? st->name : st->id,
                            weechat_color("reset"),
                            weechat_color("green"),
                            st->handle ? st->handle : "?",
                            weechat_color("reset"),
                            st->is_member ? "member" : "not a member",
                            st->description && st->description[0] ? " — " : "",
                            st->description ? st->description : "");
            count++;
        }
        weechat_printf(buffer,
                        "%sweeslack: %d user group%s (usergroups @handle for members)",
                        weechat_prefix("network"), count,
                        count == 1 ? "" : "s");
    }
    else if (weechat_strcasecmp(argv[1], "talk") == 0 ||
             weechat_strcasecmp(argv[1], "open") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        if (argc < 3)
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: /cslack talk|open <user>[,user2,...]",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        /* argv_eol keeps "a, b, c" as one string for MPDM */
        slack_event_open_dm(ws, argv_eol[2]);
    }
    else if (weechat_strcasecmp(argv[1], "queue") == 0)
    {
        char status[160];
        slack_http_queue_status(status, sizeof(status));
        weechat_printf(buffer, "%sweeslack HTTP queue: %s",
                        weechat_prefix("network"), status);
    }
    else if (weechat_strcasecmp(argv[1], "version") == 0 ||
             weechat_strcasecmp(argv[1], "info") == 0)
    {
        const char *wver = weechat_info_get("version", "");
        struct t_weeslack_workspace *ws;
        char qstatus[160];
        int n = 0, connected = 0;
        int mode;

        weechat_printf(buffer,
                        "%sweeslack %s (WeeChat %s)",
                        weechat_prefix("network"),
                        WEESLACK_VERSION,
                        wver && wver[0] ? wver : "?");

        slack_http_queue_status(qstatus, sizeof(qstatus));
        weechat_printf(buffer, "%s  HTTP queue: %s",
                        weechat_prefix("network"), qstatus);

        mode = weechat_config_integer(weeslack_config.auto_open_threads);
        weechat_printf(buffer,
                        "%s  auto_open_threads=%s  leave_on_close=%s  "
                        "record_events=%s  icat=%s",
                        weechat_prefix("network"),
                        mode == 2 ? "all"
                                  : (mode == 1 ? "subscribed" : "off"),
                        weechat_config_boolean(
                            weeslack_config.leave_channel_on_buffer_close)
                            ? "on" : "off",
                        weechat_config_boolean(weeslack_config.record_events)
                            ? "on" : "off",
                        weechat_config_boolean(weeslack_config.icat_enabled)
                            ? "on" : "off");

        for (ws = weeslack_workspaces; ws; ws = ws->next_workspace)
        {
            n++;
            if (ws->connected)
                connected++;
            weechat_printf(buffer,
                            "%s  workspace %s id=%s %s%s%s%s",
                            weechat_prefix("network"),
                            ws->name ? ws->name : "?",
                            ws->id ? ws->id : "?",
                            ws->connected ? "connected" : "disconnected",
                            ws->domain && ws->domain[0] ? " domain=" : "",
                            ws->domain && ws->domain[0] ? ws->domain : "",
                            "");
        }
        if (n == 0)
            weechat_printf(buffer, "%s  (no workspaces loaded)",
                            weechat_prefix("network"));
        else
            weechat_printf(buffer, "%s  %d workspace(s), %d connected",
                            weechat_prefix("network"), n, connected);
    }
    else if (weechat_strcasecmp(argv[1], "debug") == 0)
    {
        struct t_weeslack_workspace *ws;
        int n = 0, connected = 0;
        int mode, level, record;

        if (argc >= 3 && argv[2] && argv[2][0])
        {
            if (weechat_strcasecmp(argv[2], "on") == 0 ||
                weechat_strcasecmp(argv[2], "1") == 0 ||
                weechat_strcasecmp(argv[2], "true") == 0)
            {
                weechat_config_option_set(weeslack_config.debug_mode, "on", 1);
                weechat_printf(buffer,
                                "%sweeslack: debug_mode on "
                                "(look.debug_level=%d)",
                                weechat_prefix("network"),
                                weechat_config_integer(
                                    weeslack_config.debug_level));
            }
            else if (weechat_strcasecmp(argv[2], "off") == 0 ||
                     weechat_strcasecmp(argv[2], "0") == 0 ||
                     weechat_strcasecmp(argv[2], "false") == 0)
            {
                weechat_config_option_set(weeslack_config.debug_mode, "off", 1);
                weechat_printf(buffer, "%sweeslack: debug_mode off",
                                weechat_prefix("network"));
                return WEECHAT_RC_OK;
            }
            else
            {
                weechat_printf(buffer,
                                "%sweeslack: usage: /cslack debug [on|off]",
                                weechat_prefix("error"));
                return WEECHAT_RC_OK;
            }
        }

        mode = weechat_config_boolean(weeslack_config.debug_mode);
        level = weechat_config_integer(weeslack_config.debug_level);
        record = weechat_config_boolean(weeslack_config.record_events);

        weeslack_debug_open_buffer();

        for (ws = weeslack_workspaces; ws; ws = ws->next_workspace)
        {
            n++;
            if (ws->connected)
                connected++;
        }

        weechat_printf(buffer,
                        "%sweeslack debug: mode=%s level=%d record_events=%s "
                        "workspaces=%d connected=%d",
                        weechat_prefix("network"),
                        mode ? "on" : "off", level,
                        record ? "on" : "off", n, connected);
        if (mode)
            weeslack_debug_at(1, "debug command: %d workspace(s), %d connected",
                              n, connected);
        else
            weechat_printf(buffer,
                            "%sweeslack: opened weeslack.debug "
                            "(enable with /cslack debug on or "
                            "/set weeslack.look.debug_mode on)",
                            weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "mute") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        const char *channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        slack_event_set_mute(ws, channel_id, 1);
        weechat_printf(buffer, "%sweeslack: channel muted",
                        weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "unmute") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        const char *channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        slack_event_set_mute(ws, channel_id, 0);
        weechat_printf(buffer, "%sweeslack: channel unmuted",
                        weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "status") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        /*
         * wee-slack: /status [<emoji> [<text>]|-delete]
         * Legacy presence: dnd|nodnd|away|active still accepted.
         */
        if (argc < 3)
        {
            struct t_slack_user *me = NULL;
            if (ws->my_user_id)
                me = slack_user_search(ws->my_user_id);
            if (me && ((me->status_emoji && me->status_emoji[0]) ||
                       (me->status_text && me->status_text[0])))
            {
                char *emo = NULL;
                if (me->status_emoji && me->status_emoji[0])
                {
                    char tmp[80];
                    snprintf(tmp, sizeof(tmp), ":%s:", me->status_emoji);
                    emo = slack_event_replace_emoji(ws, tmp);
                }
                weechat_printf(buffer, "%sweeslack: status: %s%s%s",
                                weechat_prefix("network"),
                                emo ? emo : "",
                                (emo && me->status_text && me->status_text[0])
                                    ? " " : "",
                                me->status_text ? me->status_text : "");
                free(emo);
            }
            else
            {
                weechat_printf(buffer, "%sweeslack: no status set  "
                                "(usage: /cslack status <emoji> [text] | "
                                "-delete | dnd|away|active)",
                                weechat_prefix("network"));
            }
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[2], "dnd") == 0)
            slack_event_set_dnd(ws, 1);
        else if (weechat_strcasecmp(argv[2], "nodnd") == 0)
            slack_event_set_dnd(ws, 0);
        else if (weechat_strcasecmp(argv[2], "away") == 0)
            slack_event_set_presence(ws, "away");
        else if (weechat_strcasecmp(argv[2], "active") == 0)
            slack_event_set_presence(ws, "auto");
        else if (weechat_strcasecmp(argv[2], "-delete") == 0)
        {
            slack_event_set_profile_status(ws, "", "");
            weechat_printf(buffer, "%sweeslack: clearing profile status…",
                            weechat_prefix("network"));
            return WEECHAT_RC_OK;
        }
        else
        {
            const char *emoji = argv[2];
            const char *text = (argc >= 4) ? argv_eol[3] : "";
            slack_event_set_profile_status(ws, emoji, text);
            weechat_printf(buffer, "%sweeslack: updating profile status…",
                            weechat_prefix("network"));
            return WEECHAT_RC_OK;
        }

        weechat_printf(buffer, "%sweeslack: presence/dnd updated",
                        weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "create") == 0)
    {
        struct t_weeslack_workspace *ws;
        int is_private = 0;
        const char *name = NULL;

        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        if (argc < 3)
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: /cslack create [-private] <name>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        if (weechat_strcasecmp(argv[2], "-private") == 0)
        {
            is_private = 1;
            if (argc < 4)
            {
                weechat_printf(buffer,
                                "%sweeslack: usage: /cslack create -private <name>",
                                weechat_prefix("error"));
                return WEECHAT_RC_OK;
            }
            name = argv[3];
        }
        else
            name = argv[2];

        slack_event_create_channel(ws, name, is_private);
        weechat_printf(buffer, "%sweeslack: creating %s%s…",
                        weechat_prefix("network"),
                        is_private ? "private channel " : "#",
                        name);
    }
    else if (weechat_strcasecmp(argv[1], "invite") == 0)
    {
        struct t_weeslack_workspace *ws;
        const char *channel_id;

        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        if (argc < 3)
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: /cslack invite <user|@nick|U…>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        slack_event_invite_user(ws, channel_id, argv[2]);
        weechat_printf(buffer, "%sweeslack: inviting %s…",
                        weechat_prefix("network"), argv[2]);
    }
    else if (weechat_strcasecmp(argv[1], "showmuted") == 0)
    {
        struct t_slack_channel *ch;
        int count = 0;
        char list[1024];
        size_t pos = 0;

        list[0] = '\0';
        for (ch = slack_channel_list_global(); ch; ch = ch->next)
        {
            if (!ch->is_muted || !ch->name)
                continue;
            if (pos > 0 && pos + 2 < sizeof(list))
            {
                list[pos++] = ',';
                list[pos++] = ' ';
                list[pos] = '\0';
            }
            {
                int n = snprintf(list + pos, sizeof(list) - pos, "%s",
                                 ch->name);
                if (n > 0 && (size_t)n < sizeof(list) - pos)
                    pos += (size_t)n;
                else
                    break;
            }
            count++;
        }
        if (count == 0)
            weechat_printf(buffer, "%sweeslack: no muted channels",
                            weechat_prefix("network"));
        else
            weechat_printf(buffer, "%sweeslack: muted channels: %s",
                            weechat_prefix("network"), list);
    }
    else if (weechat_strcasecmp(argv[1], "distracting") == 0)
    {
        /* Toggle current buffer full_name in look.distracting_channels */
        struct t_gui_buffer *buf = weeslack_cmd_buffer(buffer);
        const char *full = weechat_buffer_get_string(buf, "full_name");
        const char *cur;
        char new_list[2048];
        int found = 0;

        if (!full || !full[0])
        {
            weechat_printf(buffer, "%sweeslack: no buffer",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        cur = weechat_config_string(weeslack_config.distracting_channels);
        new_list[0] = '\0';
        if (cur && cur[0])
        {
            char *copy = strdup(cur);
            char *tok, *save = NULL;
            size_t pos = 0;

            if (copy)
            {
                for (tok = strtok_r(copy, ",", &save); tok;
                     tok = strtok_r(NULL, ",", &save))
                {
                    while (*tok == ' ')
                        tok++;
                    if (!*tok)
                        continue;
                    if (strcmp(tok, full) == 0)
                    {
                        found = 1;
                        continue;
                    }
                    if (pos && pos + 1 < sizeof(new_list))
                        new_list[pos++] = ',';
                    {
                        int n = snprintf(new_list + pos,
                                         sizeof(new_list) - pos, "%s", tok);
                        if (n > 0 && (size_t)n < sizeof(new_list) - pos)
                            pos += (size_t)n;
                    }
                }
                free(copy);
            }
        }
        if (!found)
        {
            size_t len = strlen(new_list);
            if (len && len + 1 < sizeof(new_list))
            {
                new_list[len++] = ',';
                new_list[len] = '\0';
            }
            snprintf(new_list + strlen(new_list),
                     sizeof(new_list) - strlen(new_list), "%s", full);
            weechat_printf(buffer, "%sweeslack: marked distracting: %s",
                            weechat_prefix("network"), full);
        }
        else
            weechat_printf(buffer, "%sweeslack: unmarked distracting: %s",
                            weechat_prefix("network"), full);
        weechat_config_option_set(weeslack_config.distracting_channels,
                                   new_list, 1);
    }
    else if (weechat_strcasecmp(argv[1], "nodistractions") == 0)
    {
        static int hide_distractions = 0;
        const char *cur;
        char *copy, *tok, *save = NULL;
        int n = 0;

        hide_distractions = !hide_distractions;
        cur = weechat_config_string(weeslack_config.distracting_channels);
        if (!cur || !cur[0])
        {
            weechat_printf(buffer, "%sweeslack: no distracting channels set "
                            "(use /cslack distracting)",
                            weechat_prefix("network"));
            return WEECHAT_RC_OK;
        }
        copy = strdup(cur);
        if (!copy)
            return WEECHAT_RC_OK;
        for (tok = strtok_r(copy, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save))
        {
            struct t_hdata *hbuf;
            void *ptr;

            while (*tok == ' ')
                tok++;
            if (!*tok)
                continue;
            hbuf = weechat_hdata_get("buffer");
            ptr = hbuf ? weechat_hdata_get_list(hbuf, "gui_buffers") : NULL;
            while (ptr)
            {
                const char *fn = weechat_hdata_string(hbuf, ptr, "full_name");
                if (fn && strcmp(fn, tok) == 0)
                {
                    weechat_buffer_set(ptr, "hidden",
                                       hide_distractions ? "1" : "0");
                    n++;
                    break;
                }
                ptr = weechat_hdata_move(hbuf, ptr, 1);
            }
        }
        free(copy);
        weechat_printf(buffer, "%sweeslack: %s %d distracting buffer(s)",
                        weechat_prefix("network"),
                        hide_distractions ? "hid" : "showed", n);
    }
    else if (weechat_strcasecmp(argv[1], "slash") == 0)
    {
        struct t_weeslack_workspace *ws;
        const char *channel_id;
        const char *cmd;
        const char *text = "";

        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        if (argc < 3)
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: /cslack slash /command [args]",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        cmd = argv[2];
        if (argc >= 4)
            text = argv_eol[3];
        slack_event_slash_command(ws, channel_id, cmd, text);
        weechat_printf(buffer, "%sweeslack: running %s…",
                        weechat_prefix("network"), cmd);
    }
    else if (weechat_strcasecmp(argv[1], "away") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        slack_event_set_presence(ws, "away");
        ws->my_manual_away = 1;
        free(ws->my_presence);
        ws->my_presence = strdup("away");
        weechat_bar_item_update("slack_away");
        weechat_printf(buffer, "%sweeslack: marked as away",
                        weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "back") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        slack_event_set_presence(ws, "auto");
        slack_event_set_dnd(ws, 0);
        ws->my_manual_away = 0;
        free(ws->my_presence);
        ws->my_presence = strdup("active");
        weechat_bar_item_update("slack_away");
        weechat_printf(buffer, "%sweeslack: marked as active",
                        weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "hide") == 0)
    {
        struct t_gui_buffer *buf = weeslack_cmd_buffer(buffer);
        if (buf)
        {
            weechat_buffer_set(buf, "hidden", "1");
            weechat_printf(buffer, "%sweeslack: channel hidden",
                            weechat_prefix("network"));
        }
    }
    else if (weechat_strcasecmp(argv[1], "show") == 0)
    {
        /*
         * /cslack show              — unhide current buffer
         * /cslack show #channel     — reopen closed member channel buffer
         */
        struct t_gui_buffer *buf = weeslack_cmd_buffer(buffer);
        struct t_weeslack_workspace *ws;
        struct t_slack_channel *ch = NULL;
        const char *q;

        if (argc >= 3 && argv[2] && argv[2][0])
        {
            q = argv[2];
            if (q[0] == '#')
                q++;
            ws = weeslack_workspace_from_buffer(buf);
            if (!ws)
            {
                weechat_printf(buffer, "%sweeslack: not connected",
                                weechat_prefix("error"));
                return WEECHAT_RC_OK;
            }
            ch = slack_channel_search(q);
            if (!ch)
            {
                for (ch = slack_channel_list_global(); ch; ch = ch->next)
                {
                    if (ch->workspace && ch->workspace != ws)
                        continue;
                    if (ch->name && weechat_strcasecmp(ch->name, q) == 0)
                        break;
                }
            }
            if (!ch || !ch->is_member)
            {
                weechat_printf(buffer,
                                "%sweeslack: unknown or non-member channel "
                                "(try /cslack join %s)",
                                weechat_prefix("error"), argv[2]);
                return WEECHAT_RC_OK;
            }
            if (!ch->buffer)
                slack_buffer_new(ws, ch);
            if (ch->buffer)
            {
                weechat_buffer_set(ch->buffer, "hidden", "0");
                weechat_buffer_set(ch->buffer, "display", "1");
                weechat_printf(buffer, "%sweeslack: showing %s",
                                weechat_prefix("network"),
                                ch->name ? ch->name : ch->id);
            }
        }
        else if (buf)
        {
            weechat_buffer_set(buf, "hidden", "0");
            weechat_buffer_set(buf, "display", "1");
        }
    }
    else if (weechat_strcasecmp(argv[1], "label") == 0)
    {
        if (argc < 3)
        {
            weechat_printf(buffer, "%sweeslack: usage: /cslack label <text>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        weechat_buffer_set(buffer, "title", argv_eol[2]);
        weechat_printf(buffer, "%sweeslack: label updated",
                        weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "thread") == 0)
    {
        struct t_weeslack_workspace *ws;
        struct t_slack_channel *channel;
        struct t_slack_message *parent = NULL;
        const char *channel_id;
        char *parent_ts = NULL;
        const char *ref;

        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        channel = slack_channel_search(channel_id);
        if (!channel)
        {
            weechat_printf(buffer, "%sweeslack: channel not found",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        /*
         * /cslack thread [$hash|N|ts]
         * No arg → last message that has replies (wee-slack).
         */
        ref = (argc >= 3) ? argv[2] : NULL;
        if (ref && ref[0])
        {
            parent = slack_message_from_ref(channel->messages, ref, NULL, NULL);
            if (!parent)
            {
                /* raw ts string even if not in model yet */
                if (strchr(ref, '.'))
                    parent_ts = strdup(ref);
            }
        }
        else
        {
            struct t_slack_message *m;
            for (m = channel->messages; m; m = m->next)
            {
                if (m->reply_count > 0)
                {
                    parent = m;
                    break;
                }
            }
        }

        if (parent)
            parent_ts = slack_ts_to_string(parent->ts);

        if (!parent_ts || !parent_ts[0])
        {
            weechat_printf(buffer,
                            "%sweeslack: no thread found "
                            "(usage: /cslack thread [$hash|N|ts])",
                            weechat_prefix("error"));
            free(parent_ts);
            return WEECHAT_RC_OK;
        }

        {
            struct t_slack_channel *thread;
            char topic_buf[256];

            thread = slack_thread_channel_find(channel, parent_ts);
            if (!thread)
            {
                if (parent && parent->hash)
                    snprintf(topic_buf, sizeof(topic_buf),
                             "Thread: $%s", parent->hash);
                else
                    snprintf(topic_buf, sizeof(topic_buf),
                             "Thread: %s", parent_ts);
                thread = slack_thread_channel_create(channel, parent_ts,
                                                     topic_buf);
                if (thread)
                {
                    slack_buffer_new(ws, thread);
                    slack_event_fetch_replies(ws, thread);
                }
            }
            if (thread && thread->buffer)
            {
                if (weechat_config_boolean(weeslack_config.switch_buffer_on_join))
                    weechat_buffer_set(thread->buffer, "display", "1");
            }
            else
                weechat_printf(buffer, "%sweeslack: could not open thread",
                                weechat_prefix("error"));
        }
        free(parent_ts);
    }
    else if (weechat_strcasecmp(argv[1], "react") == 0 ||
             weechat_strcasecmp(argv[1], "unreact") == 0)
    {
        struct t_weeslack_workspace *ws;
        int add = (weechat_strcasecmp(argv[1], "react") == 0);
        const char *channel_id;
        const char *ts = NULL;
        const char *emoji = NULL;

        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        /* /cslack react <emoji>  OR  /cslack react <ts> <emoji> */
        if (argc >= 4)
        {
            ts = argv[2];
            emoji = argv[3];
        }
        else if (argc >= 3)
        {
            emoji = argv[2];
            ts = weeslack_cmd_message_ts(buffer, channel_id, NULL);
        }
        if (!emoji || !emoji[0] || !ts || !ts[0])
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: /cslack %s [timestamp] <emoji>",
                            weechat_prefix("error"), argv[1]);
            return WEECHAT_RC_OK;
        }
        slack_event_react(ws, channel_id, ts, emoji, add);
    }
    else if (weechat_strcasecmp(argv[1], "linkarchive") == 0)
    {
        struct t_weeslack_workspace *ws;
        const char *channel_id;
        const char *ts;

        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        ts = weeslack_cmd_message_ts(buffer, channel_id,
                                     (argc >= 3) ? argv[2] : NULL);
        if (!ts || !ts[0])
        {
            weechat_printf(buffer, "%sweeslack: no message timestamp; "
                            "usage: /cslack linkarchive [timestamp]",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        slack_event_get_permalink(ws, channel_id, ts, buffer);
    }
    else if (weechat_strcasecmp(argv[1], "subscribe") == 0)
    {
        struct t_weeslack_workspace *ws;
        const char *channel_id;
        struct t_slack_channel *channel;

        ws = weeslack_workspace_from_buffer(buffer);
        channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        channel = slack_channel_search(channel_id);
        if (channel)
        {
            slack_event_set_subscribe(ws, channel, 1);
            weechat_printf(buffer, "%sweeslack: subscribed to thread",
                            weechat_prefix("network"));
        }
    }
    else if (weechat_strcasecmp(argv[1], "unsubscribe") == 0)
    {
        struct t_weeslack_workspace *ws;
        const char *channel_id;
        struct t_slack_channel *channel;

        ws = weeslack_workspace_from_buffer(buffer);
        channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        channel = slack_channel_search(channel_id);
        if (channel)
        {
            slack_event_set_subscribe(ws, channel, 0);
            weechat_printf(buffer, "%sweeslack: unsubscribed from thread",
                            weechat_prefix("network"));
        }
    }
    else if (weechat_strcasecmp(argv[1], "pin") == 0 ||
             weechat_strcasecmp(argv[1], "unpin") == 0)
    {
        struct t_weeslack_workspace *ws;
        int pin = (weechat_strcasecmp(argv[1], "pin") == 0);
        const char *channel_id;
        const char *ts;

        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        ts = weeslack_cmd_message_ts(buffer, channel_id,
                                     (argc >= 3) ? argv[2] : NULL);
        if (!ts || !ts[0])
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: /cslack %s [timestamp]",
                            weechat_prefix("error"), argv[1]);
            return WEECHAT_RC_OK;
        }

        slack_event_pin_message(ws, channel_id, ts, pin);
        weechat_printf(buffer, "%sweeslack: message %s",
                        weechat_prefix("network"),
                        pin ? "pinned" : "unpinned");
    }
    else if (weechat_strcasecmp(argv[1], "search") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        if (argc < 3)
        {
            weechat_printf(buffer, "%sweeslack: usage: /cslack search <query>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        slack_event_search_messages(ws, argv_eol[2], buffer);
        weechat_printf(buffer, "%sweeslack: searching...",
                        weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "download") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        if (argc < 3)
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: /cslack download <url>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        {
            struct t_gui_buffer *buf = weeslack_cmd_buffer(buffer);
            const char *origin = NULL;
            const char *ch = weechat_buffer_get_string(
                buf, "localvar_channel");
            const char *team = weechat_buffer_get_string(
                buf, "localvar_server");
            char origin_buf[192];

            if (team && team[0] && ch && ch[0])
            {
                snprintf(origin_buf, sizeof(origin_buf), "%s.%s", team, ch);
                origin = origin_buf;
            }
            else if (ch && ch[0])
                origin = ch;
            slack_event_download_file(ws, argv[2], buf, origin, NULL);
        }
    }
    else if (weechat_strcasecmp(argv[1], "stars") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        slack_event_stars_list(ws, weeslack_cmd_buffer(buffer));
    }
    else if (weechat_strcasecmp(argv[1], "star") == 0 ||
             weechat_strcasecmp(argv[1], "unstar") == 0)
    {
        struct t_weeslack_workspace *ws;
        int add = (weechat_strcasecmp(argv[1], "star") == 0);
        const char *channel_id;
        const char *ts;

        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        ts = weeslack_cmd_message_ts(buffer, channel_id,
                                     (argc >= 3) ? argv[2] : NULL);
        if (!ts || !ts[0])
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: /cslack %s [timestamp]",
                            weechat_prefix("error"), argv[1]);
            return WEECHAT_RC_OK;
        }
        slack_event_star_message(ws, channel_id, ts, add);
        weechat_printf(buffer, "%sweeslack: %s",
                        weechat_prefix("network"),
                        add ? "starred" : "unstarred");
    }
    else if (weechat_strcasecmp(argv[1], "whois") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        if (argc < 3)
        {
            weechat_printf(buffer, "%sweeslack: usage: /cslack whois <user>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        slack_event_whois(ws, argv[2], weeslack_cmd_buffer(buffer));
    }
    else if (weechat_strcasecmp(argv[1], "refresh") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        weechat_printf(buffer, "%sweeslack: refreshing users + emoji…",
                        weechat_prefix("network"));
        slack_event_refresh_directory(ws);
    }
    else if (weechat_strcasecmp(argv[1], "names") == 0)
    {
        struct t_weeslack_workspace *ws;
        const char *channel_id;
        struct t_slack_channel *channel;

        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        channel_id = weeslack_cmd_channel_id(buffer);
        if (!channel_id)
        {
            weechat_printf(buffer, "%sweeslack: no channel selected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        channel = slack_channel_search(channel_id);
        if (!channel)
        {
            weechat_printf(buffer, "%sweeslack: channel not found",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        weechat_printf(buffer, "%sweeslack: refreshing nicklist…",
                        weechat_prefix("network"));
        slack_event_refresh_members(ws, channel);
    }
    else if (weechat_strcasecmp(argv[1], "join") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        if (argc < 3)
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: /cslack join <#channel|id>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        slack_event_join_channel(ws, argv[2]);
        weechat_printf(buffer, "%sweeslack: joining %s...",
                        weechat_prefix("network"), argv[2]);
    }
    else if (weechat_strcasecmp(argv[1], "leave") == 0 ||
             weechat_strcasecmp(argv[1], "part") == 0)
    {
        struct t_weeslack_workspace *ws;
        const char *channel_id;

        ws = weeslack_workspace_from_buffer(buffer);
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        channel_id = (argc >= 3) ? argv[2] : weeslack_cmd_channel_id(buffer);
        if (!channel_id || !channel_id[0])
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: /cslack leave [channel_id]",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
        /* allow #name → resolve to id */
        if (channel_id[0] == '#' || (channel_id[0] != 'C' && channel_id[0] != 'G'
                                     && channel_id[0] != 'D'))
        {
            struct t_slack_channel *ch;
            const char *q = channel_id[0] == '#' ? channel_id + 1 : channel_id;
            for (ch = slack_channel_list_global(); ch; ch = ch->next)
            {
                if (ch->name && weechat_strcasecmp(ch->name, q) == 0)
                {
                    channel_id = ch->id;
                    break;
                }
            }
        }
        slack_event_leave_channel(ws, channel_id);
        weechat_printf(buffer, "%sweeslack: leaving %s...",
                        weechat_prefix("network"), channel_id);
    }
    else if (weechat_strcasecmp(argv[1], "help") == 0)
    {
        weechat_printf(buffer, "%sweeslack commands:%s",
                        weechat_color("bold"), weechat_color("reset"));
        weechat_printf(buffer, "  %sconnect%s      Connect to Slack",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sreconnect%s [all]  Re-issue rtm.connect",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sdisconnect%s   Disconnect from Slack",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %smigrate%s      Import token from wee-slack",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sregister%s     OAuth add team or paste xox token",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sunregister%s   Retire team/token (-yes [id|name])",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %slist%s         List workspaces",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %schannels%s [re] List or -refresh channels",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %susers%s        List loaded users",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %susergroups%s   List user groups",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sloadhistory%s  Load message history",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %styping%s       Send typing notification",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %supload%s <file> Upload a file",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sreply%s [ts] <msg> Reply in thread (default: last)",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %stopic%s <text>    Set channel topic",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %stalk%s <user>  Open DM with user (alias: open)",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sopen%s <user>  Alias for talk (DM/MPDM)",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sdebug%s [on|off] Open debug buffer / toggle",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %squeue%s        Show HTTP request queue status",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sversion%s      Plugin version (+ info)",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sinfo%s         Status: queue, options, workspaces",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %smute%s         Mute current channel",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sunmute%s       Unmute current channel",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sstatus%s [emoji [text]|-delete|dnd|away|active]",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %screate%s [-private] <name>  Create channel",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sinvite%s <user> Invite user to current channel",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sshowmuted%s    List muted channels",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sdistracting%s  Toggle distracting mark",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %snodistractions%s Hide/show distracting",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sslash%s /cmd   Slack slash command",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %saway%s         Mark as away",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sback%s         Mark as active",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %shide%s         Hide current channel",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sshow%s [#ch]   Unhide current or reopen channel",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %slabel%s <text>  Set buffer label",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sthread%s <ts>   Open thread by parent ts",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sreact%s [ts] <emoji>  Add reaction (default: last msg)",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sunreact%s [ts] <emoji> Remove reaction",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %steams%s         List teams/workspaces",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %slinkarchive%s [ts] Get permalink (default: last msg)",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %ssubscribe%s     Subscribe to thread",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sunsubscribe%s   Unsubscribe from thread",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %spin%s [ts]      Pin message (default: last)",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sunpin%s [ts]    Unpin message",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sstar%s [ts]     Star message",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sunstar%s [ts]   Unstar message",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %ssearch%s <query> Search messages",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %swhois%s <user>  User info (+ live presence)",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sjoin%s <#ch|id> Join a channel",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %sleave%s [id]    Leave channel (alias: part)",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %srefresh%s       Re-fetch users + emoji",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %snames%s         Refresh channel nicklist",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %shelp%s         Show this help",
                        weechat_color("cyan"), weechat_color("reset"));
    }
    else
    {
        weechat_printf(buffer, "%sweeslack: unknown command '%s'. Try /cslack help",
                        weechat_prefix("error"), argv[1]);
    }

    return WEECHAT_RC_OK;
}

static int
weeslack_config_init(void)
{
    weeslack_config.file = weechat_config_new("weeslack", NULL, NULL, NULL);
    if (!weeslack_config.file)
        return WEECHAT_RC_ERROR;

    /* workspace section */
    weeslack_config.section_workspace = weechat_config_new_section(
        weeslack_config.file, "workspace",
        1, 1,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    if (!weeslack_config.section_workspace)
        return WEECHAT_RC_ERROR;

    weeslack_config.token = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_workspace,
        "token", "string",
        "Slack API token(s): xoxp/xoxb/xoxs or xoxc-token:cookie. "
        "Comma-separated for multiple teams (first=default, then ws1…; "
        "extra teams connect staggered 3s apart)",
        NULL, 0, 0, "", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    if (!weeslack_config.token)
        return WEECHAT_RC_ERROR;

    weeslack_config.auto_connect = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_workspace,
        "auto_connect", "boolean",
        "Connect automatically on plugin load when a valid token is set "
        "(also respects WeeChat auto_connect / -a; use /cslack connect "
        "manually if off)",
        NULL, 0, 0, "on", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    if (!weeslack_config.auto_connect)
        return WEECHAT_RC_ERROR;

    weeslack_config.server_aliases = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_workspace,
        "server_aliases", "string",
        "Comma-separated subdomain:alias pairs (e.g. acme:Work). "
        "Replaces the team display name after rtm.connect",
        NULL, 0, 0, "", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    if (!weeslack_config.server_aliases)
        return WEECHAT_RC_ERROR;

    /* look section */
    weeslack_config.section_look = weechat_config_new_section(
        weeslack_config.file, "look",
        1, 1,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    if (!weeslack_config.section_look)
        return WEECHAT_RC_ERROR;

    weeslack_config.render_bold_as = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "render_bold_as", "string",
        "WeeChat color/attr name for *bold* text (default: bold)",
        NULL, 0, 0, "bold", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.render_italic_as = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "render_italic_as", "string",
        "WeeChat color/attr name for _italic_ text (default: italic)",
        NULL, 0, 0, "italic", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.render_strikethrough_as = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "render_strikethrough_as", "string",
        "Strikethrough style: weechat (red), irc (lightred), or any WeeChat color name",
        NULL, 0, 0, "weechat", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.thread_messages_in_channel = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "thread_messages_in_channel", "boolean",
        "Show thread messages in the parent channel buffer",
        NULL, 0, 0, "off", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.thread_broadcast_prefix = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "thread_broadcast_prefix", "string",
        "Prefix for thread broadcast messages",
        NULL, 0, 0, "broadcast", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.short_buffer_names = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "short_buffer_names", "boolean",
        "Use short buffer names (e.g. #general instead of server.#general)",
        NULL, 0, 0, "on", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.channel_name_typing_indicator = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "channel_name_typing_indicator", "boolean",
        "Also show remote typing in the buffer title (WeeChat bar item "
        "\"typing\" is always driven via typing_set_nick; enable "
        "typing.look.enabled_nicks and put \"typing\" in a bar)",
        NULL, 0, 0, "on", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.emoji_render_mode = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "emoji_render_mode", "integer",
        "How to render emoji: 0=emoji, 1=slack_shortcodes, 2=text",
        NULL, 0, 2, 0, NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.download_path = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "download_path", "string",
        "Download root (empty = $XDG_DOWNLOAD_DIR or ~/Downloads). "
        "Files: <root>/weeslack/<origin>/<YYYY-MM-DD>/<file> "
        "(Xepher-style; date avoids overwrites)",
        NULL, 0, 0, "", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.auto_download_files = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "auto_download_files", "boolean",
        "Automatically download file attachments from live messages into "
        "<root>/weeslack/<team.channel>/<YYYY-MM-DD>/",
        NULL, 0, 0, "on", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.icat_enabled = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "icat_enabled", "boolean",
        "Show image previews via Kitty graphics (/icat from weechat-icat "
        "icat.py). Only runs when the /icat command is registered — "
        "never spams buffers if the script is not loaded",
        NULL, 0, 0, "on", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.never_away = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "never_away", "boolean",
        "Periodically set presence to active (every 5 min) so you never appear away",
        NULL, 0, 0, "off", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.send_typing_notice = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "send_typing_notice", "boolean",
        "Send typing indicator to Slack while composing in a channel buffer",
        NULL, 0, 0, "on", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.use_full_names = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "use_full_names", "boolean",
        "Prefer real names over display names in the nick column",
        NULL, 0, 0, "off", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.external_user_suffix = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "external_user_suffix", "string",
        "Suffix for external/shared-channel users (e.g. *)",
        NULL, 0, 0, "*", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.show_reaction_nicks = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "show_reaction_nicks", "boolean",
        "Show nick list in reaction suffix instead of count",
        NULL, 0, 0, "off", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.distracting_channels = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "distracting_channels", "string",
        "Comma-separated buffer full_names marked distracting (for /cslack nodistractions)",
        NULL, 0, 0, "", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.history_fetch_count = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "history_fetch_count", "integer",
        "Messages per history page (1–1000; still capped to a few pages)",
        NULL, 1, 1000, "100", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.group_name_prefix = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "group_name_prefix", "string",
        "Short-name prefix for private channels/groups (default &)",
        NULL, 0, 0, "&", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.switch_buffer_on_join = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "switch_buffer_on_join", "boolean",
        "Switch to channel/thread buffer when joining or opening a thread",
        NULL, 0, 0, "on", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.unhide_buffers_with_activity = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "unhide_buffers_with_activity", "boolean",
        "Unhide a hidden buffer when a new live message arrives (if not muted)",
        NULL, 0, 0, "off", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.muted_channels_activity = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "muted_channels_activity", "integer",
        "Muted channel activity: 0=none, 1=personal_highlights, 2=all_highlights, 3=all",
        "none|personal_highlights|all_highlights|all",
        0, 3, "personal_highlights", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.map_underline_to = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "map_underline_to", "string",
        "Map WeeChat underline (\\x1f) to this Slack marker when sending (default _ for italic)",
        NULL, 0, 0, "_", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.link_previews = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "link_previews", "boolean",
        "Show website link-preview attachments on messages",
        NULL, 0, 0, "on", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.notify_subscribed_threads = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "notify_subscribed_threads", "integer",
        "Server-buffer notice for subscribed threads: 0=auto, 1=always, 2=never",
        "auto|true|false",
        0, 2, "auto", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.colorize_attachments = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "colorize_attachments", "integer",
        "Attachment colorize: 0=none, 1=prefix only, 2=whole line",
        "none|prefix|all",
        0, 2, "prefix", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.shared_name_prefix = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "shared_name_prefix", "string",
        "Short-name prefix for shared/external channels (empty = same as normal)",
        NULL, 0, 0, "%", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.auto_open_threads = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "auto_open_threads", "integer",
        "Auto-open thread buffers on live replies: "
        "0=off, 1=subscribed or personal @mention, "
        "2=all replies (opens many buffers + fetch_replies — rate-limit risk)",
        "off|subscribed|all",
        0, 2, "off", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.show_buflist_presence = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "show_buflist_presence", "boolean",
        "Prefix DM short names with + when the peer is active "
        "(space when away), wee-slack style",
        NULL, 0, 0, "on", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.notify_usergroup_handle_updated = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "notify_usergroup_handle_updated", "boolean",
        "Print a notice on the team buffer when a usergroup handle changes",
        NULL, 0, 0, "off", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.unfurl_ignore_alt_text = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "unfurl_ignore_alt_text", "boolean",
        "When on, show raw Slack link targets instead of |alt labels",
        NULL, 0, 0, "off", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.unfurl_auto_link_display = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "unfurl_auto_link_display", "string",
        "How to show <url|label> links: both, text, or url",
        NULL, 0, 0, "both", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.debug_mode = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "debug_mode", "boolean",
        "Open a weeslack.debug buffer and print plugin debug lines there",
        NULL, 0, 0, "off", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.record_events = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "record_events", "boolean",
        "Append RTM frames and HTTP API responses (truncated, no tokens) to "
        "${weechat_data_dir}/weeslack/events/<domain>-YYYYMMDD.jsonl",
        NULL, 0, 0, "off", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.background_load_all_history = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "background_load_all_history", "boolean",
        "After connect quiet period, queue history for open member channels "
        "on the slow queue (default off — rate-limit sensitive; prefer "
        "lazy load on focus)",
        NULL, 0, 0, "off", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.background_history_max = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "background_history_max", "integer",
        "Max channels to queue for background history after connect "
        "(0 = no extra cap beyond open member channels, hard max 200)",
        NULL, 0, 200, "40", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.history_max_pages = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "history_max_pages", "integer",
        "Max conversations.history pages per channel load (each page uses "
        "history_fetch_count messages; hard max 20)",
        NULL, 1, 20, "5", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.members_max_pages = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "members_max_pages", "integer",
        "Max conversations.members pages per channel (200 members/page). "
        "0 = unlimited (until no cursor; still slow queue — can be heavy). "
        "Soft max 500. Default 3",
        NULL, 0, 500, "3", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.slack_timeout = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "slack_timeout", "integer",
        "Timeout in milliseconds for Slack HTTP (libcurl multi) and "
        "binary upload/download (min effective 5000)",
        NULL, 5000, 600000, "30000", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.debug_level = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "debug_level", "integer",
        "When debug_mode is on, print messages with level <= this value "
        "(1=important … 5=noisy; RTM snippets use 3)",
        NULL, 1, 5, "3", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.leave_channel_on_buffer_close = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_look,
        "leave_channel_on_buffer_close", "boolean",
        "When closing a channel/DM buffer, also leave/close it on Slack "
        "(conversations.leave / conversations.close). Default off — "
        "closing a buffer only hides it locally",
        NULL, 0, 0, "off", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    /* color section */
    weeslack_config.section_color = weechat_config_new_section(
        weeslack_config.file, "color",
        1, 1,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    if (!weeslack_config.section_color)
        return WEECHAT_RC_ERROR;

    weeslack_config.color_typing_notice = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_color,
        "typing_notice", "color",
        "Color for typing notifications",
        NULL, 0, 0, "cyan", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.color_deleted = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_color,
        "deleted", "color",
        "Color for deleted messages",
        NULL, 0, 0, "red", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.color_edited_suffix = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_color,
        "edited_suffix", "color",
        "Color for edited message suffix",
        NULL, 0, 0, "yellow", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.color_thread_suffix = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_color,
        "thread_suffix", "string",
        "Color for thread suffix: 'multiple' (nick color) or fixed color name",
        NULL, 0, 0, "cyan", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.color_reaction_suffix = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_color,
        "reaction_suffix", "color",
        "Color for reaction suffix brackets",
        NULL, 0, 0, "darkgray", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.color_reaction_suffix_added_by_you = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_color,
        "reaction_suffix_added_by_you", "color",
        "Color for reactions that include you",
        NULL, 0, 0, "lightgreen", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.colorize_private_chats = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_color,
        "colorize_private_chats", "boolean",
        "Use nick colors for private messages",
        NULL, 0, 0, "on", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    weeslack_config.color_buflist_muted_channels = weechat_config_new_option(
        weeslack_config.file, weeslack_config.section_color,
        "buflist_muted_channels", "color",
        "Color for muted channels in buflist",
        NULL, 0, 0, "darkgray", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    return weechat_config_read(weeslack_config.file);
}

static int
weeslack_nick_completion_cb(const void *pointer, void *data,
                            const char *completion_item,
                            struct t_gui_buffer *buffer,
                            struct t_gui_completion *completion)
{
    struct t_gui_buffer *buf;
    struct t_infolist *list;
    int from_nicklist = 0;

    (void) pointer;
    (void) data;
    (void) completion_item;

    buf = weeslack_cmd_buffer(buffer);
    /* Prefer nicks already in this buffer's nicklist (channel-scoped) */
    if (buf)
    {
        list = weechat_infolist_get("nicklist", buf, "");
        if (list)
        {
            while (weechat_infolist_next(list))
            {
                if (strcmp(weechat_infolist_string(list, "type"), "nick") == 0)
                {
                    const char *name = weechat_infolist_string(list, "name");
                    char at[160];

                    if (name && name[0])
                    {
                        weechat_completion_list_add(completion, name, 0,
                                                     WEECHAT_LIST_POS_SORT);
                        snprintf(at, sizeof(at), "@%s", name);
                        weechat_completion_list_add(completion, at, 0,
                                                     WEECHAT_LIST_POS_SORT);
                        from_nicklist = 1;
                    }
                }
            }
            weechat_infolist_free(list);
        }
    }

    if (!from_nicklist)
    {
        struct t_slack_user *user;
        struct t_weeslack_workspace *ws = weeslack_workspace_from_buffer(buf);

        for (user = slack_user_list_global(); user; user = user->next)
        {
            const char *name = slack_user_best_name(user);
            char at[160];

            if (!name || !name[0])
                continue;
            /* Multi-team: only complete nicks from the focused workspace. */
            if (ws && user->workspace && user->workspace != ws)
                continue;
            if (slack_user_hide_from_nicklist(user))
                continue;
            weechat_completion_list_add(completion, name, 0,
                                         WEECHAT_LIST_POS_SORT);
            /* @nick for complete_next / mention completion (wee-slack) */
            snprintf(at, sizeof(at), "@%s", name);
            weechat_completion_list_add(completion, at, 0,
                                         WEECHAT_LIST_POS_SORT);
        }
    }

    return WEECHAT_RC_OK;
}

static const char *slack_emoji_shortcodes[] = {
    "thumbsup", "thumbsdown", "heart", "fire", "rocket", "star",
    "clap", "wave", "smile", "laughing", "wink", "thinking_face",
    "neutral_face", "disappointed", "cry", "scream", "angry",
    "tada", "party_parrot", "thumbsup", "ok_hand", "v",
    "raised_hands", "pray", "muscle", "eyes", "poop",
    "ghost", "skull", "hundred", "sparkles", "zap",
    "sun", "moon", "rainbow", "snowflake", "coffee",
    "pizza", "beer", "wine", "bomb", "bulb",
    "check", "x", "white_check_mark", "x", "warning",
    "question", "exclamation", "info", "link", "lock",
    "pushpin", "pin", "bookmark", "memo", "pencil",
    NULL
};

static void
weeslack_emoji_completion_add_cb(const char *name, const char *unicode,
                                 void *data)
{
    struct t_gui_completion *completion = data;
    char buf[80];

    (void)unicode;
    if (!name || !name[0] || !completion)
        return;
    /* Skip skin-tone compounds in tab completion (noisy). */
    if (strstr(name, "::"))
        return;
    snprintf(buf, sizeof(buf), ":%s:", name);
    weechat_completion_list_add(completion, buf, 0, WEECHAT_LIST_POS_SORT);
}

/*
 * Topic completion for /topic and /cslack topic (wee-slack irc_channel_topic).
 * Offers the current channel topic/purpose as a completion candidate.
 */
static int
weeslack_topic_completion_cb(const void *pointer, void *data,
                              const char *completion_item,
                              struct t_gui_buffer *buffer,
                              struct t_gui_completion *completion)
{
    struct t_gui_buffer *buf;
    const char *channel_id;
    struct t_slack_channel *ch;
    const char *topic;

    (void)pointer;
    (void)data;
    (void)completion_item;

    buf = weeslack_cmd_buffer(buffer);
    if (!buf || !completion)
        return WEECHAT_RC_OK;

    channel_id = weechat_buffer_get_string(buf, "localvar_slack_channel_id");
    if (!channel_id)
        return WEECHAT_RC_OK;
    ch = slack_channel_search(channel_id);
    if (!ch)
        return WEECHAT_RC_OK;

    topic = slack_channel_display_topic(ch);
    if (topic && topic[0])
        weechat_completion_list_add(completion, topic, 0,
                                     WEECHAT_LIST_POS_BEGINNING);

    return WEECHAT_RC_OK;
}

static int
weeslack_emoji_completion_cb(const void *pointer, void *data,
                              const char *completion_item,
                              struct t_gui_buffer *buffer,
                              struct t_gui_completion *completion)
{
    (void) pointer;
    (void) data;
    (void) completion_item;
    (void) buffer;

    if (slack_event_weemoji_count() > 0)
    {
        slack_event_weemoji_foreach(weeslack_emoji_completion_add_cb,
                                    completion);
    }
    else
    {
        int i;
        for (i = 0; slack_emoji_shortcodes[i]; i++)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), ":%s:", slack_emoji_shortcodes[i]);
            weechat_completion_list_add(completion, buf, 0,
                                         WEECHAT_LIST_POS_SORT);
        }
    }

    return WEECHAT_RC_OK;
}

static int
weeslack_channel_completion_cb(const void *pointer, void *data,
                               const char *completion_item,
                               struct t_gui_buffer *buffer,
                               struct t_gui_completion *completion)
{
    (void) pointer;
    (void) data;
    (void) completion_item;
    (void) buffer;

    struct t_slack_channel *channel;
    for (channel = slack_channel_list_global();
         channel;
         channel = channel->next)
    {
        if (channel->type == SLACK_CHANNEL_TYPE_CHANNEL && channel->name)
        {
            char name_buf[256];
            snprintf(name_buf, sizeof(name_buf), "#%s", channel->name);
            weechat_completion_list_add(completion, name_buf, 0,
                                         WEECHAT_LIST_POS_SORT);
        }
        else if (channel->name)
        {
            weechat_completion_list_add(completion, channel->name, 0,
                                         WEECHAT_LIST_POS_SORT);
        }
    }

    return WEECHAT_RC_OK;
}

static int
weeslack_usergroup_completion_cb(const void *pointer, void *data,
                                  const char *completion_item,
                                  struct t_gui_buffer *buffer,
                                  struct t_gui_completion *completion)
{
    (void) pointer;
    (void) data;
    (void) completion_item;
    (void) buffer;

    struct t_slack_subteam *st;
    for (st = slack_subteam_list_global(); st; st = st->next)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "@%s", st->handle ? st->handle : st->name);
        weechat_completion_list_add(completion, buf, 0,
                                     WEECHAT_LIST_POS_SORT);
    }

    return WEECHAT_RC_OK;
}

static int
weeslack_thread_completion_cb(const void *pointer, void *data,
                               const char *completion_item,
                               struct t_gui_buffer *buffer,
                               struct t_gui_completion *completion)
{
    (void) pointer;
    (void) data;
    (void) completion_item;

    /* Only offer completions from thread-capable buffers */
    const char *channel_id = weeslack_cmd_channel_id(buffer);
    struct t_slack_channel *parent;
    struct t_slack_message *m;
    struct t_slack_channel *ch;

    if (!channel_id)
        return WEECHAT_RC_OK;

    parent = slack_channel_search(channel_id);
    if (!parent)
        return WEECHAT_RC_OK;

    /* Offer $hash for parents that have replies */
    for (m = parent->messages; m; m = m->next)
    {
        if (m->reply_count > 0 && m->hash)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "$%s", m->hash);
            weechat_completion_list_add(completion, buf, 0,
                                         WEECHAT_LIST_POS_SORT);
        }
    }

    /* Search all channels for thread children of this parent */
    for (ch = slack_channel_list_global(); ch; ch = ch->next)
    {
        if (ch->type == SLACK_CHANNEL_TYPE_THREAD && ch->id)
        {
            /* thread IDs: "thread_<parent_id>_<thread_ts>" */
            const char *prefix = "thread_";
            size_t prefix_len = strlen(prefix);
            if (strncmp(ch->id, prefix, prefix_len) == 0)
            {
                const char *rest = ch->id + prefix_len;
                const char *underscore = strchr(rest, '_');
                if (underscore && strncmp(rest, channel_id,
                                          (size_t)(underscore - rest)) == 0)
                {
                    weechat_completion_list_add(completion, ch->id, 0,
                                                 WEECHAT_LIST_POS_SORT);
                }
            }
        }
    }

    return WEECHAT_RC_OK;
}

static int
weeslack_buffer_switch_cb(const void *pointer, void *data,
                           const char *signal, const char *type_data,
                           void *signal_data)
{
    (void) pointer;
    (void) data;
    (void) signal;
    (void) type_data;

    struct t_gui_buffer *buffer = (struct t_gui_buffer *)signal_data;
    if (!buffer)
        return WEECHAT_RC_OK;

    const char *channel_id = weeslack_cmd_channel_id(buffer);
    const char *slack_type = weechat_buffer_get_string(buffer,
        "localvar_slack_type");

    if (!channel_id || !slack_type)
        return WEECHAT_RC_OK;

    /* Mark the channel as read when switching to it */
    struct t_slack_channel *channel = slack_channel_search(channel_id);
    if (channel)
    {
        struct t_weeslack_workspace *ws = weeslack_workspace_from_buffer(buffer);
        if (ws && ws->connected)
        {
            /* Ensure topic bar shows topic/purpose on focus. */
            {
                const char *disp = slack_channel_display_topic(channel);
                const char *cur = weechat_buffer_get_string(buffer, "title");
                if (!(cur && strstr(cur, "typing")))
                    weechat_buffer_set(buffer, "title", disp ? disp : "");
                /* conversations.list sometimes omits topic — fill in once */
                if (!disp && channel->type != SLACK_CHANNEL_TYPE_DM &&
                    channel->type != SLACK_CHANNEL_TYPE_THREAD &&
                    !slack_event_in_bootstrap_quiet())
                    slack_event_fetch_channel_info(ws, channel);
            }
            /*
             * During bootstrap, creating many buffers fires buffer_switch for
             * each — that must not enqueue history for all of them.
             */
            if (!slack_event_in_bootstrap_quiet())
            {
                if (channel->history_state == 0)
                    slack_event_fetch_history(ws, channel);
                if (channel->type == SLACK_CHANNEL_TYPE_CHANNEL ||
                    channel->type == SLACK_CHANNEL_TYPE_GROUP ||
                    channel->type == SLACK_CHANNEL_TYPE_MPDM)
                    slack_event_fetch_members(ws, channel);
            }
            /* mark is low-priority / dropped under API cooldown */
            slack_event_mark_read(ws, channel);
        }
        else
        {
            channel->unread_count = 0;
            slack_buffer_clear_hotlist(buffer);
        }
    }

    return WEECHAT_RC_OK;
}

/* ============================================================
 * Hdata callbacks
 * ============================================================ */

static struct t_hdata *
weeslack_hdata_channel_cb(const void *pointer, void *data,
                           const char *hdata_name)
{
    (void) pointer;
    (void) data;

    struct t_hdata *hdata = weechat_hdata_new(
        hdata_name, "prev", "next", 0, 0, NULL, NULL);
    if (!hdata)
        return NULL;

    WEECHAT_HDATA_VAR(struct t_slack_channel, id, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_channel, name, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_channel, topic, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_channel, purpose, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_channel, type, INTEGER, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_channel, is_member, INTEGER, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_channel, is_muted, INTEGER, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_channel, is_subscribed, INTEGER, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_channel, unread_count, INTEGER, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_channel, buffer, POINTER, 0, NULL, "gui_buffer");

    weechat_hdata_new_list(hdata, "cslack_channels",
                            slack_channel_list_global(),
                            WEECHAT_HDATA_LIST_CHECK_POINTERS);

    return hdata;
}

static struct t_hdata *
weeslack_hdata_user_cb(const void *pointer, void *data,
                        const char *hdata_name)
{
    (void) pointer;
    (void) data;

    struct t_hdata *hdata = weechat_hdata_new(
        hdata_name, "prev", "next", 0, 0, NULL, NULL);
    if (!hdata)
        return NULL;

    WEECHAT_HDATA_VAR(struct t_slack_user, id, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_user, name, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_user, real_name, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_user, display_name, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_user, color, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_user, presence, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_user, status_emoji, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_user, status_text, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_user, deleted, INTEGER, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_user, is_bot, INTEGER, 0, NULL, NULL);

    weechat_hdata_new_list(hdata, "cslack_users", slack_user_list_global(),
                            WEECHAT_HDATA_LIST_CHECK_POINTERS);

    return hdata;
}

static struct t_hdata *
weeslack_hdata_message_cb(const void *pointer, void *data,
                           const char *hdata_name)
{
    (void) pointer;
    (void) data;

    struct t_hdata *hdata = weechat_hdata_new(
        hdata_name, "prev", "next", 0, 0, NULL, NULL);
    if (!hdata)
        return NULL;

    WEECHAT_HDATA_VAR(struct t_slack_message, user_id, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_message, text, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_message, subtype, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_message, thread_ts, STRING, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_message, reply_count, INTEGER, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_message, is_deleted, INTEGER, 0, NULL, NULL);
    WEECHAT_HDATA_VAR(struct t_slack_message, is_edited, INTEGER, 0, NULL, NULL);

    return hdata;
}

/* ============================================================
 * Infolist callbacks
 * ============================================================ */

static struct t_infolist *
weeslack_infolist_channel_cb(const void *pointer, void *data,
                              const char *infolist_name,
                              void *obj_pointer,
                              const char *arguments)
{
    (void) pointer;
    (void) data;
    (void) infolist_name;
    (void) arguments;

    struct t_infolist *infolist = weechat_infolist_new();
    if (!infolist)
        return NULL;

    struct t_slack_channel *channel;
    if (obj_pointer)
    {
        channel = (struct t_slack_channel *)obj_pointer;
        struct t_infolist_item *item = weechat_infolist_new_item(infolist);
        if (item)
        {
            weechat_infolist_new_var_string(item, "id", channel->id);
            weechat_infolist_new_var_string(item, "name", channel->name);
            weechat_infolist_new_var_string(item, "topic", channel->topic);
            weechat_infolist_new_var_string(item, "purpose", channel->purpose);
            weechat_infolist_new_var_integer(item, "type", channel->type);
            weechat_infolist_new_var_integer(item, "is_member", channel->is_member);
            weechat_infolist_new_var_integer(item, "is_muted", channel->is_muted);
            weechat_infolist_new_var_integer(item, "unread_count", channel->unread_count);
            weechat_infolist_new_var_pointer(item, "buffer", channel->buffer);
        }
    }
    else
    {
        for (channel = slack_channel_list_global();
             channel;
             channel = channel->next)
        {
            struct t_infolist_item *item = weechat_infolist_new_item(infolist);
            if (!item)
                continue;

            weechat_infolist_new_var_string(item, "id", channel->id);
            weechat_infolist_new_var_string(item, "name", channel->name);
            weechat_infolist_new_var_string(item, "topic", channel->topic);
            weechat_infolist_new_var_string(item, "purpose", channel->purpose);
            weechat_infolist_new_var_integer(item, "type", channel->type);
            weechat_infolist_new_var_integer(item, "is_member", channel->is_member);
            weechat_infolist_new_var_integer(item, "is_muted", channel->is_muted);
            weechat_infolist_new_var_integer(item, "unread_count", channel->unread_count);
            weechat_infolist_new_var_pointer(item, "buffer", channel->buffer);
        }
    }

    return infolist;
}

static struct t_infolist *
weeslack_infolist_user_cb(const void *pointer, void *data,
                           const char *infolist_name,
                           void *obj_pointer,
                           const char *arguments)
{
    (void) pointer;
    (void) data;
    (void) infolist_name;
    (void) arguments;

    struct t_infolist *infolist = weechat_infolist_new();
    if (!infolist)
        return NULL;

    struct t_slack_user *user;
    if (obj_pointer)
    {
        user = (struct t_slack_user *)obj_pointer;
        struct t_infolist_item *item = weechat_infolist_new_item(infolist);
        if (item)
        {
            weechat_infolist_new_var_string(item, "id", user->id);
            weechat_infolist_new_var_string(item, "name", user->name);
            weechat_infolist_new_var_string(item, "real_name", user->real_name);
            weechat_infolist_new_var_string(item, "display_name", user->display_name);
            weechat_infolist_new_var_string(item, "color", user->color);
            weechat_infolist_new_var_string(item, "presence", user->presence);
            weechat_infolist_new_var_integer(item, "deleted", user->deleted);
            weechat_infolist_new_var_integer(item, "is_bot", user->is_bot);
        }
    }
    else
    {
        for (user = slack_user_list_global(); user; user = user->next)
        {
            struct t_infolist_item *item = weechat_infolist_new_item(infolist);
            if (!item)
                continue;

            weechat_infolist_new_var_string(item, "id", user->id);
            weechat_infolist_new_var_string(item, "name", user->name);
            weechat_infolist_new_var_string(item, "real_name", user->real_name);
            weechat_infolist_new_var_string(item, "display_name", user->display_name);
            weechat_infolist_new_var_string(item, "color", user->color);
            weechat_infolist_new_var_string(item, "presence", user->presence);
            weechat_infolist_new_var_integer(item, "deleted", user->deleted);
            weechat_infolist_new_var_integer(item, "is_bot", user->is_bot);
        }
    }

    return infolist;
}

/* ============================================================
 * Upgrade file support
 * ============================================================ */

enum {
    UPGRADE_OBJECT_CHANNEL = 1,
    UPGRADE_OBJECT_END     = 0,
};

static int
weeslack_upgrade_write_cb(const void *pointer, void *data,
                           struct t_upgrade_file *upgrade_file,
                           int object_id)
{
    (void) pointer;
    (void) data;

    switch (object_id)
    {
        case UPGRADE_OBJECT_CHANNEL:
            {
                struct t_slack_channel *channel;
                for (channel = slack_channel_list_global();
                     channel;
                     channel = channel->next)
                {
                    struct t_infolist *infolist = weechat_infolist_new();
                    if (!infolist)
                        continue;

                    struct t_infolist_item *item = weechat_infolist_new_item(infolist);
                    if (item)
                    {
                        weechat_infolist_new_var_string(item, "id", channel->id);
                        weechat_infolist_new_var_string(item, "name", channel->name);
                        weechat_infolist_new_var_integer(item, "type", channel->type);
                        weechat_infolist_new_var_integer(item, "is_subscribed",
                                                         channel->is_subscribed);
                    }

                    weechat_upgrade_write_object(upgrade_file,
                                                  UPGRADE_OBJECT_CHANNEL,
                                                  infolist);
                    weechat_infolist_free(infolist);
                }
            }
            break;
    }

    return WEECHAT_RC_OK;
}

static int
weeslack_upgrade_read_cb(const void *pointer, void *data,
                          struct t_upgrade_file *upgrade_file,
                          int object_id,
                          struct t_infolist *infolist)
{
    (void) pointer;
    (void) data;
    (void) upgrade_file;

    if (!infolist)
        return WEECHAT_RC_OK;

    if (object_id == UPGRADE_OBJECT_CHANNEL)
    {
        if (weechat_infolist_next(infolist))
        {
            const char *id = weechat_infolist_string(infolist, "id");
            const char *name = weechat_infolist_string(infolist, "name");
            int type = weechat_infolist_integer(infolist, "type");
            int is_subscribed = weechat_infolist_integer(infolist, "is_subscribed");

            if (id && name)
            {
                struct t_slack_channel *channel = slack_channel_search(id);
                if (!channel)
                    channel = slack_channel_new(id, name,
                                                 (enum slack_channel_type)type);
                if (channel)
                    channel->is_subscribed = is_subscribed;
            }
        }
    }

    return WEECHAT_RC_OK;
}

/* True if buffer belongs to this plugin (channel or server). */
static int
weeslack_buffer_is_ours(struct t_gui_buffer *buffer)
{
    const char *plugin;

    if (!buffer)
        return 0;
    plugin = weechat_buffer_get_string(buffer, "plugin");
    return (plugin && strcmp(plugin, "weeslack") == 0) ? 1 : 0;
}

/* Mark all Slack channels read (wee-slack /input set_unread). */
static void
weeslack_mark_all_read(void)
{
    struct t_weeslack_workspace *ws;
    struct t_slack_channel *ch;

    for (ch = slack_channel_list_global(); ch; ch = ch->next)
    {
        if (!ch->id || ch->type == SLACK_CHANNEL_TYPE_THREAD)
            continue;
        ws = ch->workspace;
        if (!ws && ch->buffer)
            ws = weeslack_workspace_from_buffer(ch->buffer);
        if (!ws || !ws->connected)
            continue;
        ch->unread_count = 0;
        if (ch->buffer)
            slack_buffer_clear_hotlist(ch->buffer);
        slack_event_mark_read(ws, ch);
    }
}

static void
weeslack_mark_buffer_read(struct t_gui_buffer *buffer)
{
    struct t_weeslack_workspace *ws;
    const char *channel_id;
    struct t_slack_channel *ch;

    if (!buffer || !weeslack_buffer_is_ours(buffer))
        return;

    ws = weeslack_workspace_from_buffer(buffer);
    if (!ws || !ws->connected)
        return;

    channel_id = weechat_buffer_get_string(buffer, "localvar_slack_channel_id");
    if (!channel_id || !channel_id[0])
        return;
    ch = slack_channel_search(channel_id);
    if (!ch)
        return;
    ch->unread_count = 0;
    slack_buffer_clear_hotlist(buffer);
    slack_event_mark_read(ws, ch);
}

/*
 * /input set_unread — all workspaces' Slack buffers (do not eat: IRC still runs).
 * /input set_unread_current_buffer and /buffer set unread — this buffer only.
 */
static int
weeslack_set_unread_all_cb(const void *pointer, void *data,
                            struct t_gui_buffer *buffer, const char *command)
{
    (void)pointer;
    (void)data;
    (void)buffer;
    (void)command;
    weeslack_mark_all_read();
    return WEECHAT_RC_OK;
}

static int
weeslack_set_unread_current_cb(const void *pointer, void *data,
                                struct t_gui_buffer *buffer,
                                const char *command)
{
    (void)pointer;
    (void)data;
    (void)command;

    if (!weeslack_buffer_is_ours(buffer))
        return WEECHAT_RC_OK;
    weeslack_mark_buffer_read(buffer);
    return WEECHAT_RC_OK;
}

/*
 * wee-slack complete_next: if the word under the cursor is a nick without @,
 * insert @ so WeeChat completion can use @nick entries.
 */
static int
weeslack_complete_next_cb(const void *pointer, void *data,
                           struct t_gui_buffer *buffer, const char *command)
{
    const char *input;
    int pos, len, start, end;
    char word[128];
    struct t_gui_nick *nick;
    char *new_input;
    size_t new_len;

    (void)pointer;
    (void)data;
    (void)command;

    if (!buffer || !weeslack_buffer_is_ours(buffer))
        return WEECHAT_RC_OK;

    input = weechat_buffer_get_string(buffer, "input");
    if (!input || !input[0])
        return WEECHAT_RC_OK;

    len = weechat_buffer_get_integer(buffer, "input_length");
    pos = weechat_buffer_get_integer(buffer, "input_pos") - 1;
    if (len <= 0)
        return WEECHAT_RC_OK;
    if (pos < 0)
        pos = 0;
    if (pos >= len)
        pos = len - 1;

    /* If on non-word, walk left toward something completable */
    while (pos >= 0 && input[pos] != '@' &&
           !((input[pos] >= 'A' && input[pos] <= 'Z') ||
             (input[pos] >= 'a' && input[pos] <= 'z') ||
             (input[pos] >= '0' && input[pos] <= '9') ||
             input[pos] == '_' || input[pos] == '-' || input[pos] == '.'))
        pos--;
    if (pos < 0)
        return WEECHAT_RC_OK;

    start = pos;
    while (start > 0)
    {
        char c = input[start - 1];
        if (c == '@' ||
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
            start--;
        else
            break;
    }
    end = pos + 1;
    while (end < len)
    {
        char c = input[end];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
            end++;
        else
            break;
    }

    if (end <= start || (size_t)(end - start) >= sizeof(word))
        return WEECHAT_RC_OK;
    if (input[start] == '@')
        return WEECHAT_RC_OK; /* already @-prefixed */

    memcpy(word, input + start, (size_t)(end - start));
    word[end - start] = '\0';

    nick = weechat_nicklist_search_nick(buffer, NULL, word);
    if (!nick)
        return WEECHAT_RC_OK;

    new_len = (size_t)len + 2;
    new_input = malloc(new_len);
    if (!new_input)
        return WEECHAT_RC_OK;
    if (start > 0)
        memcpy(new_input, input, (size_t)start);
    new_input[start] = '@';
    memcpy(new_input + start + 1, input + start, (size_t)(len - start));
    new_input[len + 1] = '\0';

    weechat_buffer_set(buffer, "input", new_input);
    {
        char posbuf[16];
        snprintf(posbuf, sizeof(posbuf), "%d",
                 weechat_buffer_get_integer(buffer, "input_pos") + 1);
        weechat_buffer_set(buffer, "input_pos", posbuf);
    }
    free(new_input);
    return WEECHAT_RC_OK_EAT;
}

/*
 * IRC-style command_run hooks on Slack buffers (wee-slack parity).
 * Return OK_EAT only for our buffers so IRC still works elsewhere.
 */
static int
weeslack_command_run_cb(const void *pointer, void *data,
                        struct t_gui_buffer *buffer, const char *command)
{
    struct t_weeslack_workspace *ws;
    const char *channel_id;
    const char *args;

    (void)pointer;
    (void)data;

    if (!command || !command[0] || !weeslack_buffer_is_ours(buffer))
        return WEECHAT_RC_OK;

    ws = weeslack_workspace_from_buffer(buffer);
    if (!ws || !ws->connected)
        return WEECHAT_RC_OK_EAT;

    channel_id = weechat_buffer_get_string(buffer, "localvar_slack_channel_id");
    args = strchr(command, ' ');
    if (args)
    {
        while (*args == ' ')
            args++;
    }
    else
        args = "";

    if (weechat_strncasecmp(command, "/me", 3) == 0 &&
        (command[3] == ' ' || command[3] == '\0'))
    {
        if (channel_id && args[0])
            slack_event_send_me_message(ws, channel_id, args, NULL);
        else
            weechat_printf(buffer, "%sweeslack: usage: /me <action>",
                            weechat_prefix("error"));
        return WEECHAT_RC_OK_EAT;
    }

    if (weechat_strncasecmp(command, "/join", 5) == 0 &&
        (command[5] == ' ' || command[5] == '\0'))
    {
        if (!args[0])
        {
            weechat_printf(buffer, "%sweeslack: usage: /join <#channel|id>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK_EAT;
        }
        slack_event_join_channel(ws, args);
        return WEECHAT_RC_OK_EAT;
    }

    if ((weechat_strncasecmp(command, "/query", 6) == 0 &&
         (command[6] == ' ' || command[6] == '\0')) ||
        (weechat_strncasecmp(command, "/msg", 4) == 0 &&
         (command[4] == ' ' || command[4] == '\0')))
    {
        char *copy, *who, *msg;
        int is_msg = (weechat_strncasecmp(command, "/msg", 4) == 0);

        if (!args[0])
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: %s <nick>[,nick2…] [message]",
                            weechat_prefix("error"),
                            is_msg ? "/msg" : "/query");
            return WEECHAT_RC_OK_EAT;
        }

        copy = strdup(args);
        if (!copy)
            return WEECHAT_RC_OK_EAT;

        who = copy;
        msg = NULL;
        if (is_msg)
        {
            /*
             * /msg nick[,n2] text — first token may contain commas for MPDM;
             * message starts after the first whitespace.
             */
            char *sp = strchr(copy, ' ');
            if (sp)
            {
                *sp = '\0';
                msg = sp + 1;
                while (*msg == ' ')
                    msg++;
            }
        }
        if (who[0] == '@')
            who++;

        if (who[0] && strcmp(who, "*") != 0)
            slack_event_open_dm(ws, who);

        if (msg && msg[0])
        {
            /* Best-effort: send to existing DM buffer by peer name. */
            struct t_slack_channel *ch;
            struct t_slack_user *u = slack_user_search(who);
            const char *uid = u && u->id ? u->id : who;
            int sent = 0;

            if (strcmp(who, "*") == 0 && channel_id)
            {
                slack_event_send_message(ws, channel_id, msg, NULL);
                sent = 1;
            }
            for (ch = slack_channel_list_global(); !sent && ch; ch = ch->next)
            {
                if (ch->type != SLACK_CHANNEL_TYPE_DM)
                    continue;
                if ((ch->user_id && strcmp(ch->user_id, uid) == 0) ||
                    (ch->name && weechat_strcasecmp(ch->name, who) == 0))
                {
                    slack_event_send_message(ws, ch->id, msg, NULL);
                    sent = 1;
                }
            }
        }

        free(copy);
        return WEECHAT_RC_OK_EAT;
    }

    if ((weechat_strncasecmp(command, "/part", 5) == 0 &&
         (command[5] == ' ' || command[5] == '\0')) ||
        (weechat_strncasecmp(command, "/leave", 6) == 0 &&
         (command[6] == ' ' || command[6] == '\0')))
    {
        const char *cid = args[0] ? args : channel_id;
        if (cid && cid[0] == '#')
            cid++;
        if (cid && cid[0] &&
            (cid[0] != 'C' && cid[0] != 'G' && cid[0] != 'D' && cid[0] != 'U'))
        {
            struct t_slack_channel *ch;
            for (ch = slack_channel_list_global(); ch; ch = ch->next)
            {
                if (ch->name && weechat_strcasecmp(ch->name, cid) == 0)
                {
                    cid = ch->id;
                    break;
                }
            }
        }
        if (cid && cid[0])
            slack_event_leave_channel(ws, cid);
        else
            weechat_printf(buffer, "%sweeslack: no channel to leave",
                            weechat_prefix("error"));
        return WEECHAT_RC_OK_EAT;
    }

    if (weechat_strncasecmp(command, "/topic", 6) == 0 &&
        (command[6] == ' ' || command[6] == '\0'))
    {
        const char *cid = channel_id;
        const char *topic = args;
        char *copy = NULL;

        /* /topic [#channel] [text|-delete] */
        if (args[0] == '#' || args[0] == '&')
        {
            char *sp;
            const char *name;

            copy = strdup(args);
            if (!copy)
                return WEECHAT_RC_OK_EAT;
            name = copy + 1;
            sp = strchr(copy, ' ');
            if (sp)
            {
                *sp = '\0';
                topic = sp + 1;
                while (*topic == ' ')
                    topic++;
            }
            else
                topic = "";
            {
                struct t_slack_channel *ch;
                cid = NULL;
                for (ch = slack_channel_list_global(); ch; ch = ch->next)
                {
                    if (ch->name && weechat_strcasecmp(ch->name, name) == 0)
                    {
                        cid = ch->id;
                        break;
                    }
                }
            }
            if (!cid)
            {
                weechat_printf(buffer, "%sweeslack: no such channel",
                                weechat_prefix("error"));
                free(copy);
                return WEECHAT_RC_OK_EAT;
            }
        }

        if (!cid)
        {
            weechat_printf(buffer, "%sweeslack: no channel for /topic",
                            weechat_prefix("error"));
            free(copy);
            return WEECHAT_RC_OK_EAT;
        }

        if (!topic[0])
        {
            struct t_slack_channel *ch = slack_channel_search(cid);
            weechat_printf(buffer, "%sweeslack: topic: %s",
                            weechat_prefix("network"),
                            (ch && ch->topic) ? ch->topic : "(none)");
        }
        else
        {
            if (strcmp(topic, "-delete") == 0)
                topic = "";
            slack_event_set_topic(ws, cid, topic);
        }
        free(copy);
        return WEECHAT_RC_OK_EAT;
    }

    if (weechat_strncasecmp(command, "/invite", 7) == 0 &&
        (command[7] == ' ' || command[7] == '\0'))
    {
        if (!args[0] || !channel_id)
        {
            weechat_printf(buffer,
                            "%sweeslack: usage: /invite <nick> [on channel]",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK_EAT;
        }
        slack_event_invite_user(ws, channel_id, args);
        return WEECHAT_RC_OK_EAT;
    }

    if (weechat_strncasecmp(command, "/away", 5) == 0 &&
        (command[5] == ' ' || command[5] == '\0'))
    {
        /* /away with no args or with message → mark away; /away - → back */
        if (args[0] == '-' && (args[1] == '\0' || args[1] == ' '))
            slack_event_set_presence(ws, "auto");
        else
            slack_event_set_presence(ws, "away");
        return WEECHAT_RC_OK_EAT;
    }

    if (weechat_strncasecmp(command, "/whois", 6) == 0 &&
        (command[6] == ' ' || command[6] == '\0'))
    {
        if (!args[0])
        {
            weechat_printf(buffer, "%sweeslack: usage: /whois <nick>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK_EAT;
        }
        {
            const char *nick = args;
            if (nick[0] == '@')
                nick++;
            slack_event_whois(ws, nick, buffer);
        }
        return WEECHAT_RC_OK_EAT;
    }

    return WEECHAT_RC_OK;
}

/* wee-slack never_away: set presence active every 5 minutes when enabled */
static int
weeslack_never_away_cb(const void *pointer, void *data, int remaining_calls)
{
    struct t_weeslack_workspace *ws;

    (void)pointer;
    (void)data;
    (void)remaining_calls;

    if (!weechat_config_boolean(weeslack_config.never_away))
        return WEECHAT_RC_OK;

    for (ws = weeslack_workspaces; ws; ws = ws->next_workspace)
    {
        if (ws->connected)
            slack_event_set_presence(ws, "auto");
    }
    return WEECHAT_RC_OK;
}

/* away / slack_away bar items (wee-slack) */
static char *
weeslack_away_bar_cb(const void *pointer, void *data,
                     struct t_gui_bar_item *item,
                     struct t_gui_window *window,
                     struct t_gui_buffer *buffer,
                     struct t_hashtable *extra_info)
{
    struct t_weeslack_workspace *ws;
    const char *color;
    char *out;
    int away = 0;
    int manual = 0;

    (void)pointer;
    (void)data;
    (void)item;
    (void)window;
    (void)buffer;
    (void)extra_info;

    for (ws = weeslack_workspaces; ws; ws = ws->next_workspace)
    {
        if (!ws->connected)
            continue;
        if (ws->my_manual_away ||
            (ws->my_presence && strcmp(ws->my_presence, "away") == 0))
        {
            away = 1;
            manual = ws->my_manual_away;
            break;
        }
    }
    if (!away)
        return NULL;

    color = "cyan";
    {
        struct t_config_option *opt = weechat_config_get("weechat.color.item_away");
        if (opt)
        {
            const char *c = weechat_config_string(opt);
            if (c && c[0])
                color = c;
        }
    }
    out = malloc(64);
    if (!out)
        return NULL;
    snprintf(out, 64, "%s%s%s",
             weechat_color(color),
             manual ? "manual away" : "auto away",
             weechat_color("reset"));
    return out;
}

/*
 * Cursor/mouse line hsignals (wee-slack): insert $hash, delete, reply, thread.
 * Bound on plugin init for @chat(weeslack.*).
 */
static int
weeslack_line_event_cb(const void *pointer, void *data,
                        const char *signal, struct t_hashtable *hashtable)
{
    const char *action = "message";
    const char *tags;
    struct t_gui_buffer *buf = NULL;
    char *tags_copy = NULL, *tok, *save = NULL;
    const char *ts_str = NULL;
    struct t_slack_channel *ch;
    struct t_slack_message *msg = NULL;
    char cmd[256];
    char hash_ref[40];

    (void)pointer;
    (void)data;

    if (!hashtable)
        return WEECHAT_RC_OK;

    if (signal)
    {
        if (strstr(signal, "delete"))
            action = "delete";
        else if (strstr(signal, "linkarchive"))
            action = "linkarchive";
        else if (strstr(signal, "reply"))
            action = "reply";
        else if (strstr(signal, "thread"))
            action = "thread";
        else if (strstr(signal, "mouse"))
            action = "auto";
    }

    tags = weechat_hashtable_get(hashtable, "_chat_line_tags");
    if (!tags)
        return WEECHAT_RC_OK;

    {
        const char *full = weechat_hashtable_get(hashtable, "_buffer_full_name");
        if (full && full[0])
        {
            struct t_hdata *hbuf = weechat_hdata_get("buffer");
            void *ptr = hbuf ? weechat_hdata_get_list(hbuf, "gui_buffers") : NULL;
            while (ptr)
            {
                const char *fn = weechat_hdata_string(hbuf, ptr, "full_name");
                if (fn && strcmp(fn, full) == 0)
                {
                    buf = ptr;
                    break;
                }
                ptr = weechat_hdata_move(hbuf, ptr, 1);
            }
        }
    }
    if (!buf)
        buf = weechat_current_buffer();
    if (!buf)
        return WEECHAT_RC_OK;

    tags_copy = strdup(tags);
    if (!tags_copy)
        return WEECHAT_RC_OK;
    for (tok = strtok_r(tags_copy, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save))
    {
        if (strncmp(tok, "slack_ts_", 9) == 0)
        {
            ts_str = tok + 9;
            break;
        }
    }
    free(tags_copy);
    if (!ts_str)
        return WEECHAT_RC_OK;

    {
        const char *cid = weechat_buffer_get_string(buf,
                                                    "localvar_slack_channel_id");
        if (!cid)
            return WEECHAT_RC_OK;
        ch = slack_channel_search(cid);
        if (!ch)
            return WEECHAT_RC_OK;
        msg = slack_message_search(ch->messages, slack_ts_new(ts_str));
    }
    if (!msg)
        return WEECHAT_RC_OK;

    if (msg->hash && msg->hash[0])
        snprintf(hash_ref, sizeof(hash_ref), "$%s", msg->hash);
    else
    {
        char *s = slack_ts_to_string(msg->ts);
        snprintf(hash_ref, sizeof(hash_ref), "%s", s ? s : "");
        free(s);
    }
    if (!hash_ref[0])
        return WEECHAT_RC_OK;

    if (strcmp(action, "auto") == 0 || strcmp(action, "message") == 0)
    {
        weechat_command(buf, "/cursor stop");
        snprintf(cmd, sizeof(cmd), "/input insert %s", hash_ref);
        weechat_command(buf, cmd);
    }
    else if (strcmp(action, "delete") == 0)
    {
        snprintf(cmd, sizeof(cmd), "/input send %ss///", hash_ref);
        weechat_command(buf, cmd);
    }
    else if (strcmp(action, "linkarchive") == 0)
    {
        weechat_command(buf, "/cursor stop");
        snprintf(cmd, sizeof(cmd), "/cslack linkarchive %s", hash_ref);
        weechat_command(buf, cmd);
    }
    else if (strcmp(action, "reply") == 0)
    {
        weechat_command(buf, "/cursor stop");
        snprintf(cmd, sizeof(cmd), "/input insert /cslack reply %s ", hash_ref);
        weechat_command(buf, cmd);
    }
    else if (strcmp(action, "thread") == 0)
    {
        weechat_command(buf, "/cursor stop");
        snprintf(cmd, sizeof(cmd), "/cslack thread %s", hash_ref);
        weechat_command(buf, cmd);
    }

    return WEECHAT_RC_OK;
}

/* Typing WS events at most once per 4s (wee-slack typing_timer). */
static time_t weeslack_typing_last_sent;
static char weeslack_typing_last_channel[128];

static int
weeslack_input_text_changed_cb(const void *pointer, void *data,
                                const char *signal, const char *type_data,
                                void *signal_data)
{
    struct t_gui_buffer *buf = signal_data;
    const char *channel_id;
    struct t_weeslack_workspace *ws;
    time_t now;

    (void)pointer;
    (void)data;
    (void)signal;
    (void)type_data;

    if (!weechat_config_boolean(weeslack_config.send_typing_notice))
        return WEECHAT_RC_OK;
    if (!buf)
        return WEECHAT_RC_OK;

    channel_id = weechat_buffer_get_string(buf, "localvar_slack_channel_id");
    if (!channel_id || !channel_id[0])
        return WEECHAT_RC_OK;

    ws = weeslack_workspace_from_buffer(buf);
    if (!ws || !ws->connected)
        return WEECHAT_RC_OK;

    now = time(NULL);
    if (weeslack_typing_last_sent != 0 &&
        now < weeslack_typing_last_sent + 4 &&
        strcmp(weeslack_typing_last_channel, channel_id) == 0)
        return WEECHAT_RC_OK;

    weeslack_typing_last_sent = now;
    snprintf(weeslack_typing_last_channel, sizeof(weeslack_typing_last_channel),
             "%s", channel_id);
    slack_event_send_typing(ws, channel_id);
    return WEECHAT_RC_OK;
}

/*
 * Legacy bar item (slack_typing_notice). Prefer core item "typing" from
 * typing.so (fed via typing_set_nick on user_typing events).
 */
static char *
weeslack_typing_bar_cb(const void *pointer, void *data,
                       struct t_gui_bar_item *item,
                       struct t_gui_window *window,
                       struct t_gui_buffer *buffer,
                       struct t_hashtable *extra_info)
{
    const char *channel_id;
    struct t_slack_channel *ch;
    const char *col;
    char *out;

    (void)pointer;
    (void)data;
    (void)item;
    (void)window;
    (void)extra_info;

    if (!buffer)
        buffer = weechat_current_buffer();
    if (!buffer)
        return NULL;
    channel_id = weechat_buffer_get_string(buffer, "localvar_slack_channel_id");
    if (!channel_id)
        return NULL;
    ch = slack_channel_search(channel_id);
    if (!ch || !ch->typing_user || !ch->typing_user[0])
        return NULL;

    col = weechat_config_string(weeslack_config.color_typing_notice);
    if (!col || !col[0])
        col = "yellow";
    out = malloc(160);
    if (!out)
        return NULL;
    snprintf(out, 160, "%styping: %s%s",
             weechat_color(col), ch->typing_user, weechat_color("reset"));
    return out;
}

int
weechat_plugin_init(struct t_weechat_plugin *plugin, int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    weechat_plugin = plugin;
    weeslack_plugin_unloading = 0;

    slack_http_queue_init();

    weechat_hook_command(
        "cslack",
        "Slack protocol commands",
        "connect || reconnect || disconnect || migrate || register || unregister || forget || list || channels || loadhistory || rehistory || typing || upload || reply || topic || talk || open || debug || queue || version || info || mute || unmute || status || create || invite || showmuted || distracting || nodistractions || slash || away || back || hide || show || label || thread || react || unreact || users || usergroups || teams || linkarchive || subscribe || unsubscribe || pin || unpin || search || download || stars || star || unstar || whois || join || leave || part || refresh || names || help",
        "connect:      Connect to Slack using configured token\n"
        "reconnect:    Re-issue rtm.connect for current team (or all)\n"
        "disconnect:   Disconnect from Slack\n"
        "migrate:      Import token from wee-slack (python) config\n"
        "register:     OAuth add team ([-nothirdparty] [code|xox…])\n"
        "unregister:   Retire a team: -yes [id|name|domain] (alias: forget)\n"
        "list:         List loaded workspaces\n"
        "channels:     List channels [regex]; -refresh re-fetches from Slack\n"
        "users:        List users [regex]\n"
        "usergroups:   List user groups, or members of @handle\n"
        "loadhistory:  Load message history for current channel\n"
        "rehistory:    Alias for loadhistory\n"
        "typing:       Send typing notification for current channel\n"
        "upload:       Upload a file to current channel\n"
        "reply:        Reply in thread ([ts] msg; default last msg as parent)\n"
        "topic:        Set channel topic\n"
        "talk:         Open DM with user id or name (comma-list = MPDM)\n"
        "open:         Alias for talk\n"
        "debug:        Open weeslack.debug buffer; optional on|off toggles look.debug_mode\n"
        "queue:        Show HTTP request queue / cooldown status\n"
        "version:      Show plugin version (alias: info adds queue/workspaces)\n"
        "info:         Version + HTTP queue + key options + workspaces\n"
        "mute:         Mute current channel\n"
        "unmute:       Unmute current channel\n"
        "status:       Profile status emoji/text, or dnd|away|active|-delete\n"
        "create:       Create channel ([-private] name)\n"
        "invite:       Invite user to current channel\n"
        "showmuted:    List muted channels\n"
        "distracting:  Toggle current buffer as distracting\n"
        "nodistractions: Hide/show all distracting buffers\n"
        "slash:        Run Slack slash command (/cmd args)\n"
        "away:         Mark as away\n"
        "back:         Mark as active (clear away/DnD)\n"
        "hide:         Hide current channel\n"
        "show:         Unhide current, or reopen closed #channel buffer\n"
        "label:        Set buffer title locally\n"
        "thread:       Open thread by parent message ts\n"
        "react:        Add emoji reaction ([ts] emoji; default last msg)\n"
        "unreact:      Remove emoji reaction\n"
        "teams:        List workspaces\n"
        "linkarchive:  Get message permalink ([ts]; default last msg)\n"
        "subscribe:    Subscribe to thread (local notify)\n"
        "unsubscribe:  Unsubscribe from thread\n"
        "pin:          Pin a message ([ts]; default last msg)\n"
        "unpin:        Unpin a message\n"
        "search:       Search messages\n"
        "download:     Download a private Slack file URL\n"
        "stars:        List starred items\n"
        "star:         Star a message ([ts]; default last msg)\n"
        "unstar:       Unstar a message\n"
        "whois:        Show user info by name or id (live presence)\n"
        "join:         Join a channel by name or id\n"
        "leave / part: Leave current (or given) channel\n"
        "refresh:      Re-fetch users.list + emoji.list (no re-bootstrap)\n"
        "names:        Refresh nicklist for the current channel\n"
        "help:         Show help",
        "connect|reconnect|disconnect|migrate|register -nothirdparty|unregister -yes|forget -yes|list|channels|users|usergroups %(slack_usergroups)|loadhistory|rehistory|typing|upload|reply|topic %(slack_topic)|talk|open %(slack_nicks)|debug|queue|version|info|mute|unmute|status -delete|%(slack_emoji)|dnd|away|active|create -private|invite %(slack_nicks)|showmuted|distracting|nodistractions|slash|away|back|hide|show %(slack_channels)|label|thread %(slack_threads)|react|unreact|teams|linkarchive|subscribe|unsubscribe|pin|unpin|search|download|stars|star|unstar|whois %(slack_nicks)|join %(slack_channels)|leave|part|refresh|names|help",
        &weeslack_command_cslack,
        NULL,
        NULL);

    weechat_hook_completion("slack_nicks", "Slack nick completion",
                             &weeslack_nick_completion_cb, NULL, NULL);
    weechat_hook_completion("slack_channels", "Slack channel completion",
                             &weeslack_channel_completion_cb, NULL, NULL);
    weechat_hook_completion("slack_emoji", "Slack emoji completion (:shortcode:)",
                             &weeslack_emoji_completion_cb, NULL, NULL);
    weechat_hook_completion("slack_usergroups", "Slack usergroup completion (@group)",
                             &weeslack_usergroup_completion_cb, NULL, NULL);
    weechat_hook_completion("slack_threads", "Slack thread completion",
                             &weeslack_thread_completion_cb, NULL, NULL);
    /* /topic and /cslack topic — offer current channel topic (wee-slack) */
    weechat_hook_completion("irc_channel_topic",
                             "Slack channel topic for /topic completion",
                             &weeslack_topic_completion_cb, NULL, NULL);
    weechat_hook_completion("slack_topic",
                             "Slack channel topic completion",
                             &weeslack_topic_completion_cb, NULL, NULL);

    weechat_hook_signal("buffer_switch",
                         &weeslack_buffer_switch_cb, NULL, NULL);
    weechat_hook_signal("input_text_changed",
                         &weeslack_input_text_changed_cb, NULL, NULL);

    /* IRC-style commands on weeslack buffers (wee-slack parity). */
    {
        const char *cmds[] = {
            "/me", "/join", "/query", "/msg", "/part", "/leave",
            "/topic", "/invite", "/away", "/whois", NULL
        };
        int i;
        for (i = 0; cmds[i]; i++)
        {
            weechat_hook_command_run(cmds[i], &weeslack_command_run_cb,
                                      NULL, NULL);
        }
    }

    /* Mark-read when WeeChat clears hotlist (wee-slack set_unread hooks). */
    weechat_hook_command_run("/input set_unread",
                              &weeslack_set_unread_all_cb, NULL, NULL);
    weechat_hook_command_run("/input set_unread_current_buffer",
                              &weeslack_set_unread_current_cb, NULL, NULL);
    weechat_hook_command_run("/buffer set unread",
                              &weeslack_set_unread_current_cb, NULL, NULL);
    weechat_hook_command_run("/input complete_next",
                              &weeslack_complete_next_cb, NULL, NULL);

    weechat_bar_item_new("slack_away", &weeslack_away_bar_cb, NULL, NULL);
    weechat_bar_item_new("slack_typing_notice", &weeslack_typing_bar_cb,
                          NULL, NULL);

    /* Cursor / mouse line actions (wee-slack parity) */
    {
        weechat_hook_hsignal("weeslack_mouse",
                              &weeslack_line_event_cb, NULL, NULL);
        weechat_hook_hsignal("weeslack_cursor_delete",
                              &weeslack_line_event_cb, NULL, NULL);
        weechat_hook_hsignal("weeslack_cursor_linkarchive",
                              &weeslack_line_event_cb, NULL, NULL);
        weechat_hook_hsignal("weeslack_cursor_message",
                              &weeslack_line_event_cb, NULL, NULL);
        weechat_hook_hsignal("weeslack_cursor_reply",
                              &weeslack_line_event_cb, NULL, NULL);
        weechat_hook_hsignal("weeslack_cursor_thread",
                              &weeslack_line_event_cb, NULL, NULL);

        /* Separate hashtables per context; __quiet avoids core spam. */
        {
            struct t_hashtable *keys;

            keys = weechat_hashtable_new(8, WEECHAT_HASHTABLE_STRING,
                                          WEECHAT_HASHTABLE_STRING, NULL, NULL);
            if (keys)
            {
                weechat_hashtable_set(keys, "__quiet", "1");
                weechat_hashtable_set(keys, "@chat(weeslack.*):button2",
                                       "hsignal:weeslack_mouse");
                weechat_key_bind("mouse", keys);
                weechat_hashtable_free(keys);
            }
            keys = weechat_hashtable_new(16, WEECHAT_HASHTABLE_STRING,
                                          WEECHAT_HASHTABLE_STRING, NULL, NULL);
            if (keys)
            {
                weechat_hashtable_set(keys, "__quiet", "1");
                weechat_hashtable_set(keys, "@chat(weeslack.*):D",
                                       "hsignal:weeslack_cursor_delete");
                weechat_hashtable_set(keys, "@chat(weeslack.*):L",
                                       "hsignal:weeslack_cursor_linkarchive");
                weechat_hashtable_set(keys, "@chat(weeslack.*):M",
                                       "hsignal:weeslack_cursor_message");
                weechat_hashtable_set(keys, "@chat(weeslack.*):R",
                                       "hsignal:weeslack_cursor_reply");
                weechat_hashtable_set(keys, "@chat(weeslack.*):T",
                                       "hsignal:weeslack_cursor_thread");
                weechat_key_bind("cursor", keys);
                weechat_hashtable_free(keys);
            }
        }
    }

    weechat_hook_hdata("cslack_channel", "Slack channels",
                        &weeslack_hdata_channel_cb, NULL, NULL);
    weechat_hook_hdata("cslack_user", "Slack users",
                        &weeslack_hdata_user_cb, NULL, NULL);
    weechat_hook_hdata("cslack_message", "Slack messages",
                        &weeslack_hdata_message_cb, NULL, NULL);

    weechat_hook_infolist("cslack_channel", "Slack channels",
                           "pointer", NULL,
                           &weeslack_infolist_channel_cb, NULL, NULL);
    weechat_hook_infolist("cslack_user", "Slack users",
                           "pointer", NULL,
                           &weeslack_infolist_user_cb, NULL, NULL);

    if (weeslack_config_init() == WEECHAT_RC_ERROR)
        return WEECHAT_RC_ERROR;

    /* Optional standard emoji table (same file as Python wee-slack). */
    slack_event_load_weemoji();

    /* 5 min interval; no-op unless look.never_away is on */
    weechat_hook_timer(1000 * 60 * 5, 0, 0,
                        &weeslack_never_away_cb, NULL, NULL);

    weeslack_upgrade_file = weechat_upgrade_new(
        "weeslack",
        &weeslack_upgrade_read_cb, NULL, NULL);

    if (weeslack_upgrade_file && weechat_plugin->upgrading)
        weechat_upgrade_read(weeslack_upgrade_file);

    /* Auto-connect (and /upgrade reconnect) when token + options allow. */
    weeslack_maybe_autoconnect();

    return WEECHAT_RC_OK;
}

int
weechat_plugin_end(struct t_weechat_plugin *plugin)
{
    (void) plugin;

    struct t_weeslack_workspace *workspace, *next_workspace;
    struct t_weeslack_channel *channel, *next_channel;

    /* Closing buffers on unload must not leave Slack channels remotely. */
    weeslack_plugin_unloading = 1;

    /* Stop network before tearing down buffers/models. */
    slack_http_queue_shutdown();

    /*
     * Close all plugin buffers while channel models still exist.
     * WeeChat closes leftover buffers after plugin_end; if we free channels
     * first, close_cb UAF's channel pointers (glibc abort on free).
     */
    slack_buffer_close_all();

    slack_event_unload_weemoji();

    for (channel = weeslack_channels; channel; channel = next_channel)
    {
        next_channel = channel->next_channel;
        weeslack_channel_free(channel);
    }
    weeslack_channels = NULL;

    for (workspace = weeslack_workspaces; workspace; workspace = next_workspace)
    {
        next_workspace = workspace->next_workspace;
        if (workspace->ws)
            slack_ws_free(workspace->ws);
        slack_channel_free_workspace(workspace);
        slack_event_free_workspace_data(workspace);
        weeslack_workspace_free(workspace);
    }
    weeslack_workspaces = NULL;

    if (weeslack_upgrade_file)
    {
        weeslack_upgrade_write_cb(NULL, NULL, weeslack_upgrade_file,
                                   UPGRADE_OBJECT_CHANNEL);
        weechat_upgrade_write_object(weeslack_upgrade_file,
                                      UPGRADE_OBJECT_END, NULL);
        weechat_upgrade_close(weeslack_upgrade_file);
        weeslack_upgrade_file = NULL;
    }

    if (weeslack_config.file)
    {
        weechat_config_write(weeslack_config.file);
        weechat_config_free(weeslack_config.file);
        weeslack_config.file = NULL;
    }

    return WEECHAT_RC_OK;
}

struct t_weeslack_workspace *
weeslack_workspace_from_buffer(struct t_gui_buffer *buffer)
{
    struct t_weeslack_workspace *ws, *only = NULL;
    const char *wid, *server;
    int n = 0;

    if (buffer)
    {
        wid = weechat_buffer_get_string(buffer, "localvar_slack_workspace_id");
        if (wid && wid[0])
        {
            ws = weeslack_workspace_search(wid);
            if (ws)
                return ws;
        }
        server = weechat_buffer_get_string(buffer, "localvar_server");
        if (server && server[0])
        {
            for (ws = weeslack_workspaces; ws; ws = ws->next_workspace)
            {
                if (ws->id && weechat_strcasecmp(ws->id, server) == 0)
                    return ws;
                if (ws->domain && weechat_strcasecmp(ws->domain, server) == 0)
                    return ws;
                if (ws->name && weechat_strcasecmp(ws->name, server) == 0)
                    return ws;
            }
        }
    }

    for (ws = weeslack_workspaces; ws; ws = ws->next_workspace)
    {
        only = ws;
        n++;
    }
    if (n == 1)
        return only;

    return weeslack_workspace_search("default");
}

struct t_weeslack_workspace *
weeslack_workspace_search(const char *name)
{
    struct t_weeslack_workspace *workspace;

    if (!name)
        return weeslack_workspaces;

    for (workspace = weeslack_workspaces; workspace; workspace = workspace->next_workspace)
    {
        if (workspace->id && weechat_strcasecmp(workspace->id, name) == 0)
            return workspace;
        if (workspace->name && weechat_strcasecmp(workspace->name, name) == 0)
            return workspace;
    }

    /* single-workspace convenience: /cslack always used "default" historically */
    if (weechat_strcasecmp(name, "default") == 0 && weeslack_workspaces &&
        !weeslack_workspaces->next_workspace)
        return weeslack_workspaces;

    return NULL;
}

struct t_weeslack_workspace *
weeslack_workspace_new(const char *name, const char *token, const char *cookie)
{
    struct t_weeslack_workspace *workspace;

    workspace = weeslack_workspace_search(name);
    if (workspace)
        return workspace;

    workspace = calloc(1, sizeof(struct t_weeslack_workspace));
    if (!workspace)
        return NULL;

    workspace->id = strdup(name);
    workspace->name = strdup(name);
    workspace->token = strdup(token);
    workspace->cookie = cookie ? strdup(cookie) : NULL;
    workspace->connected = 0;
    workspace->reconnect_delay = 1;
    workspace->max_reconnect_delay = 60;
    workspace->ws = NULL;
    workspace->my_user_id = NULL;
    workspace->my_presence = strdup("active");
    workspace->my_manual_away = 0;
    workspace->next_workspace = weeslack_workspaces;
    workspace->prev_workspace = NULL;

    if (weeslack_workspaces)
        weeslack_workspaces->prev_workspace = workspace;
    weeslack_workspaces = workspace;

    return workspace;
}

int
weeslack_workspace_free(struct t_weeslack_workspace *workspace)
{
    if (!workspace)
        return WEECHAT_RC_ERROR;

    if (workspace->prev_workspace)
        workspace->prev_workspace->next_workspace = workspace->next_workspace;
    else
        weeslack_workspaces = workspace->next_workspace;

    if (workspace->next_workspace)
        workspace->next_workspace->prev_workspace = workspace->prev_workspace;

    free(workspace->id);
    free(workspace->name);
    free(workspace->domain);
    free(workspace->token);
    free(workspace->cookie);
    free(workspace->ws_url);
    free(workspace->my_user_id);
    free(workspace->highlight_words);
    free(workspace->my_presence);
    free(workspace);

    return WEECHAT_RC_OK;
}

/* ---- debug_mode / record_events (wee-slack parity) ---- */

static struct t_gui_buffer *weeslack_debug_buffer;

static int
weeslack_debug_buffer_close_cb(const void *pointer, void *data,
                                struct t_gui_buffer *buffer)
{
    (void)pointer;
    (void)data;
    (void)buffer;
    weeslack_debug_buffer = NULL;
    return WEECHAT_RC_OK;
}

static struct t_gui_buffer *
weeslack_debug_buffer_get(void)
{
    if (weeslack_debug_buffer)
        return weeslack_debug_buffer;

    weeslack_debug_buffer = weechat_buffer_new(
        "weeslack.debug",
        NULL, NULL, NULL,
        &weeslack_debug_buffer_close_cb, NULL, NULL);
    if (weeslack_debug_buffer)
    {
        weechat_buffer_set(weeslack_debug_buffer, "title",
                           "weeslack debug log");
        weechat_buffer_set(weeslack_debug_buffer, "localvar_set_type",
                           "debug");
        weechat_buffer_set(weeslack_debug_buffer, "notify", "none");
    }
    return weeslack_debug_buffer;
}

void
weeslack_debug_open_buffer(void)
{
    struct t_gui_buffer *dbuf = weeslack_debug_buffer_get();
    if (dbuf)
        weechat_buffer_set(dbuf, "display", "1");
}

void
weeslack_debug_at(int level, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    struct t_gui_buffer *dbuf;
    int max_level;

    if (!fmt)
        return;
    if (!weechat_config_boolean(weeslack_config.debug_mode))
        return;
    max_level = weechat_config_integer(weeslack_config.debug_level);
    if (max_level < 1)
        max_level = 3;
    if (level < 1)
        level = 1;
    /* Lower level numbers are more important; show level <= max */
    if (level > max_level)
        return;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    dbuf = weeslack_debug_buffer_get();
    weechat_printf(dbuf, "%s[%d] %s", weechat_prefix("network"), level, buf);
}

void
weeslack_debug(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;

    if (!fmt)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    weeslack_debug_at(2, "%s", buf);
}

void
weeslack_record_json_ws(struct t_weeslack_workspace *workspace,
                        const char *json_text)
{
    char dir[512];
    char path[640];
    const char *data_dir;
    const char *domain;
    FILE *fp;
    time_t now;
    struct tm tm_now;
    struct t_weeslack_workspace *ws;

    if (!json_text || !json_text[0])
        return;
    if (!weechat_config_boolean(weeslack_config.record_events))
        return;

    data_dir = weechat_info_get("weechat_data_dir", "");
    if (!data_dir || !data_dir[0])
        data_dir = weechat_info_get("weechat_dir", "");
    if (!data_dir || !data_dir[0])
        return;

    domain = "default";
    if (workspace)
    {
        if (workspace->domain && workspace->domain[0])
            domain = workspace->domain;
        else if (workspace->name && workspace->name[0])
            domain = workspace->name;
        else if (workspace->id && workspace->id[0])
            domain = workspace->id;
    }
    else
    {
        for (ws = weeslack_workspaces; ws; ws = ws->next_workspace)
        {
            if (ws->domain && ws->domain[0])
            {
                domain = ws->domain;
                break;
            }
            if (ws->name && ws->name[0])
            {
                domain = ws->name;
                break;
            }
        }
    }

    snprintf(dir, sizeof(dir), "%s/weeslack/events", data_dir);
    weechat_mkdir_parents(dir, 0755);

    now = time(NULL);
    if (localtime_r(&now, &tm_now))
    {
        char date_s[32];
        snprintf(date_s, sizeof(date_s), "%04d%02d%02d",
                 tm_now.tm_year + 1900,
                 tm_now.tm_mon + 1,
                 tm_now.tm_mday);
        snprintf(path, sizeof(path), "%s/%s-%s.jsonl", dir, domain, date_s);
    }
    else
        snprintf(path, sizeof(path), "%s/%s.jsonl", dir, domain);

    fp = fopen(path, "a");
    if (!fp)
        return;
    fprintf(fp, "%s\n", json_text);
    fclose(fp);
}

void
weeslack_record_json(const char *json_text)
{
    weeslack_record_json_ws(NULL, json_text);
}

struct t_weeslack_channel *
weeslack_channel_search(struct t_weeslack_workspace *workspace, const char *name)
{
    struct t_weeslack_channel *channel;

    for (channel = weeslack_channels; channel; channel = channel->next_channel)
    {
        if (channel->workspace == workspace
            && weechat_strcasecmp(channel->name, name) == 0)
        {
            return channel;
        }
    }
    return NULL;
}

struct t_weeslack_channel *
weeslack_channel_new(struct t_weeslack_workspace *workspace,
                     const char *id, const char *name)
{
    struct t_weeslack_channel *channel;

    channel = weeslack_channel_search(workspace, name);
    if (channel)
        return channel;

    channel = calloc(1, sizeof(struct t_weeslack_channel));
    if (!channel)
        return NULL;

    channel->id = strdup(id);
    channel->name = strdup(name);
    channel->workspace = workspace;
    channel->next_channel = weeslack_channels;
    channel->prev_channel = NULL;

    if (weeslack_channels)
        weeslack_channels->prev_channel = channel;
    weeslack_channels = channel;

    return channel;
}

int
weeslack_channel_free(struct t_weeslack_channel *channel)
{
    if (!channel)
        return WEECHAT_RC_ERROR;

    if (channel->prev_channel)
        channel->prev_channel->next_channel = channel->next_channel;
    else
        weeslack_channels = channel->next_channel;

    if (channel->next_channel)
        channel->next_channel->prev_channel = channel->prev_channel;

    free(channel->id);
    free(channel->name);
    free(channel);

    return WEECHAT_RC_OK;
}
