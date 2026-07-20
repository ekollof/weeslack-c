#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weeslack.h"
#include "slack_buffer.h"
#include "slack_data.h"
#include "slack_event.h"

static struct t_slack_buffer *slack_buffer_list = NULL;

/*
 * Buffer layout — IRC-style, isolated from python wee-slack:
 *
 *   Server:   weeslack.server.<team>     (localvar type=server, server=<team>)
 *   Channel:  weeslack.<team>.<channel>  (localvar type=channel|private, server=<team>)
 *
 * Never use script_name=slack or names under python.slack.* so both plugins
 * can run side by side without buffer or buflist collisions.
 *
 * Buflist parent lookup (search_server_buffer_ptr) finds weeslack.server.<team>
 * once "weeslack" is in its plugin regex; until then buffers still use a clear
 * hierarchy in full_name / localvars / short_name.
 */
static void
slack_buffer_sanitize(char *s)
{
    if (!s)
        return;
    for (; *s; s++)
    {
        if (*s == ' ' || *s == '\t')
            *s = '_';
        else if (*s == '.')
            *s = '_';
    }
}

/* Sanitized team key used in buffer names and localvar server. */
static void
slack_buffer_team_key(const struct t_weeslack_workspace *workspace,
                      char *out, size_t out_size)
{
    snprintf(out, out_size, "%s",
             (workspace && workspace->name && workspace->name[0])
                 ? workspace->name : "workspace");
    slack_buffer_sanitize(out);
}

static const char *
slack_buffer_get_name(struct t_weeslack_workspace *workspace,
                      struct t_slack_channel *channel)
{
    static char name[320];
    char team[96];
    char ch[96];

    if (!workspace || !channel)
        return "(unknown)";

    slack_buffer_team_key(workspace, team, sizeof(team));
    snprintf(ch, sizeof(ch), "%s",
             channel->name ? channel->name : channel->id);
    slack_buffer_sanitize(ch);

    /* full_name: weeslack.<team>.<channel> */
    snprintf(name, sizeof(name), "%s.%s", team, ch);

    return name;
}

static void
slack_buffer_set_common_localvars(struct t_gui_buffer *buffer,
                                  struct t_weeslack_workspace *workspace,
                                  const char *type)
{
    char team[96];

    if (!buffer || !workspace)
        return;

    slack_buffer_team_key(workspace, team, sizeof(team));

    weechat_buffer_set(buffer, "localvar_set_type", type);
    weechat_buffer_set(buffer, "localvar_set_server", team);
    /* Keep distinct from python wee-slack (script_name=slack / python.slack.*) */
    weechat_buffer_set(buffer, "localvar_set_no_python_slack", "1");
}

static int
slack_buffer_close_cb(const void *pointer, void *data,
                      struct t_gui_buffer *buffer)
{
    (void) data;
    (void) buffer;

    struct t_slack_buffer *sbuf = (struct t_slack_buffer *)pointer;

    if (sbuf)
    {
        /* keep t_slack_buffer / channel model for API history; clear links */
        if (sbuf->channel)
            sbuf->channel->buffer = NULL;
        sbuf->buffer = NULL;
    }

    return WEECHAT_RC_OK;
}

