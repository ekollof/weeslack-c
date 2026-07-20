#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
WEECHAT_PLUGIN_VERSION("0.1.0")
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
        struct json_object *name_obj;
        if (json_object_object_get_ex(team_obj, "name", &name_obj))
        {
            free(workspace->name);
            workspace->name = strdup(json_object_get_string(name_obj));
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

static int
weeslack_command_cslack(const void *pointer, void *data,
                        struct t_gui_buffer *buffer,
                        int argc, char **argv, char **argv_eol)
{
    (void) pointer;
    (void) data;
    (void) argv_eol;

    if (argc < 2)
    {
        weechat_printf(buffer, "%sweeslack: usage: /cslack <command> [args]",
                        weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (weechat_strcasecmp(argv[1], "connect") == 0)
    {
        const char *token_cfg = weechat_config_string(weeslack_config.token);
        if (!token_cfg || !token_cfg[0])
        {
            weechat_printf(buffer, "%sweeslack: no token configured. "
                            "Run /cslack migrate or set weeslack.workspace.token",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        /* token may be "xoxc-...:cookie" or plain xoxp/xoxb */
        char *token_copy = strdup(token_cfg);
        char *token_part = token_copy ? token_copy : (char *)token_cfg;
        char *cookie_part = NULL;
        if (token_copy)
        {
            char *colon = strchr(token_copy, ':');
            if (colon && strncmp(token_copy, "xoxc-", 5) == 0)
            {
                *colon = '\0';
                cookie_part = colon + 1;
            }
        }

        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_search("default");
        if (!ws)
        {
            ws = weeslack_workspace_new("default", token_part, cookie_part);
        }
        else
        {
            free(ws->token);
            ws->token = strdup(token_part);
            free(ws->cookie);
            ws->cookie = cookie_part ? strdup(cookie_part) : NULL;
        }
        free(token_copy);

        if (ws)
            weeslack_connect_workspace(ws, buffer);
    }
    else if (weechat_strcasecmp(argv[1], "disconnect") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_search("default");
        if (ws && ws->ws)
        {
            slack_ws_disconnect(ws->ws);
            ws->connected = 0;
            weechat_printf(buffer, "%sweeslack: disconnected from %s",
                            weechat_prefix("network"), ws->name);
        }
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
        ws = weeslack_workspace_search("default");
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        slack_event_fetch_channels(ws);
        weechat_printf(buffer, "%sweeslack: refreshing channel list...",
                        weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "loadhistory") == 0 ||
             weechat_strcasecmp(argv[1], "rehistory") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_search("default");
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
        ws = weeslack_workspace_search("default");
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
        ws = weeslack_workspace_search("default");
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

        ws = weeslack_workspace_search("default");
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
        ws = weeslack_workspace_search("default");
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
        int count = 0;
        for (user = slack_user_list_global(); user; user = user->next)
        {
            const char *status = user->presence ? user->presence : "unknown";
            const char *display = user->display_name && user->display_name[0]
                                  ? user->display_name : user->name;
            const char *real = user->real_name ? user->real_name : "";

            weechat_printf(buffer, "%s  %s%s%s %s (%s) [%s]",
                            weechat_prefix("network"),
                            weechat_color("cyan"),
                            display,
                            weechat_color("reset"),
                            real,
                            user->id,
                            status);
            count++;
        }
        weechat_printf(buffer, "%sweeslack: %d users loaded",
                        weechat_prefix("network"), count);
    }
    else if (weechat_strcasecmp(argv[1], "usergroups") == 0)
    {
        struct t_slack_subteam *st;
        int count = 0;
        for (st = slack_subteam_list_global(); st; st = st->next)
        {
            weechat_printf(buffer, "%s  %s%s%s @%s%s%s — %s",
                            weechat_prefix("network"),
                            weechat_color("cyan"),
                            st->name ? st->name : st->id,
                            weechat_color("reset"),
                            weechat_color("green"),
                            st->handle ? st->handle : "?",
                            weechat_color("reset"),
                            st->description ? st->description : "");
            count++;
        }
        weechat_printf(buffer, "%sweeslack: %d user groups loaded",
                        weechat_prefix("network"), count);
    }
    else if (weechat_strcasecmp(argv[1], "talk") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_search("default");
        if (!ws || !ws->connected)
        {
            weechat_printf(buffer, "%sweeslack: not connected",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        if (argc < 3)
        {
            weechat_printf(buffer, "%sweeslack: usage: /cslack talk <user_id or name>",
                            weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        slack_event_open_dm(ws, argv[2]);
    }
    else if (weechat_strcasecmp(argv[1], "mute") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_search("default");
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
        ws = weeslack_workspace_search("default");
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
        ws = weeslack_workspace_search("default");
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
                    emo = slack_event_replace_emoji(tmp);
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

        ws = weeslack_workspace_search("default");
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

        ws = weeslack_workspace_search("default");
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

        ws = weeslack_workspace_search("default");
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
        ws = weeslack_workspace_search("default");
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
        weechat_bar_item_update("away");
        weechat_bar_item_update("slack_away");
        weechat_printf(buffer, "%sweeslack: marked as away",
                        weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "back") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_search("default");
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
        weechat_bar_item_update("away");
        weechat_bar_item_update("slack_away");
        weechat_printf(buffer, "%sweeslack: marked as active",
                        weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "hide") == 0)
    {
        weechat_buffer_set(buffer, "hidden", "1");
        weechat_printf(buffer, "%sweeslack: channel hidden",
                        weechat_prefix("network"));
    }
    else if (weechat_strcasecmp(argv[1], "show") == 0)
    {
        weechat_buffer_set(buffer, "hidden", "0");
        weechat_buffer_set(buffer, "active", "1");
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

        ws = weeslack_workspace_search("default");
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
                weechat_buffer_set(thread->buffer, "display", "1");
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

        ws = weeslack_workspace_search("default");
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

        ws = weeslack_workspace_search("default");
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

        ws = weeslack_workspace_search("default");
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

        ws = weeslack_workspace_search("default");
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

        ws = weeslack_workspace_search("default");
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
        ws = weeslack_workspace_search("default");
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
        ws = weeslack_workspace_search("default");
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
        slack_event_download_file(ws, argv[2], weeslack_cmd_buffer(buffer));
    }
    else if (weechat_strcasecmp(argv[1], "stars") == 0)
    {
        struct t_weeslack_workspace *ws;
        ws = weeslack_workspace_search("default");
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

        ws = weeslack_workspace_search("default");
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
        ws = weeslack_workspace_search("default");
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
        ws = weeslack_workspace_search("default");
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

        ws = weeslack_workspace_search("default");
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
        ws = weeslack_workspace_search("default");
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

        ws = weeslack_workspace_search("default");
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
        weechat_printf(buffer, "  %sdisconnect%s   Disconnect from Slack",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %smigrate%s      Import token from wee-slack",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %slist%s         List workspaces",
                        weechat_color("cyan"), weechat_color("reset"));
        weechat_printf(buffer, "  %schannels%s     Refresh channel list",
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
        weechat_printf(buffer, "  %stalk%s <user>  Open DM with user",
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
        weechat_printf(buffer, "  %sshow%s         Show hidden channel",
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
        "Slack API token (supports xoxp, xoxs, xoxc:cookie formats)",
        NULL, 0, 0, "", NULL, 0,
        NULL, NULL, NULL,
        NULL, NULL, NULL,
        NULL, NULL, NULL);

    if (!weeslack_config.token)
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
        "Show typing indicator in the buffer title (not short_name)",
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
        "Directory for /cslack download (empty = ~/Downloads/weeslack)",
        NULL, 0, 0, "", NULL, 0,
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
                    if (name && name[0])
                    {
                        weechat_completion_list_add(completion, name, 0,
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
        for (user = slack_user_list_global(); user; user = user->next)
        {
            const char *name = slack_user_best_name(user);
            if (name)
                weechat_completion_list_add(completion, name, 0,
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
        struct t_weeslack_workspace *ws = weeslack_workspace_search("default");
        if (ws && ws->connected)
        {
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

static int weeslack_upgrade_reconnect_cb(const void *pointer, void *data,
                                          int remaining_calls);

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

    color = weechat_config_string(weechat_config_get("weechat.color.item_away"));
    if (!color || !color[0])
        color = "cyan";
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
    const char *action = data;
    const char *tags, *buf_ptr_str;
    struct t_gui_buffer *buf;
    char *tags_copy = NULL, *tok, *save = NULL;
    const char *ts_str = NULL;
    struct t_slack_channel *ch;
    struct t_slack_message *msg = NULL;
    char cmd[256];
    char hash_ref[40];

    (void)pointer;
    (void)signal;

    if (!action || !hashtable)
        return WEECHAT_RC_OK;

    tags = weechat_hashtable_get(hashtable, "_chat_line_tags");
    buf_ptr_str = weechat_hashtable_get(hashtable, "_buffer");
    if (!tags || !buf_ptr_str)
        return WEECHAT_RC_OK;

    /* buffer pointer is hex string of address in some WeeChat versions */
    buf = (struct t_gui_buffer *)strtoull(buf_ptr_str, NULL, 16);
    if (!buf)
        buf = weechat_current_buffer();

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

/* Send typing notices while composing (throttled lightly by WeeChat signals). */
static int
weeslack_input_text_changed_cb(const void *pointer, void *data,
                                const char *signal, const char *type_data,
                                void *signal_data)
{
    struct t_gui_buffer *buf = signal_data;
    const char *channel_id;
    struct t_weeslack_workspace *ws;

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

    ws = weeslack_workspace_search("default");
    if (!ws || !ws->connected)
        return WEECHAT_RC_OK;

    slack_event_send_typing(ws, channel_id);
    return WEECHAT_RC_OK;
}

int
weechat_plugin_init(struct t_weechat_plugin *plugin, int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    weechat_plugin = plugin;

    slack_http_queue_init();

    weechat_hook_command(
        "cslack",
        "Slack protocol commands",
        "connect || disconnect || migrate || list || channels || loadhistory || rehistory || typing || upload || reply || topic || talk || mute || unmute || status || create || invite || showmuted || distracting || nodistractions || slash || away || back || hide || show || label || thread || react || unreact || users || usergroups || teams || linkarchive || subscribe || unsubscribe || pin || unpin || search || download || stars || star || unstar || whois || join || leave || part || refresh || names || help",
        "connect:      Connect to Slack using configured token\n"
        "disconnect:   Disconnect from Slack\n"
        "migrate:      Import token from wee-slack (python) config\n"
        "list:         List loaded workspaces\n"
        "channels:     Refresh channel list from Slack\n"
        "users:        List loaded users\n"
        "usergroups:   List user groups\n"
        "loadhistory:  Load message history for current channel\n"
        "rehistory:    Alias for loadhistory\n"
        "typing:       Send typing notification for current channel\n"
        "upload:       Upload a file to current channel\n"
        "reply:        Reply in thread ([ts] msg; default last msg as parent)\n"
        "topic:        Set channel topic\n"
        "talk:         Open DM with user id or name\n"
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
        "show:         Show hidden channel\n"
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
        "connect|disconnect|migrate|list|channels|users|usergroups|loadhistory|rehistory|typing|upload|reply|topic|talk %(slack_nicks)|mute|unmute|status -delete|%(slack_emoji)|dnd|away|active|create -private|invite %(slack_nicks)|showmuted|distracting|nodistractions|slash|away|back|hide|show|label|thread %(slack_threads)|react|unreact|teams|linkarchive|subscribe|unsubscribe|pin|unpin|search|download|stars|star|unstar|whois %(slack_nicks)|join %(slack_channels)|leave|part|refresh|names|help",
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

    weechat_hook_signal("buffer_switch",
                         &weeslack_buffer_switch_cb, NULL, NULL);
    weechat_hook_signal("input_text_changed",
                         &weeslack_input_text_changed_cb, NULL, NULL);

    weechat_bar_item_new("away", &weeslack_away_bar_cb, NULL, NULL);
    weechat_bar_item_new("slack_away", &weeslack_away_bar_cb, NULL, NULL);

    /* Cursor / mouse line actions (wee-slack parity) */
    {
        struct t_hashtable *keys;

        weechat_hook_hsignal("weeslack_mouse",
                              &weeslack_line_event_cb, NULL, "auto");
        weechat_hook_hsignal("weeslack_cursor_delete",
                              &weeslack_line_event_cb, NULL, "delete");
        weechat_hook_hsignal("weeslack_cursor_linkarchive",
                              &weeslack_line_event_cb, NULL, "linkarchive");
        weechat_hook_hsignal("weeslack_cursor_message",
                              &weeslack_line_event_cb, NULL, "message");
        weechat_hook_hsignal("weeslack_cursor_reply",
                              &weeslack_line_event_cb, NULL, "reply");
        weechat_hook_hsignal("weeslack_cursor_thread",
                              &weeslack_line_event_cb, NULL, "thread");

        keys = weechat_hashtable_new(8, WEECHAT_HASHTABLE_STRING,
                                      WEECHAT_HASHTABLE_STRING, NULL, NULL);
        if (keys)
        {
            weechat_hashtable_set(keys, "@chat(weeslack.*):button2",
                                   "hsignal:weeslack_mouse");
            weechat_key_bind("mouse", keys);
            weechat_hashtable_free(keys);
        }
        keys = weechat_hashtable_new(16, WEECHAT_HASHTABLE_STRING,
                                      WEECHAT_HASHTABLE_STRING, NULL, NULL);
        if (keys)
        {
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

    /* After /upgrade: reconnect if we still have a token */
    if (weechat_plugin->upgrading)
    {
        const char *tok = weechat_config_string(weeslack_config.token);
        if (tok && tok[0])
        {
            weechat_hook_timer(
                500, 0, 1,
                &weeslack_upgrade_reconnect_cb, NULL, NULL);
        }
    }

    return WEECHAT_RC_OK;
}

static int
weeslack_upgrade_reconnect_cb(const void *pointer, void *data,
                                int remaining_calls)
{
    struct t_weeslack_workspace *ws;
    const char *token_cfg;
    char *token_copy, *token_part, *cookie_part, *colon;

    (void) pointer;
    (void) data;
    (void) remaining_calls;

    token_cfg = weechat_config_string(weeslack_config.token);
    if (!token_cfg || !token_cfg[0])
        return WEECHAT_RC_OK;

    token_copy = strdup(token_cfg);
    token_part = token_copy ? token_copy : (char *)token_cfg;
    cookie_part = NULL;
    if (token_copy)
    {
        colon = strchr(token_copy, ':');
        if (colon && strncmp(token_copy, "xoxc-", 5) == 0)
        {
            *colon = '\0';
            cookie_part = colon + 1;
        }
    }

    ws = weeslack_workspace_search("default");
    if (!ws)
        ws = weeslack_workspace_new("default", token_part, cookie_part);
    else
    {
        free(ws->token);
        ws->token = strdup(token_part);
        free(ws->cookie);
        ws->cookie = cookie_part ? strdup(cookie_part) : NULL;
    }
    free(token_copy);

    if (ws)
        weeslack_connect_workspace(ws, NULL);

    return WEECHAT_RC_OK;
}

int
weechat_plugin_end(struct t_weechat_plugin *plugin)
{
    (void) plugin;

    struct t_weeslack_workspace *workspace, *next_workspace;
    struct t_weeslack_channel *channel, *next_channel;

    slack_http_queue_shutdown();
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
    free(workspace->token);
    free(workspace->cookie);
    free(workspace->ws_url);
    free(workspace->my_user_id);
    free(workspace->my_presence);
    free(workspace);

    return WEECHAT_RC_OK;
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
