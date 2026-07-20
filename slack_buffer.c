#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>

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

/* Unescape \/ → / in sed pattern/replacement fragments. */
static char *
slack_buffer_unescape_slashes(const char *s, size_t len)
{
    char *out;
    size_t i, j;

    out = malloc(len + 1);
    if (!out)
        return NULL;
    for (i = 0, j = 0; i < len; i++)
    {
        if (s[i] == '\\' && i + 1 < len && s[i + 1] == '/')
        {
            out[j++] = '/';
            i++;
        }
        else
            out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

/* Own-message filter for sed edits */
static int
slack_buffer_filter_own(struct t_slack_message *msg, void *data)
{
    const char *my_id = data;
    if (!msg || msg->is_deleted || !my_id)
        return 0;
    return (msg->user_id && strcmp(msg->user_id, my_id) == 0);
}

/*
 * wee-slack style: [$hash|N]s/old/new/[flags]
 * empty old+new → chat.delete; else chat.update with POSIX ERE replace.
 * flags: g (global), i (ignore case). Returns 1 if handled.
 */
static int
slack_buffer_try_sed_edit(struct t_slack_buffer *sbuf,
                           const char *channel_id,
                           const char *input)
{
    const char *p = input;
    char ref_buf[64];
    const char *ref = NULL;
    const char *s_start;
    char *old_pat = NULL, *new_pat = NULL, *result = NULL;
    const char *flags = "";
    const char *slash1, *slash2;
    struct t_slack_message *msg;
    char *ts_str;
    int cflags = REG_EXTENDED;
    regex_t re;
    int global = 0;
    int handled = 0;

    if (!sbuf || !sbuf->workspace || !channel_id || !input)
        return 0;

    /* optional leading $hash or index before s/ */
    if (p[0] == '$' || isdigit((unsigned char)p[0]))
    {
        size_t i = 0;
        if (p[0] == '$')
        {
            ref_buf[i++] = *p++;
            while (i + 1 < sizeof(ref_buf) && isxdigit((unsigned char)*p))
                ref_buf[i++] = *p++;
        }
        else
        {
            while (i + 1 < sizeof(ref_buf) && isdigit((unsigned char)*p))
                ref_buf[i++] = *p++;
        }
        ref_buf[i] = '\0';
        if (p[0] == 's')
            ref = ref_buf;
        else
            p = input; /* not sed with prefix */
    }

    if (p[0] != 's' || p[1] != '/')
        return 0;

    s_start = p + 2;
    /* split on unescaped / → old / new / flags */
    slash1 = NULL;
    for (p = s_start; *p; p++)
    {
        if (*p == '/' && (p == s_start || p[-1] != '\\'))
        {
            slash1 = p;
            break;
        }
    }
    if (!slash1)
    {
        weechat_printf(sbuf->buffer,
                        "%sweeslack: incomplete s/// (use s/old/new/)",
                        weechat_prefix("error"));
        return 1;
    }
    slash2 = NULL;
    for (p = slash1 + 1; *p; p++)
    {
        if (*p == '/' && p[-1] != '\\')
        {
            slash2 = p;
            break;
        }
    }
    if (!slash2)
    {
        weechat_printf(sbuf->buffer,
                        "%sweeslack: incomplete s/// (use s/old/new/)",
                        weechat_prefix("error"));
        return 1;
    }
    flags = slash2 + 1;

    old_pat = slack_buffer_unescape_slashes(s_start,
                                             (size_t)(slash1 - s_start));
    new_pat = slack_buffer_unescape_slashes(slash1 + 1,
                                             (size_t)(slash2 - slash1 - 1));
    if (!old_pat || !new_pat)
        goto done;

    msg = slack_message_from_ref(
        sbuf->channel->messages,
        ref,
        slack_buffer_filter_own,
        (void *)sbuf->workspace->my_user_id);
    if (!msg || !msg->text)
    {
        weechat_printf(sbuf->buffer,
                        "%sweeslack: no matching own message to edit",
                        weechat_prefix("error"));
        handled = 1;
        goto done;
    }

    ts_str = slack_ts_to_string(msg->ts);
    if (!ts_str)
        goto done;

    /* s/// or s//  with empty old+new → delete */
    if (old_pat[0] == '\0' && new_pat[0] == '\0')
    {
        slack_event_delete_message(sbuf->workspace, channel_id, ts_str);
        free(ts_str);
        handled = 1;
        goto done;
    }

    if (strchr(flags, 'i') || strchr(flags, 'I'))
        cflags |= REG_ICASE;
    if (strchr(flags, 'g') || strchr(flags, 'G'))
        global = 1;

    if (regcomp(&re, old_pat, cflags) != 0)
    {
        weechat_printf(sbuf->buffer,
                        "%sweeslack: invalid regex in s///",
                        weechat_prefix("error"));
        free(ts_str);
        handled = 1;
        goto done;
    }

    {
        const char *src = msg->text;
        size_t cap = strlen(src) * 2 + strlen(new_pat) + 64;
        size_t pos = 0;
        int matches = 0;

        result = malloc(cap);
        if (!result)
        {
            regfree(&re);
            free(ts_str);
            goto done;
        }
        result[0] = '\0';

        while (*src)
        {
            regmatch_t m;
            size_t before, mid, after_need;
            if (regexec(&re, src, 1, &m, 0) != 0)
            {
                size_t rest = strlen(src);
                if (pos + rest + 1 > cap)
                {
                    char *n = realloc(result, pos + rest + 1);
                    if (!n)
                        break;
                    result = n;
                    cap = pos + rest + 1;
                }
                memcpy(result + pos, src, rest);
                pos += rest;
                result[pos] = '\0';
                break;
            }
            matches++;
            before = (size_t)m.rm_so;
            mid = strlen(new_pat);
            after_need = pos + before + mid + 1;
            if (after_need > cap)
            {
                char *n = realloc(result, after_need + 256);
                if (!n)
                    break;
                result = n;
                cap = after_need + 256;
            }
            if (before)
            {
                memcpy(result + pos, src, before);
                pos += before;
            }
            memcpy(result + pos, new_pat, mid);
            pos += mid;
            result[pos] = '\0';
            src += m.rm_eo;
            if (!global || m.rm_eo == 0)
            {
                /* append rest after first match if not global */
                if (!global)
                {
                    size_t rest = strlen(src);
                    if (pos + rest + 1 > cap)
                    {
                        char *n = realloc(result, pos + rest + 1);
                        if (n)
                        {
                            result = n;
                            memcpy(result + pos, src, rest);
                            pos += rest;
                            result[pos] = '\0';
                        }
                    }
                    else
                    {
                        memcpy(result + pos, src, rest);
                        pos += rest;
                        result[pos] = '\0';
                    }
                }
                if (m.rm_eo == 0 && global)
                    src++; /* avoid infinite empty match */
                if (!global)
                    break;
            }
        }
        regfree(&re);

        if (matches == 0)
        {
            weechat_printf(sbuf->buffer,
                            "%sweeslack: regex did not match the message",
                            weechat_prefix("error"));
            free(ts_str);
            handled = 1;
            goto done;
        }

        if (strcmp(result, msg->text) != 0)
            slack_event_update_message(sbuf->workspace, channel_id, ts_str,
                                        result);
        free(ts_str);
        handled = 1;
    }

done:
    free(old_pat);
    free(new_pat);
    free(result);
    return handled;
}

/*
 * [$hash|N]+emoji / -emoji (wee-slack reaction shortcuts).
 * Returns 1 if handled.
 */
static int
slack_buffer_try_reaction_input(struct t_slack_buffer *sbuf,
                                 const char *channel_id,
                                 const char *input)
{
    const char *p = input;
    char ref_buf[64];
    const char *ref = NULL;
    int add = 0;
    const char *emoji;
    struct t_slack_message *msg;
    char *ts_str;
    char name[64];

    if (!sbuf || !channel_id || !input)
        return 0;

    if (p[0] == '$' || isdigit((unsigned char)p[0]))
    {
        size_t i = 0;
        if (p[0] == '$')
        {
            ref_buf[i++] = *p++;
            while (i + 1 < sizeof(ref_buf) && isxdigit((unsigned char)*p))
                ref_buf[i++] = *p++;
        }
        else
        {
            while (i + 1 < sizeof(ref_buf) && isdigit((unsigned char)*p))
                ref_buf[i++] = *p++;
        }
        ref_buf[i] = '\0';
        if (p[0] == '+' || p[0] == '-')
            ref = ref_buf;
        else
            return 0;
    }
    if (p[0] == '+')
        add = 1;
    else if (p[0] == '-')
        add = 0;
    else
        return 0;
    p++;
    if (!*p)
        return 0;

    emoji = p;
    if (emoji[0] == ':')
    {
        size_t n = strlen(emoji);
        if (n >= 3 && emoji[n - 1] == ':' && n - 2 < sizeof(name))
        {
            memcpy(name, emoji + 1, n - 2);
            name[n - 2] = '\0';
            emoji = name;
        }
    }

    msg = slack_message_from_ref(sbuf->channel->messages, ref, NULL, NULL);
    if (!msg)
        return 0;

    ts_str = slack_ts_to_string(msg->ts);
    if (!ts_str)
        return 1;
    slack_event_react(sbuf->workspace, channel_id, ts_str, emoji, add);
    free(ts_str);
    return 1;
}

static int
slack_buffer_input_cb(const void *pointer, void *data,
                      struct t_gui_buffer *buffer,
                      const char *input_data)
{
    (void) data;
    (void) buffer;

    struct t_slack_buffer *sbuf = (struct t_slack_buffer *)pointer;
    const char *thread_ts = NULL;
    const char *channel_id;
    char *processed;

    if (!sbuf || !sbuf->channel || !input_data || !input_data[0])
        return WEECHAT_RC_OK;

    if (!sbuf->workspace || !sbuf->workspace->connected)
    {
        weechat_printf(sbuf->buffer, "%sweeslack: not connected",
                        weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    channel_id = sbuf->channel->id;

    if (sbuf->channel->type == SLACK_CHANNEL_TYPE_THREAD)
    {
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

    /* sed-like edit / delete before emoji expansion (patterns are raw text) */
    if (slack_buffer_try_sed_edit(sbuf, channel_id, input_data))
        return WEECHAT_RC_OK;

    /* +emoji / -emoji reaction shortcuts */
    if (slack_buffer_try_reaction_input(sbuf, channel_id, input_data))
        return WEECHAT_RC_OK;

    /* replace emoji shortcodes with unicode before sending */
    processed = slack_event_replace_emoji(input_data);
    if (!processed)
        return WEECHAT_RC_OK;

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
                if (channel->type == SLACK_CHANNEL_TYPE_GROUP)
                {
                    const char *gpfx = weechat_config_string(
                        weeslack_config.group_name_prefix);
                    if (!gpfx || !gpfx[0])
                        gpfx = "&";
                    snprintf(short_name, sizeof(short_name), "%s%s",
                             gpfx, ch_name);
                }
                else if (channel->type == SLACK_CHANNEL_TYPE_CHANNEL)
                    snprintf(short_name, sizeof(short_name), "#%s", ch_name);
                else
                    snprintf(short_name, sizeof(short_name), "%s", ch_name);
            }
            else
            {
                /* longer short_name: team.#channel (full_name stays hierarchical) */
                if (channel->type == SLACK_CHANNEL_TYPE_GROUP)
                {
                    const char *gpfx = weechat_config_string(
                        weeslack_config.group_name_prefix);
                    if (!gpfx || !gpfx[0])
                        gpfx = "&";
                    snprintf(short_name, sizeof(short_name), "%s.%s%s",
                             team ? team : "slack", gpfx, ch_name);
                }
                else if (channel->type == SLACK_CHANNEL_TYPE_CHANNEL)
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

/*
 * Walk buffer own_lines for tag slack_ts_<sec.usec> and rewrite "message"
 * via hdata_update (same technique as Python wee-slack modify_buffer_line).
 * WeeChat ≥ 4 can rewrite a multi-line message with one update.
 */
int
slack_buffer_modify_line(struct t_gui_buffer *buffer, SlackTS ts,
                          const char *new_message)
{
    struct t_hdata *h_buffer, *h_lines, *h_line, *h_line_data;
    void *own_lines, *line_ptr;
    char want_tag[64];
    char *ts_str;
    int found = 0;

    if (!buffer || !new_message)
        return 0;

    ts_str = slack_ts_to_string(ts);
    if (!ts_str)
        return 0;
    snprintf(want_tag, sizeof(want_tag), "slack_ts_%s", ts_str);
    free(ts_str);

    h_buffer = weechat_hdata_get("buffer");
    h_lines = weechat_hdata_get("lines");
    h_line = weechat_hdata_get("line");
    h_line_data = weechat_hdata_get("line_data");
    if (!h_buffer || !h_lines || !h_line || !h_line_data)
        return 0;

    own_lines = weechat_hdata_pointer(h_buffer, buffer, "own_lines");
    if (!own_lines)
        return 0;

    line_ptr = weechat_hdata_pointer(h_lines, own_lines, "last_line");
    while (line_ptr)
    {
        void *data = weechat_hdata_pointer(h_line, line_ptr, "data");
        int tags_count, i, match = 0;

        if (!data)
        {
            line_ptr = weechat_hdata_move(h_line, line_ptr, -1);
            continue;
        }

        tags_count = weechat_hdata_integer(h_line_data, data, "tags_count");
        for (i = 0; i < tags_count; i++)
        {
            char key[32];
            const char *tag;

            snprintf(key, sizeof(key), "%d|tags_array", i);
            tag = weechat_hdata_string(h_line_data, data, key);
            if (tag && strcmp(tag, want_tag) == 0)
            {
                match = 1;
                break;
            }
        }

        if (match)
        {
            struct t_hashtable *ht;

            ht = weechat_hashtable_new(8,
                                        WEECHAT_HASHTABLE_STRING,
                                        WEECHAT_HASHTABLE_STRING,
                                        NULL, NULL);
            if (ht)
            {
                weechat_hashtable_set(ht, "message", new_message);
                weechat_hdata_update(h_line_data, data, ht);
                weechat_hashtable_free(ht);
                found = 1;
            }
            /* WeeChat 4+: one update rewrites the whole logical line set */
            break;
        }

        line_ptr = weechat_hdata_move(h_line, line_ptr, -1);
    }

    return found;
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

    /*
     * muted_channels_activity: none → notify none; personal/all_highlights →
     * highlight; all → message (activity like unmuted).
     */
    if (muted)
    {
        int mode = weechat_config_integer(
            weeslack_config.muted_channels_activity);
        const char *notify = "none";

        if (mode == 3) /* all */
            notify = "message";
        else if (mode == 1 || mode == 2) /* personal / all highlights */
            notify = "highlight";
        weechat_buffer_set(sbuf->buffer, "notify", notify);
    }
    else
        weechat_buffer_set(sbuf->buffer, "notify", "highlight");

    weechat_buffer_set(sbuf->buffer, "localvar_set_slack_muted",
                       muted ? "1" : "0");

    if (muted)
    {
        const char *muted_color = weechat_config_string(
            weeslack_config.color_buflist_muted_channels);
        if (muted_color && muted_color[0])
            weechat_buffer_set(sbuf->buffer, "color_name_inactive", muted_color);
        if (weechat_config_integer(weeslack_config.muted_channels_activity) == 0)
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