static int
slack_buffer_input_cb(const void *pointer, void *data,
                      struct t_gui_buffer *buffer,
                      const char *input_data)
{
    (void) data;
    (void) buffer;

    struct t_slack_buffer *sbuf = (struct t_slack_buffer *)pointer;

    if (!sbuf || !sbuf->channel || !input_data || !input_data[0])
        return WEECHAT_RC_OK;

    if (!sbuf->workspace || !sbuf->workspace->connected)
    {
        weechat_printf(sbuf->buffer, "%sweeslack: not connected",
                        weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    /* replace emoji shortcodes with unicode before sending */
    char *processed = slack_event_replace_emoji(input_data);
    if (!processed)
        return WEECHAT_RC_OK;

    /* for thread buffers, find the parent channel and use thread_ts */
    const char *thread_ts = NULL;
    const char *channel_id = sbuf->channel->id;

    if (sbuf->channel->type == SLACK_CHANNEL_TYPE_THREAD)
    {
        /* thread channel IDs are "thread_<parent_id>_<thread_ts>" */
        const char *prefix = "thread_";
        if (strncmp(channel_id, prefix, strlen(prefix)) == 0)
        {
            const char *rest = channel_id + strlen(prefix);
            const char *second_underscore = strchr(rest, '_');
            if (second_underscore)
            {
                size_t parent_id_len = second_underscore - rest;
                char parent_id[128];
                if (parent_id_len < sizeof(parent_id))
                {
                    memcpy(parent_id, rest, parent_id_len);
                    parent_id[parent_id_len] = '\0';

                    struct t_slack_channel *parent = slack_channel_search(parent_id);
                    if (parent)
                        channel_id = parent->id;
                }
                thread_ts = second_underscore + 1;
            }
        }
    }

    /* /me action → chat.meMessage (wee-slack style) */
    if (strncmp(processed, "/me ", 4) == 0 && processed[4])
    {
        slack_event_send_me_message(sbuf->workspace, channel_id,
                                     processed + 4, thread_ts);
        free(processed);
        return WEECHAT_RC_OK;
    }

    /*
     * Leading / that is not a WeeChat command: post as chat text
     * (emoji already expanded). Real Slack slash APIs are app-specific.
     */
    slack_event_send_message(sbuf->workspace,
                              channel_id,
                              processed,
                              thread_ts);

    free(processed);

    return WEECHAT_RC_OK;
}

static int
slack_buffer_signal_close_cb(const void *pointer, void *data,
                              const char *signal, const char *type_data,
                              void *signal_data)
{
    (void) signal;
    (void) type_data;
    (void) signal_data;

    struct t_slack_buffer *sbuf = (struct t_slack_buffer *)pointer;
    struct t_gui_buffer *buffer = (struct t_gui_buffer *)data;

    (void) buffer;

    if (sbuf)
        sbuf->buffer = NULL;

    return WEECHAT_RC_OK;
}

struct t_slack_buffer *
slack_buffer_new_server(struct t_weeslack_workspace *workspace)
{
    struct t_slack_buffer *sbuf;
    char name[256];

    if (!workspace)
        return NULL;

    if (workspace->server_buffer)
        return NULL;

    sbuf = calloc(1, sizeof(struct t_slack_buffer));
    if (!sbuf)
        return NULL;

    sbuf->workspace = workspace;
    sbuf->channel = NULL;

    {
        char team[96];
        slack_buffer_team_key(workspace, team, sizeof(team));
        /* full_name: weeslack.server.<team> (IRC-style, not python.slack.*) */
        snprintf(name, sizeof(name), "server.%s", team);
    }

    sbuf->buffer = weechat_buffer_new(
        name,
        &slack_buffer_input_cb, sbuf, NULL,
        &slack_buffer_close_cb, sbuf, NULL);

    if (!sbuf->buffer)
    {
        weechat_printf(NULL, "%sweeslack: buffer_new failed for %s",
                        weechat_prefix("error"), name);
        free(sbuf);
        return NULL;
    }

    slack_buffer_set_common_localvars(sbuf->buffer, workspace, "server");
    {
        char team[96];
        slack_buffer_team_key(workspace, team, sizeof(team));
        weechat_buffer_set(sbuf->buffer, "short_name", team);
    }

    weechat_buffer_set(sbuf->buffer, "nicklist", "0");

    weechat_hook_signal("buffer_closing",
                         &slack_buffer_signal_close_cb, sbuf, NULL);

    workspace->server_buffer = sbuf->buffer;

    sbuf->next = slack_buffer_list;
    sbuf->prev = NULL;
    if (slack_buffer_list)
        slack_buffer_list->prev = sbuf;
    slack_buffer_list = sbuf;

    return sbuf;
}

/* Resolve DM peer id → display name on channel->name before buffer naming. */
static void
slack_buffer_resolve_dm_name(struct t_slack_channel *channel)
{
    struct t_slack_user *peer = NULL;
    const char *pretty;

    if (!channel || channel->type != SLACK_CHANNEL_TYPE_DM)
        return;

    if (channel->user_id)
        peer = slack_user_search(channel->user_id);
    if (!peer && channel->name)
        peer = slack_user_search(channel->name);

    pretty = peer ? slack_user_best_name(peer) : NULL;
    if (!pretty || !pretty[0])
        return;

    if (!channel->name || strcmp(channel->name, pretty) != 0)
    {
        free(channel->name);
        channel->name = strdup(pretty);
    }
}

struct t_slack_buffer *
slack_buffer_new(struct t_weeslack_workspace *workspace,
                 struct t_slack_channel *channel)
{
    struct t_slack_buffer *sbuf;

    if (!workspace || !channel)
        return NULL;

    /* users.list is fetched before channels; resolve PM titles now */
    slack_buffer_resolve_dm_name(channel);

    sbuf = calloc(1, sizeof(struct t_slack_buffer));
    if (!sbuf)
        return NULL;

    sbuf->workspace = workspace;
    sbuf->channel = channel;

    const char *name = slack_buffer_get_name(workspace, channel);

    sbuf->buffer = weechat_buffer_new(
        name,
        &slack_buffer_input_cb, sbuf, NULL,
        &slack_buffer_close_cb, sbuf, NULL);

    if (!sbuf->buffer)
    {
        weechat_printf(NULL, "%sweeslack: buffer_new failed for %s",
                        weechat_prefix("error"), name);
        free(sbuf);
        return NULL;
    }

    {
        const char *type = "channel";
        char short_name[128];

        if (channel->type == SLACK_CHANNEL_TYPE_DM ||
            channel->type == SLACK_CHANNEL_TYPE_MPDM)
            type = "private";

        slack_buffer_set_common_localvars(sbuf->buffer, workspace, type);

        {
            const char *ch_name = channel->name ? channel->name : channel->id;
            const char *team = workspace->name ? workspace->name : workspace->id;
            int use_short = weechat_config_boolean(
                weeslack_config.short_buffer_names);

            if (use_short)
            {
                if (channel->type == SLACK_CHANNEL_TYPE_CHANNEL ||
                    channel->type == SLACK_CHANNEL_TYPE_GROUP)
                    snprintf(short_name, sizeof(short_name), "#%s", ch_name);
                else
                    snprintf(short_name, sizeof(short_name), "%s", ch_name);
            }
            else
            {
                /* longer short_name: team.#channel (full_name stays hierarchical) */
                if (channel->type == SLACK_CHANNEL_TYPE_CHANNEL ||
                    channel->type == SLACK_CHANNEL_TYPE_GROUP)
                    snprintf(short_name, sizeof(short_name), "%s.#%s",
                             team ? team : "slack", ch_name);
                else
                    snprintf(short_name, sizeof(short_name), "%s.%s",
                             team ? team : "slack", ch_name);
            }
        }

        weechat_buffer_set(sbuf->buffer, "short_name", short_name);
        weechat_buffer_set(sbuf->buffer, "localvar_set_channel",
                           channel->name ? channel->name : channel->id);
        weechat_buffer_set(sbuf->buffer, "localvar_set_slack_channel_id",
                           channel->id);
        weechat_buffer_set(sbuf->buffer, "localvar_set_slack_type",
                           slack_channel_type_string(channel->type));

        if (channel->type == SLACK_CHANNEL_TYPE_DM && channel->user_id)
            weechat_buffer_set(sbuf->buffer, "localvar_set_slack_dm_user",
                               channel->user_id);

        if (workspace->my_user_id)
            weechat_buffer_set(sbuf->buffer, "localvar_set_nick",
                               workspace->my_user_id);
    }

    weechat_buffer_set(sbuf->buffer, "input_multiline", "1");
    weechat_buffer_set(sbuf->buffer, "input_get_unknown_commands", "1");

    if (channel->type != SLACK_CHANNEL_TYPE_DM)
    {
        weechat_buffer_set(sbuf->buffer, "nicklist", "1");
        /* Show Here/Away group headers (was 0 — flat list, groups invisible). */
        weechat_buffer_set(sbuf->buffer, "nicklist_display_groups", "1");
    }

    if (channel->topic && channel->topic[0])
        weechat_buffer_set(sbuf->buffer, "title", channel->topic);

    /* place after server buffer in the list */
    if (workspace->server_buffer)
    {
        int server_num = weechat_buffer_get_integer(workspace->server_buffer,
                                                     "number");
        char numbuf[32];
        snprintf(numbuf, sizeof(numbuf), "%d", server_num + 1);
        weechat_buffer_set(sbuf->buffer, "number", numbuf);
    }

    weechat_hook_signal("buffer_closing",
                         &slack_buffer_signal_close_cb, sbuf, NULL);

    channel->buffer = sbuf->buffer;

    sbuf->next = slack_buffer_list;
    sbuf->prev = NULL;
    if (slack_buffer_list)
        slack_buffer_list->prev = sbuf;
    slack_buffer_list = sbuf;

    if (channel->is_muted)
        slack_buffer_set_muted(sbuf, 1);

    /*
     * Do not fetch history at create. Opening many buffers at connect
     * rate-limits conversations.history (Tier 3) and leaves most PMs empty.
     * History + nicklist load on first focus (buffer_switch) or
     * /cslack loadhistory.
     */
    (void) workspace;

    return sbuf;
}

struct t_slack_buffer *
slack_buffer_search(struct t_gui_buffer *buffer)
{
    struct t_slack_buffer *sbuf;

    for (sbuf = slack_buffer_list; sbuf; sbuf = sbuf->next)
    {
        if (sbuf->buffer == buffer)
            return sbuf;
    }
    return NULL;
}

struct t_slack_buffer *
slack_buffer_search_by_channel(const char *channel_id)
{
    struct t_slack_buffer *sbuf;

    if (!channel_id)
        return NULL;

    for (sbuf = slack_buffer_list; sbuf; sbuf = sbuf->next)
    {
        if (sbuf->channel && strcmp(sbuf->channel->id, channel_id) == 0)
            return sbuf;
    }
    return NULL;
}

void
slack_buffer_free(struct t_slack_buffer *sbuf)
{
    if (!sbuf)
        return;

    if (sbuf->prev)
        sbuf->prev->next = sbuf->next;
    else
        slack_buffer_list = sbuf->next;

    if (sbuf->next)
        sbuf->next->prev = sbuf->prev;

    if (sbuf->channel)
        sbuf->channel->buffer = NULL;

    free(sbuf);
}

void
slack_buffer_set_topic(struct t_slack_buffer *sbuf, const char *topic)
{
    if (!sbuf || !sbuf->buffer)
        return;

    weechat_buffer_set(sbuf->buffer, "title", topic ? topic : "");
}

void
slack_buffer_set_title(struct t_slack_buffer *sbuf, const char *title)
{
    if (!sbuf || !sbuf->buffer)
        return;

    weechat_buffer_set(sbuf->buffer, "title", title ? title : "");
}

void
slack_buffer_print_message(struct t_slack_buffer *sbuf,
                            struct t_slack_message *msg,
                            const char *nick,
                            const char *message)
{
    if (!sbuf || !sbuf->buffer || !msg || !message)
        return;

    const char *display_nick = nick ? nick : "unknown";

    weechat_printf_date_tags(
        sbuf->buffer,
        msg->ts.sec,
        NULL,
        "%s\t%s",
        display_nick,
        message);
}

static const char *
slack_buffer_nick_color(struct t_slack_user *user, char *hex_buf, size_t hex_size)
{
    const char *color = slack_user_get_color(user);

    if (color && color[0] && color[0] != '|' &&
        strspn(color, "0123456789abcdefABCDEF") == strlen(color) &&
        (strlen(color) == 6 || strlen(color) == 3))
    {
        snprintf(hex_buf, hex_size, "|#%s", color);
        return hex_buf;
    }
    return color && color[0] ? color : "default";
}

/* wee-slack-style nicklist groups (sort prefixes force Here above Away). */
#define SLACK_NICK_GROUP_HERE  "0|Here"
#define SLACK_NICK_GROUP_AWAY  "1|Away"

static struct t_gui_nick_group *
slack_buffer_ensure_nick_group(struct t_gui_buffer *buffer,
                               const char *name, int visible)
{
    struct t_gui_nick_group *grp;

    if (!buffer || !name)
        return NULL;
    grp = weechat_nicklist_search_group(buffer, NULL, name);
    if (grp)
        return grp;
    return weechat_nicklist_add_group(
        buffer, NULL, name, "weechat.color.nicklist_group", visible);
}

static struct t_gui_nick_group *
slack_buffer_nick_group_for_user(struct t_gui_buffer *buffer,
                                 struct t_slack_user *user)
{
    int here = 0;

    if (!buffer || !user)
        return NULL;

    /* Ensure both groups exist so the list stays stable when empty. */
    slack_buffer_ensure_nick_group(buffer, SLACK_NICK_GROUP_HERE, 1);
    slack_buffer_ensure_nick_group(buffer, SLACK_NICK_GROUP_AWAY, 1);

    if (user->presence && strcmp(user->presence, "active") == 0)
        here = 1;

    return slack_buffer_ensure_nick_group(
        buffer,
        here ? SLACK_NICK_GROUP_HERE : SLACK_NICK_GROUP_AWAY,
        1);
}

void
slack_buffer_add_nick(struct t_slack_buffer *sbuf, struct t_slack_user *user)
{
    char hex_color[16];
    const char *nick;
    const char *color;
    const char *prefix;
    const char *prefix_color;
    struct t_gui_nick_group *nick_group;
    struct t_gui_nick *existing;

    if (!sbuf || !sbuf->buffer || !user)
        return;

    /* Bots, app users (Google Drive, …), USLACKBOT — not in roster */
    if (slack_user_hide_from_nicklist(user))
        return;

    nick = slack_user_best_name(user);
    if (!nick || !nick[0])
        nick = user->id;
    if (!nick || !nick[0])
        return;

    /* Drop from any group (Here/Away/legacy "users") before re-add. */
    existing = weechat_nicklist_search_nick(sbuf->buffer, NULL, nick);
    if (existing)
        weechat_nicklist_remove_nick(sbuf->buffer, existing);

    nick_group = slack_buffer_nick_group_for_user(sbuf->buffer, user);
    if (!nick_group)
        return;

    color = slack_buffer_nick_color(user, hex_color, sizeof(hex_color));

    if (user->presence && strcmp(user->presence, "active") == 0)
    {
        prefix = "+";
        prefix_color = "green";
    }
    else if (user->presence && strcmp(user->presence, "away") == 0)
    {
        prefix = "-";
        prefix_color = "yellow";
    }
    else
    {
        prefix = " ";
        prefix_color = NULL;
    }

    weechat_nicklist_add_nick(
        sbuf->buffer,
        nick_group,
        nick,
        color,
        prefix,
        prefix_color,
        1);
}

void
slack_buffer_remove_nick(struct t_slack_buffer *sbuf,
                          struct t_slack_user *user)
{
    const char *nick;
    struct t_gui_nick *n;

    if (!sbuf || !sbuf->buffer || !user)
        return;

    nick = slack_user_best_name(user);
    if (!nick)
        return;

    /* NULL group → search all groups (Here/Away/legacy). */
    n = weechat_nicklist_search_nick(sbuf->buffer, NULL, nick);
    if (n)
        weechat_nicklist_remove_nick(sbuf->buffer, n);
}

void
slack_buffer_clear_nicks(struct t_slack_buffer *sbuf)
{
    if (!sbuf || !sbuf->buffer)
        return;

    weechat_nicklist_remove_all(sbuf->buffer);
}

void
slack_buffer_refresh_nicks(struct t_slack_buffer *sbuf)
{
    if (!sbuf || !sbuf->workspace || !sbuf->channel)
        return;

    slack_buffer_clear_nicks(sbuf);
    if (sbuf->channel->type == SLACK_CHANNEL_TYPE_CHANNEL ||
        sbuf->channel->type == SLACK_CHANNEL_TYPE_GROUP ||
        sbuf->channel->type == SLACK_CHANNEL_TYPE_MPDM)
        slack_event_fetch_members(sbuf->workspace, sbuf->channel);
}

void
slack_buffer_set_muted(struct t_slack_buffer *sbuf, int muted)
{
    if (!sbuf || !sbuf->buffer)
        return;

    if (sbuf->channel)
        sbuf->channel->is_muted = muted ? 1 : 0;

    weechat_buffer_set(sbuf->buffer, "notify", muted ? "none" : "highlight");
    weechat_buffer_set(sbuf->buffer, "localvar_set_slack_muted",
                       muted ? "1" : "0");

    if (muted)
    {
        const char *muted_color = weechat_config_string(
            weeslack_config.color_buflist_muted_channels);
        if (muted_color && muted_color[0])
            weechat_buffer_set(sbuf->buffer, "color_name_inactive", muted_color);
        slack_buffer_clear_hotlist(sbuf->buffer);
    }
    else
    {
        weechat_buffer_set(sbuf->buffer, "color_name_inactive", "");
    }
}

void
slack_buffer_clear_hotlist(struct t_gui_buffer *buffer)
{
    if (!buffer)
        return;
    weechat_buffer_set(buffer, "hotlist", "-1");
}

static int
slack_buffer_typing_clear_cb(const void *pointer, void *data, int remaining_calls)
{
    struct t_slack_channel *channel = (struct t_slack_channel *)pointer;
    (void) data;
    (void) remaining_calls;

    if (!channel)
        return WEECHAT_RC_OK;

    channel->typing_clear_hook = NULL;
    free(channel->typing_user);
    channel->typing_user = NULL;

    if (channel->buffer)
    {
        weechat_buffer_set(channel->buffer, "title",
                           channel->topic ? channel->topic : "");
    }

    return WEECHAT_RC_OK;
}

void
slack_buffer_set_typing(struct t_slack_channel *channel, const char *user_name)
{
    char title[320];
    const char *color;

    if (!channel || !channel->buffer || !user_name || !user_name[0])
        return;

    if (!weechat_config_boolean(weeslack_config.channel_name_typing_indicator))
        return;

    free(channel->typing_user);
    channel->typing_user = strdup(user_name);

    color = weechat_config_string(weeslack_config.color_typing_notice);
    if (!color || !color[0])
        color = "cyan";

    snprintf(title, sizeof(title), "%s%s is typing…%s%s%s",
             weechat_color(color),
             user_name,
             weechat_color("reset"),
             channel->topic && channel->topic[0] ? " — " : "",
             channel->topic ? channel->topic : "");
    weechat_buffer_set(channel->buffer, "title", title);

    if (channel->typing_clear_hook)
        weechat_unhook(channel->typing_clear_hook);

    channel->typing_clear_hook = weechat_hook_timer(
        5000, 0, 1,
        &slack_buffer_typing_clear_cb, channel, NULL);
}

void
slack_buffer_purge_hidden_nicks(void)
{
    struct t_slack_user *user;
    struct t_slack_channel *ch;

    for (user = slack_user_list_global(); user; user = user->next)
    {
        if (!slack_user_hide_from_nicklist(user))
            continue;
        for (ch = slack_channel_list_global(); ch; ch = ch->next)
        {
            struct t_slack_buffer *sbuf;
            if (!ch->buffer)
                continue;
            sbuf = slack_buffer_search(ch->buffer);
            if (!sbuf)
                sbuf = slack_buffer_search_by_channel(ch->id);
            if (sbuf)
                slack_buffer_remove_nick(sbuf, user);
        }
    }
}

void
slack_buffer_update_user_presence(struct t_slack_user *user)
{
    const char *nick;
    struct t_slack_channel *ch;

    if (!user)
        return;

    if (slack_user_hide_from_nicklist(user))
    {
        /* Ensure bots never linger in nicklists after profile updates */
        for (ch = slack_channel_list_global(); ch; ch = ch->next)
        {
            struct t_slack_buffer *sbuf;
            if (!ch->buffer)
                continue;
            sbuf = slack_buffer_search(ch->buffer);
            if (!sbuf)
                sbuf = slack_buffer_search_by_channel(ch->id);
            if (sbuf)
                slack_buffer_remove_nick(sbuf, user);
        }
        return;
    }

    nick = slack_user_best_name(user);
    if (!nick)
        return;

    /* Re-home nick into Here vs Away (wee-slack style). */
    for (ch = slack_channel_list_global(); ch; ch = ch->next)
    {
        struct t_slack_buffer *sbuf;

        if (!ch->buffer)
            continue;
        sbuf = slack_buffer_search(ch->buffer);
        if (!sbuf)
            sbuf = slack_buffer_search_by_channel(ch->id);
        if (!sbuf)
            continue;
        /* only touch channels that already show this user */
        if (!weechat_nicklist_search_nick(ch->buffer, NULL, nick))
            continue;
        slack_buffer_add_nick(sbuf, user);
    }
}
