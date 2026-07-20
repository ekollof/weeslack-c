#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

#include "weeslack.h"
#include "slack_event.h"
#include "slack_data.h"
#include "slack_buffer.h"
#include "slack_http.h"
#include "slack_ws.h"

static char *slack_event_render_formatting(const char *text);
static char *slack_event_format_files(struct json_object *msg_json);
static char *slack_event_format_attachments(struct json_object *msg_json);
static char *slack_event_apply_emoji_mode(const char *text);
static char *slack_event_format_user(struct t_slack_user *user,
                                     const char *fallback_id,
                                     struct t_slack_channel *channel);
static void slack_event_emoji_cb(struct t_weeslack_workspace *workspace,
                                 int return_code, const char *output,
                                 void *user_data);
/* defined later in bootstrap chain */
void slack_event_fetch_emoji(struct t_weeslack_workspace *workspace);

/* ============================================================
 * Common error checking for Slack API responses
 * ============================================================ */

int
slack_api_check_error(struct t_weeslack_workspace *workspace,
                      struct json_object *json, const char *context)
{
    struct json_object *ok_obj;
    if (!json_object_object_get_ex(json, "ok", &ok_obj) ||
        !json_object_get_boolean(ok_obj))
    {
        struct json_object *error_obj;
        const char *error_str = "unknown";
        if (json_object_object_get_ex(json, "error", &error_obj))
            error_str = json_object_get_string(error_obj);

        if (strcmp(error_str, "invalid_auth") == 0 ||
            strcmp(error_str, "token_revoked") == 0 ||
            strcmp(error_str, "token_expired") == 0)
        {
            SLACK_WS_PRINTF(workspace,
                            "%sweeslack: authentication failed (%s) — "
                            "token may be expired or revoked",
                            weechat_prefix("error"), error_str);
        }
        else if (strcmp(error_str, "ratelimited") == 0)
        {
            /* HTTP layer retries; this is a final failure after retries */
            SLACK_WS_PRINTF(workspace,
                            "%sweeslack: rate limited by Slack API (%s) "
                            "after retries",
                            weechat_prefix("error"), context);
        }
        else
        {
            SLACK_WS_PRINTF(workspace,
                            "%sweeslack: %s API error: %s",
                            weechat_prefix("error"), context, error_str);
        }
        return 1;
    }
    return 0;
}

/* ============================================================
 * Emoji replacement (common shortcodes → unicode)
 * ============================================================ */

struct slack_emoji_entry
{
    const char *name;
    const char *unicode;
};

/* Custom emoji from emoji.list (name → display replacement) */
struct t_slack_custom_emoji
{
    char *name;
    char *value; /* unicode, URL, or alias target */
    struct t_slack_custom_emoji *next;
};

static struct t_slack_custom_emoji *slack_custom_emoji_list = NULL;

static void
slack_custom_emoji_clear(void)
{
    struct t_slack_custom_emoji *e, *next;
    for (e = slack_custom_emoji_list; e; e = next)
    {
        next = e->next;
        free(e->name);
        free(e->value);
        free(e);
    }
    slack_custom_emoji_list = NULL;
}

static void
slack_custom_emoji_add(const char *name, const char *value)
{
    struct t_slack_custom_emoji *e;

    if (!name || !name[0] || !value)
        return;

    e = calloc(1, sizeof(*e));
    if (!e)
        return;
    e->name = strdup(name);
    e->value = strdup(value);
    e->next = slack_custom_emoji_list;
    slack_custom_emoji_list = e;
}

static const char *
slack_custom_emoji_lookup(const char *name)
{
    struct t_slack_custom_emoji *e;
    for (e = slack_custom_emoji_list; e; e = e->next)
    {
        if (strcmp(e->name, name) == 0)
            return e->value;
    }
    return NULL;
}

static const struct slack_emoji_entry slack_emoji_common[] = {
    { "+1", "\xF0\x9F\x91\x8D" },         /* 👍 */
    { "-1", "\xF0\x9F\x91\x8E" },         /* 👎 */
    { "thumbsup", "\xF0\x9F\x91\x8D" },   /* 👍 */
    { "thumbsdown", "\xF0\x9F\x91\x8E" }, /* 👎 */
    { "heart", "\xE2\x9D\xA4" },           /* ❤ */
    { "smile", "\xF0\x9F\x98\x84" },       /* 😄 */
    { "laughing", "\xF0\x9F\x98\x82" },    /* 😂 */
    { "joy", "\xF0\x9F\x98\x82" },         /* 😂 */
    { "cry", "\xF0\x9F\x98\xA2" },         /* 😢 */
    { "angry", "\xF0\x9F\x98\xA0" },       /* 😠 */
    { "thumbsup", "\xF0\x9F\x91\x8D" },   /* 👍 */
    { "fire", "\xF0\x9F\x94\xA5" },        /* 🔥 */
    { "rocket", "\xF0\x9F\x9A\x80" },      /* 🚀 */
    { "eyes", "\xF0\x9F\x91\x80" },        /* 👀 */
    { "tada", "\xF0\x9F\x8E\x89" },        /* 🎉 */
    { "wave", "\xF0\x9F\x91\x8B" },        /* 👋 */
    { "clap", "\xF0\x9F\x91\x8F" },        /* 👏 */
    { "ok_hand", "\xF0\x9F\x91\x8C" },     /* 👌 */
    { "pray", "\xF0\x9F\x99\x8F" },        /* 🙏 */
    { "sparkles", "\xE2\x9C\xA8" },        /* ✨ */
    { "star", "\xE2\xAD\x90" },            /* ⭐ */
    { "white_check_mark", "\xE2\x9C\x85" }, /* ✅ */
    { "x", "\xE2\x9D\x8C" },               /* ❌ */
    { "warning", "\xE2\x9A\xA0" },          /* ⚠ */
    { "bulb", "\xF0\x9F\x92\xA1" },        /* 💡 */
    { "memo", "\xF0\x9F\x93\x9D" },        /* 📝 */
    { "bug", "\xF0\x9F\x90\x9B" },         /* 🐛 */
    { "lock", "\xF0\x9F\x94\x92" },        /* 🔒 */
    { "unlock", "\xF0\x9F\x94\x93" },      /* 🔓 */
    { "link", "\xF0\x9F\x97\x97" },        /* 🔗 */
    { "confused", "\xF0\x9F\x98\x95" },    /* 😕 */
    { "slightly_smiling_face", "\xF0\x9F\x99\x82" }, /* 🙂 */
    { "thinking_face", "\xF0\x9F\xA4\x94" }, /* 🤔 */
    { "eyes", "\xF0\x9F\x91\x80" },
    { "100", "\xF0\x9F\x92\xAF" },
    { "pray", "\xF0\x9F\x99\x8F" },
    { "raised_hands", "\xF0\x9F\x99\x8C" },
    { "sweat_smile", "\xF0\x9F\x98\x85" },
    { "grinning", "\xF0\x9F\x98\x80" },
    { "wink", "\xF0\x9F\x98\x89" },
    { "sob", "\xF0\x9F\x98\xAD" },
    { "boom", "\xF0\x9F\x92\xA5" },
    { "tada", "\xF0\x9F\x8E\x89" },
    { NULL, NULL }
};

char *
slack_event_replace_emoji(const char *text)
{
    if (!text)
        return strdup("");

    size_t len = strlen(text);
    char *result = malloc(len * 4 + 1);
    if (!result)
        return strdup(text);

    const char *src = text;
    char *dst = result;

    while (*src)
    {
        if (*src == ':')
        {
            const char *end = strchr(src + 1, ':');
            if (end && (end - src - 1) > 0 && (end - src - 1) < 32)
            {
                char shortcode[32];
                size_t sc_len = end - src - 1;
                memcpy(shortcode, src + 1, sc_len);
                shortcode[sc_len] = '\0';

                const char *custom = slack_custom_emoji_lookup(shortcode);
                if (custom && custom[0])
                {
                    /* alias:foo → resolve once; URLs stay as :name: for TUI */
                    if (strncmp(custom, "alias:", 6) == 0)
                    {
                        const char *a = slack_custom_emoji_lookup(custom + 6);
                        if (a && a[0] && strncmp(a, "http", 4) != 0)
                            custom = a;
                        else
                            custom = NULL;
                    }
                    if (custom && strncmp(custom, "http", 4) != 0)
                    {
                        size_t u_len = strlen(custom);
                        memcpy(dst, custom, u_len);
                        dst += u_len;
                        src = end + 1;
                        goto next;
                    }
                }

                const struct slack_emoji_entry *e;
                for (e = slack_emoji_common; e->name; e++)
                {
                    if (strcmp(e->name, shortcode) == 0)
                    {
                        size_t u_len = strlen(e->unicode);
                        memcpy(dst, e->unicode, u_len);
                        dst += u_len;
                        src = end + 1;
                        goto next;
                    }
                }
            }
        }

        *dst++ = *src++;
        next:;
    }
    *dst = '\0';

    return result;
}

/* emoji_render_mode: 0=emoji unicode, 1=keep :shortcodes:, 2=strip colons to text */
static char *
slack_event_apply_emoji_mode(const char *text)
{
    int mode;

    if (!text)
        return strdup("");

    mode = weechat_config_integer(weeslack_config.emoji_render_mode);
    if (mode == 1)
        return strdup(text);
    if (mode == 2)
    {
        /* :smile: → smile */
        size_t len = strlen(text);
        char *result = malloc(len + 1);
        const char *src = text;
        char *dst;

        if (!result)
            return strdup(text);
        dst = result;
        while (*src)
        {
            if (*src == ':')
            {
                const char *end = strchr(src + 1, ':');
                if (end && end > src + 1 && (end - src) < 34)
                {
                    size_t n = (size_t)(end - src - 1);
                    memcpy(dst, src + 1, n);
                    dst += n;
                    src = end + 1;
                    continue;
                }
            }
            *dst++ = *src++;
        }
        *dst = '\0';
        return result;
    }

    return slack_event_replace_emoji(text);
}

static char *
slack_event_format_user(struct t_slack_user *user, const char *fallback_id,
                        struct t_slack_channel *channel)
{
    const char *name = "unknown";
    const char *color = "default";
    char hex_color[16];
    struct t_slack_bot *bot = NULL;
    int is_private = 0;
    int colorize = 1;

    if (channel &&
        (channel->type == SLACK_CHANNEL_TYPE_DM ||
         channel->type == SLACK_CHANNEL_TYPE_MPDM))
        is_private = 1;

    if (is_private &&
        !weechat_config_boolean(weeslack_config.colorize_private_chats))
        colorize = 0;

    if (user)
    {
        if (user->display_name && user->display_name[0])
            name = user->display_name;
        else if (user->real_name && user->real_name[0])
            name = user->real_name;
        else if (user->name && user->name[0])
            name = user->name;
        if (colorize)
        {
            color = slack_user_get_color(user);
            if (color && color[0] && color[0] != '|' &&
                strspn(color, "0123456789abcdefABCDEF") == strlen(color) &&
                (strlen(color) == 6 || strlen(color) == 3))
            {
                snprintf(hex_color, sizeof(hex_color), "|#%s", color);
                color = hex_color;
            }
        }
    }
    else if (fallback_id && fallback_id[0])
    {
        bot = slack_bot_search(fallback_id);
        if (bot)
            name = slack_bot_best_name(bot);
        else
            name = fallback_id;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "%s%s%s",
             weechat_color(color),
             name,
             weechat_color("reset"));
    return strdup(buf);
}

/* Pull plain text out of Block Kit (section / rich_text) when "text" is empty. */
static void
slack_blocks_collect_text(struct json_object *node, char *out, size_t out_size,
                          size_t *pos)
{
    enum json_type t;
    size_t i, n;

    if (!node || !out || !pos || *pos + 1 >= out_size)
        return;

    t = json_object_get_type(node);
    if (t == json_type_object)
    {
        struct json_object *text_obj, *elements, *type_obj;
        const char *type = NULL;

        if (json_object_object_get_ex(node, "type", &type_obj))
            type = json_object_get_string(type_obj);

        /* section / header / button style: { "text": { "text": "..." } }
         * or plain { "text": "..." } */
        if (json_object_object_get_ex(node, "text", &text_obj))
        {
            if (json_object_is_type(text_obj, json_type_string))
            {
                const char *s = json_object_get_string(text_obj);
                if (s && s[0] && *pos + 1 < out_size)
                {
                    int w = snprintf(out + *pos, out_size - *pos, "%s%s",
                                     *pos ? " " : "", s);
                    if (w > 0)
                        *pos += (size_t)w < out_size - *pos
                            ? (size_t)w : out_size - *pos - 1;
                }
            }
            else if (json_object_is_type(text_obj, json_type_object))
                slack_blocks_collect_text(text_obj, out, out_size, pos);
        }

        if (json_object_object_get_ex(node, "elements", &elements) &&
            json_object_is_type(elements, json_type_array))
        {
            n = (size_t)json_object_array_length(elements);
            for (i = 0; i < n; i++)
                slack_blocks_collect_text(
                    json_object_array_get_idx(elements, i),
                    out, out_size, pos);
        }

        /* rich_text_section leaves often have "text" string already handled */
        (void) type;
    }
    else if (t == json_type_array)
    {
        n = (size_t)json_object_array_length(node);
        for (i = 0; i < n; i++)
            slack_blocks_collect_text(json_object_array_get_idx(node, i),
                                      out, out_size, pos);
    }
}

/* Decode common Slack/HTML entities in message text (malloc; caller frees). */
static char *
slack_event_unescape_html(const char *text)
{
    size_t len, cap, pos = 0;
    char *out;
    const char *src;

    if (!text)
        return strdup("");
    len = strlen(text);
    /* numeric entities can expand to multi-byte UTF-8 */
    cap = len * 4 + 1;
    out = malloc(cap);
    if (!out)
        return strdup(text);

    for (src = text; *src && pos + 4 < cap; )
    {
        if (src[0] == '&')
        {
            if (strncmp(src, "&amp;", 5) == 0)
            { out[pos++] = '&'; src += 5; continue; }
            if (strncmp(src, "&lt;", 4) == 0)
            { out[pos++] = '<'; src += 4; continue; }
            if (strncmp(src, "&gt;", 4) == 0)
            { out[pos++] = '>'; src += 4; continue; }
            if (strncmp(src, "&quot;", 6) == 0)
            { out[pos++] = '"'; src += 6; continue; }
            if (strncmp(src, "&#39;", 5) == 0)
            { out[pos++] = '\''; src += 5; continue; }
            if (strncmp(src, "&apos;", 6) == 0)
            { out[pos++] = '\''; src += 6; continue; }
            /* numeric &#NNN; or &#xHH; */
            if (src[1] == '#' && src[2])
            {
                char *end = NULL;
                unsigned long cp;
                if (src[2] == 'x' || src[2] == 'X')
                    cp = strtoul(src + 3, &end, 16);
                else
                    cp = strtoul(src + 2, &end, 10);
                if (end && *end == ';' && cp > 0 && cp < 0x110000)
                {
                    if (cp < 0x80)
                        out[pos++] = (char)cp;
                    else if (cp < 0x800)
                    {
                        out[pos++] = (char)(0xC0 | (cp >> 6));
                        out[pos++] = (char)(0x80 | (cp & 0x3F));
                    }
                    else if (cp < 0x10000)
                    {
                        out[pos++] = (char)(0xE0 | (cp >> 12));
                        out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[pos++] = (char)(0x80 | (cp & 0x3F));
                    }
                    else
                    {
                        out[pos++] = (char)(0xF0 | (cp >> 18));
                        out[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[pos++] = (char)(0x80 | (cp & 0x3F));
                    }
                    src = end + 1;
                    continue;
                }
            }
        }
        out[pos++] = *src++;
    }
    out[pos] = '\0';
    return out;
}

/* notify_* tags: highlight when message mentions this workspace user. */
static const char *
slack_event_notify_tags(struct t_weeslack_workspace *workspace,
                        struct t_slack_channel *channel,
                        const char *text,
                        int history)
{
    if (history)
        return "no_highlight,notify_none,no_log,logger_backlog";
    if (channel && channel->is_muted)
        return "no_highlight,notify_none,log1";

    if (workspace && workspace->my_user_id && text && text[0])
    {
        char needle[80];
        snprintf(needle, sizeof(needle), "<@%s", workspace->my_user_id);
        if (strstr(text, needle) != NULL)
            return "notify_highlight,log1,slack_mention";
        /* also plain @display_name if we can resolve self */
        {
            struct t_slack_user *me = slack_user_search(workspace->my_user_id);
            const char *dn = me ? slack_user_best_name(me) : NULL;
            if (dn && dn[0])
            {
                char at[128];
                snprintf(at, sizeof(at), "@%s", dn);
                if (strstr(text, at) != NULL)
                    return "notify_highlight,log1,slack_mention";
            }
        }
    }
    return "notify_message,log1";
}

static char *
slack_event_message_text(struct json_object *msg_json)
{
    struct json_object *text_obj, *blocks_obj;
    const char *text = NULL;
    char buf[4096];
    size_t pos = 0;
    char *raw = NULL;
    char *unesc;

    if (json_object_object_get_ex(msg_json, "text", &text_obj))
        text = json_object_get_string(text_obj);
    if (text && text[0])
        raw = strdup(text);
    else
    {
        buf[0] = '\0';
        if (json_object_object_get_ex(msg_json, "blocks", &blocks_obj))
            slack_blocks_collect_text(blocks_obj, buf, sizeof(buf), &pos);
        raw = buf[0] ? strdup(buf) : strdup("");
    }

    if (!raw)
        return strdup("");
    unesc = slack_event_unescape_html(raw);
    free(raw);
    return unesc ? unesc : strdup("");
}

static void
slack_event_process_message(struct t_weeslack_workspace *workspace,
                             struct t_slack_channel *channel,
                             struct json_object *msg_json,
                             int history)
{
    (void) history;

    struct json_object *ts_obj, *user_obj;
    const char *ts_str = NULL;
    const char *user_id = NULL;
    char *text_owned = NULL;
    const char *text = "";
    const char *subtype = NULL;
    int is_me = 0;

    json_object_object_get_ex(msg_json, "ts", &ts_obj);
    if (ts_obj)
        ts_str = json_object_get_string(ts_obj);

    json_object_object_get_ex(msg_json, "user", &user_obj);
    if (user_obj)
        user_id = json_object_get_string(user_obj);
    if (!user_id)
    {
        struct json_object *bot_obj;
        if (json_object_object_get_ex(msg_json, "bot_id", &bot_obj))
            user_id = json_object_get_string(bot_obj);
        else if (json_object_object_get_ex(msg_json, "username", &bot_obj))
            user_id = json_object_get_string(bot_obj);
    }

    text_owned = slack_event_message_text(msg_json);
    text = text_owned ? text_owned : "";

    struct json_object *subtype_obj;
    if (json_object_object_get_ex(msg_json, "subtype", &subtype_obj))
        subtype = json_object_get_string(subtype_obj);

    if (subtype && strcmp(subtype, "me_message") == 0)
        is_me = 1;

    SlackTS ts = slack_ts_new(ts_str);

    if (!channel)
    {
        free(text_owned);
        return;
    }

    /* ensure a buffer exists for live traffic */
    if (!channel->buffer && workspace)
        slack_buffer_new(workspace, channel);

    /* handle subtypes */
    if (subtype && strcmp(subtype, "message_changed") == 0)
    {
        struct json_object *message_obj;
        free(text_owned);
        text_owned = NULL;
        if (json_object_object_get_ex(msg_json, "message", &message_obj))
        {
            struct json_object *new_ts_obj;
            char *new_text_owned;
            const char *new_ts_str = NULL;
            const char *new_text = "";
            const char *edit_color;

            json_object_object_get_ex(message_obj, "ts", &new_ts_obj);
            if (new_ts_obj)
                new_ts_str = json_object_get_string(new_ts_obj);

            new_text_owned = slack_event_message_text(message_obj);
            new_text = new_text_owned ? new_text_owned : "";

            SlackTS new_ts = slack_ts_new(new_ts_str);
            struct t_slack_message *msg = slack_message_search(channel->messages, new_ts);
            if (msg)
            {
                free(msg->text);
                msg->text = strdup(new_text);
                msg->is_edited = 1;
                slack_message_update(msg, message_obj);
            }
            if (channel->buffer)
            {
                char *rendered = slack_event_render_formatting(new_text);
                char *with_emoji = slack_event_apply_emoji_mode(
                    rendered ? rendered : new_text);
                edit_color = weechat_config_string(weeslack_config.color_edited_suffix);
                if (!edit_color || !edit_color[0])
                    edit_color = "yellow";
                weechat_printf_date_tags(
                    channel->buffer,
                    new_ts.sec,
                    "no_highlight,notify_none,slack_edited",
                    "%s(edited)%s\t%s %s(edited)%s",
                    weechat_color(edit_color),
                    weechat_color("reset"),
                    with_emoji ? with_emoji : new_text,
                    weechat_color(edit_color),
                    weechat_color("reset"));
                free(rendered);
                free(with_emoji);
            }
            free(new_text_owned);
        }
        return;
    }

    if (subtype && strcmp(subtype, "message_deleted") == 0)
    {
        struct json_object *deleted_ts_obj;
        free(text_owned);
        if (json_object_object_get_ex(msg_json, "deleted_ts", &deleted_ts_obj))
        {
            SlackTS deleted_ts = slack_ts_new(
                json_object_get_string(deleted_ts_obj));
            struct t_slack_message *msg = slack_message_search(
                channel->messages, deleted_ts);
            if (msg)
                msg->is_deleted = 1;
            if (channel->buffer)
            {
                const char *del_color = weechat_config_string(
                    weeslack_config.color_deleted);
                if (!del_color || !del_color[0])
                    del_color = "red";
                weechat_printf_date_tags(
                    channel->buffer,
                    deleted_ts.sec,
                    "no_highlight,notify_none,slack_deleted",
                    "%s(deleted)%s\t%smessage deleted%s",
                    weechat_color(del_color),
                    weechat_color("reset"),
                    weechat_color(del_color),
                    weechat_color("reset"));
            }
        }
        return;
    }

    if (subtype && (strcmp(subtype, "channel_topic") == 0 ||
                    strcmp(subtype, "channel_purpose") == 0))
    {
        struct json_object *topic_obj = NULL;
        const char *field = (strcmp(subtype, "channel_purpose") == 0)
            ? "purpose" : "topic";
        free(text_owned);
        if (json_object_object_get_ex(msg_json, field, &topic_obj))
        {
            const char *topic = json_object_get_string(topic_obj);
            if (strcmp(field, "purpose") == 0)
            {
                free(channel->purpose);
                channel->purpose = topic ? strdup(topic) : NULL;
            }
            else
            {
                free(channel->topic);
                channel->topic = topic ? strdup(topic) : NULL;
            }

            if (channel->buffer && topic)
            {
                if (strcmp(field, "topic") == 0)
                    weechat_buffer_set(channel->buffer, "title", topic);
                weechat_printf(channel->buffer,
                                "%s%s%s",
                                weechat_color("green"),
                                topic,
                                weechat_color("reset"));
            }
        }
        return;
    }

    if (subtype && strcmp(subtype, "channel_join") == 0)
    {
        if (channel->buffer && user_id)
        {
            struct t_slack_user *ju = slack_user_search(user_id);
            const char *jname = ju ? slack_user_best_name(ju) : user_id;
            weechat_printf(channel->buffer,
                            "%s--> %s has joined%s",
                            weechat_color("green"),
                            jname ? jname : user_id,
                            weechat_color("reset"));
        }
        free(text_owned);
        return;
    }

    if (subtype && strcmp(subtype, "channel_leave") == 0)
    {
        if (channel->buffer && user_id)
        {
            struct t_slack_user *lu = slack_user_search(user_id);
            const char *lname = lu ? slack_user_best_name(lu) : user_id;
            weechat_printf(channel->buffer,
                            "%s<-- %s has left%s",
                            weechat_color("red"),
                            lname ? lname : user_id,
                            weechat_color("reset"));
        }
        free(text_owned);
        return;
    }

    /* check for thread reply */
    struct json_object *thread_ts_obj;
    const char *thread_ts = NULL;
    if (json_object_object_get_ex(msg_json, "thread_ts", &thread_ts_obj))
        thread_ts = json_object_get_string(thread_ts_obj);

    struct t_slack_channel *display_channel = channel;

    /* thread replies have thread_ts but not reply_count */
    struct json_object *reply_count_obj;
    int is_parent = (thread_ts &&
                     json_object_object_get_ex(msg_json, "reply_count",
                                               &reply_count_obj));

    if (thread_ts && !is_parent)
    {
        int show_in_parent = weechat_config_boolean(
            weeslack_config.thread_messages_in_channel);
        struct t_slack_channel *thread = slack_thread_channel_find(channel, thread_ts);
        if (!thread)
        {
            char topic_buf[256];
            snprintf(topic_buf, sizeof(topic_buf), "Thread: %s", thread_ts);
            thread = slack_thread_channel_create(channel, thread_ts, topic_buf);
        }

        /*
         * Do not auto-open a buffer for every live reply — that storms
         * buffers + replies fetches. Open only when:
         *  - the thread buffer is already open, or
         *  - the user subscribed (/cslack subscribe), or
         *  - this is history for an already-open thread (handled by fetch_replies).
         * Explicit open: /cslack thread <ts>.
         */
        if (thread && !thread->buffer && thread->is_subscribed)
        {
            slack_buffer_new(workspace, thread);
            {
                struct t_slack_buffer *tb =
                    slack_buffer_search_by_channel(thread->id);
                if (tb)
                    weechat_buffer_set(tb->buffer, "notify", "highlight");
            }
            if (!history)
                slack_event_fetch_replies(workspace, thread);
        }

        /* Prefer open thread buffer when not broadcasting into parent */
        if (thread && thread->buffer && !show_in_parent)
            display_channel = thread;
        else
            display_channel = channel;
    }

    /* create and store the message */
    struct t_slack_message *msg = slack_message_new(ts, user_id, text);
    if (!msg)
    {
        free(text_owned);
        return;
    }

    slack_message_update(msg, msg_json);

    /* if this is a parent message with reply_count, show in parent channel */
    if (is_parent)
    {
        channel->messages = slack_message_prepend(channel->messages, msg);

        if (channel->buffer)
        {
            int rc = json_object_get_int(reply_count_obj);
            struct t_slack_user *user = user_id ? slack_user_search(user_id) : NULL;
            char *nick_str = slack_event_format_user(user, user_id, channel);
            char *formatted_text = slack_event_render_formatting(text);
            char *emoji_text = slack_event_apply_emoji_mode(formatted_text);
            char *formatted = slack_event_format_mentions(workspace, emoji_text, channel);
            char tags[256];
            char *ts_tag = slack_ts_to_string(ts);
            const char *thread_color = weechat_config_string(
                weeslack_config.color_thread_suffix);
            const char *suffix_color = "cyan";

            if (thread_color && thread_color[0] &&
                strcmp(thread_color, "multiple") == 0)
            {
                /* reuse nick color already applied in nick_str; use a fixed
                 * contrast default for the bracket text */
                if (user)
                {
                    const char *uc = slack_user_get_color(user);
                    if (uc && uc[0])
                        suffix_color = uc;
                }
            }
            else if (thread_color && thread_color[0])
                suffix_color = thread_color;

            snprintf(tags, sizeof(tags),
                     "%s,slack_ts_%s",
                     slack_event_notify_tags(workspace, channel, text, history),
                     ts_tag ? ts_tag : "0");

            weechat_printf_date_tags(
                channel->buffer,
                ts.sec,
                tags,
                "%s\t%s  %s[%d replies]%s",
                nick_str,
                formatted,
                weechat_color(suffix_color),
                rc,
                weechat_color("reset"));

            if (ts_tag)
            {
                free(channel->last_message_ts);
                channel->last_message_ts = strdup(ts_tag);
                weechat_buffer_set(channel->buffer,
                                   "localvar_set_slack_timestamp", ts_tag);
            }
            free(ts_tag);

            free(nick_str);
            free(formatted_text);
            free(emoji_text);
            free(formatted);
        }
        free(text_owned);
        return;
    }

    /* store in the display channel (may be thread channel) */
    display_channel->messages = slack_message_prepend(display_channel->messages, msg);

    /* display in buffer */
    if (display_channel->buffer)
    {
        struct t_slack_user *user = user_id ? slack_user_search(user_id) : NULL;
        char *nick_str = slack_event_format_user(user, user_id, display_channel);
        char *formatted_text = slack_event_render_formatting(text);
        char *emoji_text = slack_event_apply_emoji_mode(formatted_text);
        char *formatted = slack_event_format_mentions(workspace, emoji_text, channel);
        char *files = slack_event_format_files(msg_json);
        char *attachments = slack_event_format_attachments(msg_json);
        char tags[256];
        char *ts_tag = slack_ts_to_string(ts);
        const char *bcast = "";
        char bcast_buf[64];
        char me_buf[512];

        if (thread_ts && !is_parent &&
            display_channel == channel)
        {
            const char *pfx = weechat_config_string(
                weeslack_config.thread_broadcast_prefix);
            if (!pfx || !pfx[0])
                pfx = "thread";
            /* Prefix when showing a reply in the parent (either by config
             * or because no dedicated thread buffer is open). */
            snprintf(bcast_buf, sizeof(bcast_buf), " %s[%s]%s",
                     weechat_color("cyan"), pfx, weechat_color("reset"));
            bcast = bcast_buf;
        }

        /* /me style: * nick does something */
        if (is_me)
        {
            const char *plain_nick = user ? slack_user_best_name(user)
                : (user_id ? user_id : "?");
            snprintf(me_buf, sizeof(me_buf), "%s* %s %s%s",
                     weechat_color("magenta"),
                     plain_nick ? plain_nick : "?",
                     formatted ? formatted : "",
                     weechat_color("reset"));
        }

        snprintf(tags, sizeof(tags),
                 "%s,slack_ts_%s%s",
                 slack_event_notify_tags(workspace, display_channel, text,
                                         history),
                 ts_tag ? ts_tag : "0",
                 is_me ? ",slack_me" : "");

        if (is_me)
        {
            weechat_printf_date_tags(
                display_channel->buffer,
                ts.sec,
                tags,
                " \t%s%s",
                me_buf,
                bcast);
        }
        else
        {
            weechat_printf_date_tags(
                display_channel->buffer,
                ts.sec,
                tags,
                "%s\t%s%s",
                nick_str,
                formatted,
                bcast);
        }

        if (ts_tag)
        {
            free(display_channel->last_message_ts);
            display_channel->last_message_ts = strdup(ts_tag);
            weechat_buffer_set(display_channel->buffer,
                               "localvar_set_slack_timestamp", ts_tag);
            /* also update parent when displaying a reply there */
            if (display_channel != channel && channel)
            {
                free(channel->last_message_ts);
                channel->last_message_ts = strdup(ts_tag);
                if (channel->buffer)
                    weechat_buffer_set(channel->buffer,
                                       "localvar_set_slack_timestamp", ts_tag);
            }
        }
        free(ts_tag);

        if (files && files[0])
        {
            weechat_printf_date_tags(
                display_channel->buffer,
                ts.sec,
                NULL,
                "%s\t%s",
                "",
                files);
        }

        if (attachments && attachments[0])
        {
            weechat_printf_date_tags(
                display_channel->buffer,
                ts.sec,
                NULL,
                "%s\t%s",
                "",
                attachments);
        }

        free(nick_str);
        free(formatted_text);
        free(emoji_text);
        free(formatted);
        free(files);
        free(attachments);
    }
    free(text_owned);
}

void
slack_event_handle(struct t_weeslack_workspace *workspace,
                    struct json_object *json)
{
    struct json_object *type_obj;

    if (!workspace || !json)
        return;

    if (!json_object_object_get_ex(json, "type", &type_obj))
        return;

    const char *type = json_object_get_string(type_obj);

    if (strcmp(type, "hello") == 0)
    {
        workspace->connected = 1;
        SLACK_WS_PRINTF(workspace, "%sweeslack: connected to %s as %s",
                        weechat_prefix("network"),
                        workspace->name,
                        workspace->my_user_id ? workspace->my_user_id : "?");
        return;
    }

    if (strcmp(type, "message") == 0)
    {
        struct json_object *channel_obj;
        const char *channel_id = NULL;

        if (json_object_object_get_ex(json, "channel", &channel_obj))
            channel_id = json_object_get_string(channel_obj);

        struct t_slack_channel *channel = channel_id
            ? slack_channel_search(channel_id) : NULL;

        /* open buffer on first message if we missed it in conversations.list */
        if (!channel && channel_id)
        {
            channel = slack_channel_new(channel_id, channel_id,
                                        SLACK_CHANNEL_TYPE_CHANNEL);
            if (channel)
                channel->is_member = 1;
        }

        slack_event_process_message(workspace, channel, json, 0);
        return;
    }

    if (strcmp(type, "reaction_added") == 0 ||
        strcmp(type, "reaction_removed") == 0)
    {
        slack_event_handle_reaction(workspace, json);
        return;
    }

    if (strcmp(type, "user_typing") == 0)
    {
        struct json_object *channel_obj, *user_obj;
        const char *channel_id = NULL;
        const char *user_id = NULL;

        if (json_object_object_get_ex(json, "channel", &channel_obj))
            channel_id = json_object_get_string(channel_obj);
        if (json_object_object_get_ex(json, "user", &user_obj))
            user_id = json_object_get_string(user_obj);

        if (channel_id && user_id)
        {
            struct t_slack_channel *ch = slack_channel_search(channel_id);
            struct t_slack_user *u = slack_user_search(user_id);
            const char *name = u ? slack_user_best_name(u) : user_id;

            if (ch)
                slack_buffer_set_typing(ch, name);
        }
        return;
    }

    if (strcmp(type, "presence_change") == 0)
    {
        struct json_object *user_obj;
        if (json_object_object_get_ex(json, "user", &user_obj))
        {
            const char *user_id = json_object_get_string(user_obj);
            struct t_slack_user *user = slack_user_search(user_id);
            if (user)
            {
                struct json_object *presence_obj;
                if (json_object_object_get_ex(json, "presence", &presence_obj))
                {
                    free(user->presence);
                    user->presence = strdup(
                        json_object_get_string(presence_obj));
                    slack_buffer_update_user_presence(user);
                }
            }
        }
        return;
    }

    if (strcmp(type, "channel_marked") == 0 ||
        strcmp(type, "group_marked") == 0 ||
        strcmp(type, "im_marked") == 0 ||
        strcmp(type, "mpim_marked") == 0)
    {
        struct json_object *channel_obj, *ts_obj;
        const char *channel_id = NULL;
        const char *ts_str = NULL;

        if (json_object_object_get_ex(json, "channel", &channel_obj))
            channel_id = json_object_get_string(channel_obj);
        if (json_object_object_get_ex(json, "ts", &ts_obj))
            ts_str = json_object_get_string(ts_obj);

        if (channel_id)
        {
            struct t_slack_channel *ch = slack_channel_search(channel_id);
            if (ch)
            {
                if (ts_str)
                    ch->last_read = slack_ts_new(ts_str);
                ch->unread_count = 0;
                if (ch->buffer)
                    slack_buffer_clear_hotlist(ch->buffer);
            }
        }
        return;
    }

    if (strcmp(type, "subteam_created") == 0 ||
        strcmp(type, "subteam_updated") == 0)
    {
        struct json_object *subteam_obj;
        if (json_object_object_get_ex(json, "subteam", &subteam_obj))
        {
            struct json_object *id_obj;
            if (json_object_object_get_ex(subteam_obj, "id", &id_obj))
            {
                const char *subteam_id = json_object_get_string(id_obj);
                struct t_slack_subteam *st = slack_subteam_search(subteam_id);
                if (!st)
                    st = slack_subteam_new(subteam_id, NULL);

                if (st)
                {
                    struct json_object *name_obj;
                    if (json_object_object_get_ex(subteam_obj, "name", &name_obj))
                    {
                        free(st->name);
                        st->name = strdup(json_object_get_string(name_obj));
                    }
                    struct json_object *handle_obj;
                    if (json_object_object_get_ex(subteam_obj, "handle", &handle_obj))
                    {
                        free(st->handle);
                        st->handle = strdup(json_object_get_string(handle_obj));
                    }
                    struct json_object *desc_obj;
                    if (json_object_object_get_ex(subteam_obj, "description", &desc_obj))
                    {
                        free(st->description);
                        st->description = strdup(
                            json_object_get_string(desc_obj));
                    }
                }
            }
        }
        return;
    }

    if (strcmp(type, "file_created") == 0 ||
        strcmp(type, "file_shared") == 0)
    {
        struct json_object *file_obj;
        if (json_object_object_get_ex(json, "file", &file_obj))
        {
            struct json_object *name_obj, *type_obj2;
            const char *file_name = NULL, *file_type = NULL;

            if (json_object_object_get_ex(file_obj, "name", &name_obj))
                file_name = json_object_get_string(name_obj);
            if (json_object_object_get_ex(file_obj, "filetype", &type_obj2))
                file_type = json_object_get_string(type_obj2);

            SLACK_WS_PRINTF(workspace, "%sweeslack: %s file: %s (%s)",
                            weechat_prefix("network"),
                            strcmp(type, "file_created") == 0
                                ? "created" : "shared",
                            file_name ? file_name : "?",
                            file_type ? file_type : "?");

            struct json_object *channel_obj;
            if (json_object_object_get_ex(json, "channel_id", &channel_obj))
            {
                const char *ch_id = json_object_get_string(channel_obj);
                struct t_slack_channel *ch = slack_channel_search(ch_id);
                if (ch && ch->buffer)
                {
                    char msg_buf[512];
                    snprintf(msg_buf, sizeof(msg_buf), "[file: %s]",
                             file_name ? file_name : "?");
                    weechat_printf(ch->buffer, "%s%s%s",
                                    weechat_prefix("action"),
                                    msg_buf,
                                    weechat_color("reset"));
                }
            }
        }
        return;
    }

    if (strcmp(type, "emoji_changed") == 0)
    {
        /* refresh custom emoji map */
        slack_event_fetch_emoji(workspace);
        return;
    }

    /* member_joined_channel / member_left_channel (not message subtypes) */
    if (strcmp(type, "member_joined_channel") == 0 ||
        strcmp(type, "member_left_channel") == 0)
    {
        struct json_object *channel_obj, *user_obj;
        const char *channel_id = NULL;
        const char *user_id = NULL;
        int joined = (strcmp(type, "member_joined_channel") == 0);

        if (json_object_object_get_ex(json, "channel", &channel_obj))
            channel_id = json_object_get_string(channel_obj);
        if (json_object_object_get_ex(json, "user", &user_obj))
            user_id = json_object_get_string(user_obj);

        if (channel_id && user_id)
        {
            struct t_slack_channel *ch = slack_channel_search(channel_id);
            struct t_slack_user *u = slack_user_search(user_id);
            const char *name = u ? slack_user_best_name(u) : user_id;
            struct t_slack_buffer *sbuf;

            if (ch && ch->buffer)
            {
                weechat_printf(ch->buffer,
                                "%s%s %s has %s%s",
                                weechat_color(joined ? "green" : "red"),
                                joined ? "-->" : "<--",
                                name ? name : user_id,
                                joined ? "joined" : "left",
                                weechat_color("reset"));
            }

            sbuf = channel_id ? slack_buffer_search_by_channel(channel_id)
                              : NULL;
            if (sbuf)
            {
                if (!u)
                    u = slack_user_new(user_id, user_id, NULL);
                if (joined && u)
                    slack_buffer_add_nick(sbuf, u);
                else if (!joined && u)
                    slack_buffer_remove_nick(sbuf, u);
            }
        }
        return;
    }

    if (strcmp(type, "user_change") == 0)
    {
        struct json_object *user_obj;
        if (json_object_object_get_ex(json, "user", &user_obj))
        {
            struct json_object *id_obj;
            const char *uid = NULL;
            if (json_object_object_get_ex(user_obj, "id", &id_obj))
                uid = json_object_get_string(id_obj);
            if (uid && uid[0])
            {
                struct t_slack_user *user = slack_user_search(uid);
                if (!user)
                {
                    struct json_object *name_obj;
                    const char *uname = uid;
                    if (json_object_object_get_ex(user_obj, "name", &name_obj))
                        uname = json_object_get_string(name_obj);
                    user = slack_user_new(uid, uname && uname[0] ? uname : uid,
                                          NULL);
                }
                if (user)
                {
                    slack_user_update(user, user_obj);
                    slack_buffer_update_user_presence(user);
                }
            }
        }
        return;
    }

    if (strcmp(type, "channel_rename") == 0 ||
        strcmp(type, "group_rename") == 0)
    {
        struct json_object *channel_obj;
        if (json_object_object_get_ex(json, "channel", &channel_obj))
        {
            struct json_object *id_obj, *name_obj;
            const char *cid = NULL, *cname = NULL;
            if (json_object_object_get_ex(channel_obj, "id", &id_obj))
                cid = json_object_get_string(id_obj);
            if (json_object_object_get_ex(channel_obj, "name", &name_obj))
                cname = json_object_get_string(name_obj);
            if (cid && cname && cname[0])
            {
                struct t_slack_channel *ch = slack_channel_search(cid);
                if (ch)
                {
                    free(ch->name);
                    ch->name = strdup(cname);
                    if (ch->buffer)
                    {
                        char short_name[128];
                        const char *team = workspace->name
                            ? workspace->name : workspace->id;
                        int use_short = weechat_config_boolean(
                            weeslack_config.short_buffer_names);
                        if (use_short)
                            snprintf(short_name, sizeof(short_name),
                                     "#%s", cname);
                        else
                            snprintf(short_name, sizeof(short_name),
                                     "%s.#%s",
                                     team ? team : "slack", cname);
                        weechat_buffer_set(ch->buffer, "short_name",
                                           short_name);
                        weechat_buffer_set(ch->buffer,
                                           "localvar_set_channel", cname);
                        weechat_printf(ch->buffer,
                                        "%schannel renamed to #%s%s",
                                        weechat_prefix("network"),
                                        cname,
                                        weechat_color("reset"));
                    }
                }
            }
        }
        return;
    }

    if (strcmp(type, "channel_archive") == 0 ||
        strcmp(type, "group_archive") == 0 ||
        strcmp(type, "channel_unarchive") == 0 ||
        strcmp(type, "group_unarchive") == 0)
    {
        struct json_object *channel_obj;
        const char *channel_id = NULL;
        int archived = (strstr(type, "unarchive") == NULL);

        if (json_object_object_get_ex(json, "channel", &channel_obj))
            channel_id = json_object_get_string(channel_obj);
        if (channel_id)
        {
            struct t_slack_channel *ch = slack_channel_search(channel_id);
            if (ch && ch->buffer)
            {
                weechat_printf(ch->buffer,
                                "%schannel %s%s",
                                weechat_prefix("network"),
                                archived ? "archived" : "unarchived",
                                weechat_color("reset"));
                if (archived)
                    weechat_buffer_set(ch->buffer, "notify", "none");
            }
        }
        return;
    }

    /* New channel / group / IM created while connected */
    if (strcmp(type, "channel_created") == 0 ||
        strcmp(type, "group_joined") == 0 ||
        strcmp(type, "im_created") == 0 ||
        strcmp(type, "mpim_joined") == 0)
    {
        struct json_object *channel_obj = NULL;
        enum slack_channel_type ctype = SLACK_CHANNEL_TYPE_CHANNEL;
        const char *cid = NULL, *cname = NULL;

        if (strcmp(type, "im_created") == 0)
            ctype = SLACK_CHANNEL_TYPE_DM;
        else if (strcmp(type, "mpim_joined") == 0)
            ctype = SLACK_CHANNEL_TYPE_MPDM;
        else if (strcmp(type, "group_joined") == 0)
            ctype = SLACK_CHANNEL_TYPE_GROUP;

        if (json_object_object_get_ex(json, "channel", &channel_obj))
        {
            struct json_object *id_obj, *name_obj, *user_obj;
            if (json_object_object_get_ex(channel_obj, "id", &id_obj))
                cid = json_object_get_string(id_obj);
            if (json_object_object_get_ex(channel_obj, "name", &name_obj))
                cname = json_object_get_string(name_obj);
            if (cid)
            {
                struct t_slack_channel *ch = slack_channel_search(cid);
                if (!ch)
                {
                    ch = slack_channel_new(cid,
                                           cname && cname[0] ? cname : cid,
                                           ctype);
                    if (ch)
                    {
                        slack_channel_update(ch, channel_obj);
                        if (ctype == SLACK_CHANNEL_TYPE_DM &&
                            json_object_object_get_ex(channel_obj, "user",
                                                      &user_obj))
                        {
                            free(ch->user_id);
                            ch->user_id = strdup(
                                json_object_get_string(user_obj));
                        }
                        ch->is_member = 1;
                        if (!ch->buffer)
                            slack_buffer_new(workspace, ch);
                        SLACK_WS_PRINTF(
                            workspace,
                            "%sweeslack: joined %s",
                            weechat_prefix("network"),
                            ch->name ? ch->name : cid);
                    }
                }
            }
        }
        return;
    }

    if (strcmp(type, "team_join") == 0)
    {
        struct json_object *user_obj;
        if (json_object_object_get_ex(json, "user", &user_obj))
        {
            struct json_object *id_obj, *name_obj;
            const char *uid = NULL, *uname = NULL;
            if (json_object_object_get_ex(user_obj, "id", &id_obj))
                uid = json_object_get_string(id_obj);
            if (json_object_object_get_ex(user_obj, "name", &name_obj))
                uname = json_object_get_string(name_obj);
            if (uid)
            {
                struct t_slack_user *u = slack_user_search(uid);
                if (!u)
                    u = slack_user_new(uid, uname && uname[0] ? uname : uid,
                                      NULL);
                if (u)
                    slack_user_update(u, user_obj);
                SLACK_WS_PRINTF(workspace,
                                "%sweeslack: %s joined the team",
                                weechat_prefix("network"),
                                u ? slack_user_best_name(u) : uid);
            }
        }
        return;
    }

    if (strcmp(type, "pin_added") == 0 || strcmp(type, "pin_removed") == 0)
    {
        struct json_object *channel_obj, *item_obj, *user_obj;
        const char *channel_id = NULL;
        const char *user_id = NULL;
        const char *item_ts = NULL;
        int added = (strcmp(type, "pin_added") == 0);

        if (json_object_object_get_ex(json, "channel_id", &channel_obj))
            channel_id = json_object_get_string(channel_obj);
        if (!channel_id &&
            json_object_object_get_ex(json, "channel", &channel_obj))
            channel_id = json_object_get_string(channel_obj);
        if (json_object_object_get_ex(json, "user", &user_obj))
            user_id = json_object_get_string(user_obj);
        if (json_object_object_get_ex(json, "item", &item_obj))
        {
            struct json_object *msg_obj, *ts_obj;
            if (json_object_object_get_ex(item_obj, "message", &msg_obj) &&
                json_object_object_get_ex(msg_obj, "ts", &ts_obj))
                item_ts = json_object_get_string(ts_obj);
            else if (json_object_object_get_ex(item_obj, "ts", &ts_obj))
                item_ts = json_object_get_string(ts_obj);
        }

        if (channel_id)
        {
            struct t_slack_channel *ch = slack_channel_search(channel_id);
            struct t_slack_user *u = user_id ? slack_user_search(user_id) : NULL;
            const char *who = u ? slack_user_best_name(u)
                : (user_id ? user_id : "?");
            if (ch && ch->buffer)
            {
                weechat_printf(ch->buffer,
                                "%s%s pin by %s%s%s",
                                weechat_prefix("network"),
                                added ? "pinned" : "unpinned",
                                who,
                                item_ts ? " ts=" : "",
                                item_ts ? item_ts : "");
            }
        }
        return;
    }

    if (strcmp(type, "channel_left") == 0 ||
        strcmp(type, "group_left") == 0 ||
        strcmp(type, "im_close") == 0 ||
        strcmp(type, "mpim_close") == 0)
    {
        struct json_object *channel_obj;
        const char *channel_id = NULL;

        if (json_object_object_get_ex(json, "channel", &channel_obj))
            channel_id = json_object_get_string(channel_obj);
        if (channel_id)
        {
            struct t_slack_channel *ch = slack_channel_search(channel_id);
            if (ch)
            {
                ch->is_member = 0;
                if (ch->buffer)
                {
                    weechat_printf(ch->buffer,
                                    "%sleft this conversation%s",
                                    weechat_prefix("network"),
                                    weechat_color("reset"));
                    weechat_buffer_set(ch->buffer, "notify", "none");
                }
            }
        }
        return;
    }

    if (strcmp(type, "channel_deleted") == 0 ||
        strcmp(type, "group_deleted") == 0)
    {
        struct json_object *channel_obj;
        const char *channel_id = NULL;

        if (json_object_object_get_ex(json, "channel", &channel_obj))
            channel_id = json_object_get_string(channel_obj);
        if (channel_id)
        {
            struct t_slack_channel *ch = slack_channel_search(channel_id);
            if (ch && ch->buffer)
            {
                weechat_printf(ch->buffer,
                                "%schannel deleted on Slack%s",
                                weechat_prefix("error"),
                                weechat_color("reset"));
                weechat_buffer_set(ch->buffer, "notify", "none");
            }
        }
        return;
    }

    if (strcmp(type, "dnd_updated") == 0 ||
        strcmp(type, "dnd_updated_user") == 0)
    {
        struct json_object *user_obj, *dnd_obj;
        const char *user_id = NULL;
        int dnd = 0;

        if (json_object_object_get_ex(json, "user", &user_obj))
            user_id = json_object_get_string(user_obj);
        if (json_object_object_get_ex(json, "dnd_status", &dnd_obj))
        {
            struct json_object *en;
            if (json_object_object_get_ex(dnd_obj, "dnd_enabled", &en))
                dnd = json_object_get_boolean(en);
        }
        if (user_id && workspace->my_user_id &&
            strcmp(user_id, workspace->my_user_id) == 0)
        {
            SLACK_WS_PRINTF(workspace,
                            "%sweeslack: Do Not Disturb %s",
                            weechat_prefix("network"),
                            dnd ? "enabled" : "disabled");
        }
        return;
    }
}

void
slack_event_handle_message(struct t_weeslack_workspace *workspace,
                            struct t_slack_channel *channel,
                            struct json_object *json,
                            int history)
{
    if (!workspace || !json)
        return;

    slack_event_process_message(workspace, channel, json, history);
}

void
slack_event_handle_reaction(struct t_weeslack_workspace *workspace,
                             struct json_object *json)
{
    (void) workspace;

    struct json_object *item_obj;
    if (!json_object_object_get_ex(json, "item", &item_obj))
        return;

    struct json_object *channel_obj, *ts_obj;
    const char *channel_id = NULL;
    const char *ts_str = NULL;

    if (json_object_object_get_ex(item_obj, "channel", &channel_obj))
        channel_id = json_object_get_string(channel_obj);

    if (json_object_object_get_ex(item_obj, "ts", &ts_obj))
        ts_str = json_object_get_string(ts_obj);

    if (!channel_id || !ts_str)
        return;

    struct t_slack_channel *channel = slack_channel_search(channel_id);
    if (!channel || !channel->buffer)
        return;

    SlackTS ts = slack_ts_new(ts_str);
    struct t_slack_message *msg = slack_message_search(channel->messages, ts);

    struct json_object *name_obj, *user_obj;
    const char *reaction_name = "";
    const char *reaction_user_id = "";
    const char *who = "?";
    int removed = 0;

    if (json_object_object_get_ex(json, "reaction", &name_obj))
        reaction_name = json_object_get_string(name_obj);

    if (json_object_object_get_ex(json, "user", &user_obj))
        reaction_user_id = json_object_get_string(user_obj);

    if (reaction_user_id && reaction_user_id[0])
    {
        struct t_slack_user *u = slack_user_search(reaction_user_id);
        who = u ? slack_user_best_name(u) : reaction_user_id;
    }

    struct json_object *type_obj;
    const char *type_str = "";
    if (json_object_object_get_ex(json, "type", &type_obj))
        type_str = json_object_get_string(type_obj);

    removed = (type_str && strcmp(type_str, "reaction_removed") == 0);

    /* Maintain reaction list on the message model (item is not a message JSON). */
    if (msg && reaction_name && reaction_name[0] &&
        reaction_user_id && reaction_user_id[0])
    {
        if (removed)
            slack_message_reaction_remove(msg, reaction_name, reaction_user_id);
        else
            slack_message_reaction_add(msg, reaction_name, reaction_user_id);
    }

    weechat_printf_date_tags(
        channel->buffer,
        ts.sec,
        "no_highlight,notify_none,slack_reaction",
        "%s%s:%s: by %s%s",
        weechat_color("blue"),
        removed ? "-" : "+",
        reaction_name,
        who,
        weechat_color("reset"));
}

/* ============================================================
 * Markdown / Slack formatting
 * ============================================================ */

/* Capacity-tracked append for render_formatting (no sprintf). */
static int
slack_fmt_append(char *dst, size_t *pos, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t rem;

    if (!dst || !pos || *pos >= cap)
        return -1;
    rem = cap - *pos;
    va_start(ap, fmt);
    n = vsnprintf(dst + *pos, rem, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n >= rem)
    {
        *pos = cap - 1;
        return -1;
    }
    *pos += (size_t)n;
    return 0;
}

static char *
slack_event_render_formatting(const char *text)
{
    size_t len, cap, pos;
    char *result;
    const char *src;

    if (!text)
        return strdup("");

    len = strlen(text);
    /* color codes expand the string; leave headroom */
    cap = len * 6 + 64;
    result = malloc(cap);
    if (!result)
        return strdup(text);

    pos = 0;
    result[0] = '\0';
    src = text;

    while (*src && pos + 8 < cap)
    {
        /* code block ```...``` */
        if (src[0] == '`' && src[1] == '`' && src[2] == '`')
        {
            const char *end = strstr(src + 3, "```");
            if (end)
            {
                slack_fmt_append(result, &pos, cap, "%s`%.*s`%s",
                                 weechat_color("green"),
                                 (int)(end - (src + 3)), src + 3,
                                 weechat_color("reset"));
                src = end + 3;
                continue;
            }
        }

        /* inline code `...` */
        if (src[0] == '`' && src[1] != '`')
        {
            const char *end = strchr(src + 1, '`');
            if (end)
            {
                slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                 weechat_color("green"),
                                 (int)(end - (src + 1)), src + 1,
                                 weechat_color("reset"));
                src = end + 1;
                continue;
            }
        }

        /* strikethrough ~...~ */
        if (src[0] == '~')
        {
            const char *end = strchr(src + 1, '~');
            if (end)
            {
                const char *style = weechat_config_string(
                    weeslack_config.render_strikethrough_as);
                /* "weechat" / empty → red; "irc" → lightred (common IRC map);
                 * any other value is treated as a WeeChat color/attr name. */
                if (!style || !style[0] || strcmp(style, "weechat") == 0)
                    style = "red";
                else if (strcmp(style, "irc") == 0)
                    style = "lightred";
                slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                 weechat_color(style),
                                 (int)(end - (src + 1)), src + 1,
                                 weechat_color("reset"));
                src = end + 1;
                continue;
            }
        }

        /* bold *...*  (but not **, not inside words) */
        if (src[0] == '*' && src[1] != '*' && src[1] != ' ')
        {
            const char *end = strchr(src + 1, '*');
            if (end && end > src + 1)
            {
                const char *style = weechat_config_string(
                    weeslack_config.render_bold_as);
                if (!style || !style[0] || strcmp(style, "bold") == 0)
                    style = "bold";
                slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                 weechat_color(style),
                                 (int)(end - (src + 1)), src + 1,
                                 weechat_color("reset"));
                src = end + 1;
                continue;
            }
        }

        /* italic _word_ only when delimiters sit on word boundaries
         * (avoids breaking :slightly_smiling_face: shortcodes) */
        if (src[0] == '_' && src[1] != '_' && src[1] != ' ' &&
            (src == text || src[-1] == ' ' || src[-1] == '\t' || src[-1] == '\n'))
        {
            const char *end = strchr(src + 1, '_');
            if (end && end > src + 1 &&
                (end[1] == '\0' || end[1] == ' ' || end[1] == '\t' ||
                 end[1] == '\n' || end[1] == ',' || end[1] == '.'))
            {
                const char *style = weechat_config_string(
                    weeslack_config.render_italic_as);
                if (!style || !style[0] || strcmp(style, "italic") == 0)
                    style = "italic";
                slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                 weechat_color(style),
                                 (int)(end - (src + 1)), src + 1,
                                 weechat_color("reset"));
                src = end + 1;
                continue;
            }
        }

        /* channel link <#C1234|channel-name> */
        if (src[0] == '<' && src[1] == '#')
        {
            const char *end = strchr(src + 2, '>');
            if (end)
            {
                const char *pipe = memchr(src + 2, '|', end - (src + 2));
                if (pipe)
                {
                    slack_fmt_append(result, &pos, cap, "%s#%.*s%s",
                                     weechat_color("cyan"),
                                     (int)(end - pipe - 1), pipe + 1,
                                     weechat_color("reset"));
                }
                else
                {
                    slack_fmt_append(result, &pos, cap, "%s#%.*s%s",
                                     weechat_color("cyan"),
                                     (int)(end - src - 2), src + 2,
                                     weechat_color("reset"));
                }
                src = end + 1;
                continue;
            }
        }

        /* URL <http...> or <http...|text> */
        if (src[0] == '<')
        {
            const char *end = strchr(src + 1, '>');
            if (end && (src[1] == 'h' || src[1] == 'm'))
            {
                const char *pipe = memchr(src + 1, '|', end - (src + 1));
                if (pipe)
                {
                    /* display the label, not the URL */
                    slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                     weechat_color("cyan"),
                                     (int)(end - pipe - 1), pipe + 1,
                                     weechat_color("reset"));
                }
                else
                {
                    slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                     weechat_color("cyan"),
                                     (int)(end - src - 1), src + 1,
                                     weechat_color("reset"));
                }
                src = end + 1;
                continue;
            }
        }

        result[pos++] = *src++;
        result[pos] = '\0';
    }
    result[pos] = '\0';

    return result;
}

/* ============================================================
 * File / attachment display
 * ============================================================ */

static void
slack_format_bytes(char *buf, size_t buf_size, long size)
{
    if (size < 0)
        size = 0;
    if (size < 1024)
        snprintf(buf, buf_size, "%ld B", size);
    else if (size < 1024 * 1024)
        snprintf(buf, buf_size, "%.1f KB", size / 1024.0);
    else if (size < 1024L * 1024 * 1024)
        snprintf(buf, buf_size, "%.1f MB", size / (1024.0 * 1024.0));
    else
        snprintf(buf, buf_size, "%.1f GB", size / (1024.0 * 1024.0 * 1024.0));
}

static char *
slack_event_format_files(struct json_object *msg_json)
{
    struct json_object *files_obj;
    int shown = 0;

    if (!json_object_object_get_ex(msg_json, "files", &files_obj))
        return NULL;

    int count = json_object_array_length(files_obj);
    if (count == 0)
        return NULL;

    /* estimate buffer size (title + url + metadata) */
    size_t buf_size = (size_t)count * 384 + 64;
    char *result = malloc(buf_size);
    if (!result)
        return NULL;

    result[0] = '\0';
    size_t pos = 0;

    for (int i = 0; i < count; i++)
    {
        struct json_object *f = json_object_array_get_idx(files_obj, i);
        struct json_object *mode_obj, *obj;
        const char *title = NULL;
        const char *name = NULL;
        const char *url = NULL;
        const char *filetype = "";
        const char *mimetype = "";
        long size = 0;
        char size_buf[32];
        const char *label;
        int written;

        if (json_object_object_get_ex(f, "mode", &mode_obj))
        {
            if (strcmp(json_object_get_string(mode_obj), "tombstone") == 0)
                continue;
        }

        if (json_object_object_get_ex(f, "title", &obj))
            title = json_object_get_string(obj);
        if (json_object_object_get_ex(f, "name", &obj))
            name = json_object_get_string(obj);
        /* prefer downloadable private URL when present */
        if (json_object_object_get_ex(f, "url_private_download", &obj))
            url = json_object_get_string(obj);
        if ((!url || !url[0]) &&
            json_object_object_get_ex(f, "url_private", &obj))
            url = json_object_get_string(obj);
        if ((!url || !url[0]) &&
            json_object_object_get_ex(f, "permalink", &obj))
            url = json_object_get_string(obj);
        if (json_object_object_get_ex(f, "filetype", &obj))
            filetype = json_object_get_string(obj);
        if (json_object_object_get_ex(f, "mimetype", &obj))
            mimetype = json_object_get_string(obj);
        if (json_object_object_get_ex(f, "size", &obj))
            size = json_object_get_int64(obj);

        label = (title && title[0]) ? title
            : ((name && name[0]) ? name : "file");
        slack_format_bytes(size_buf, sizeof(size_buf), size);

        if (pos >= buf_size - 1)
            break;

        written = snprintf(
            result + pos, buf_size - pos,
            "%s%s[file]%s %s (%s%s%s%s%s) %s%s",
            shown > 0 ? " | " : "",
            weechat_color("blue"),
            weechat_color("reset"),
            label,
            filetype && filetype[0] ? filetype : "bin",
            (mimetype && mimetype[0]) ? ", " : "",
            (mimetype && mimetype[0]) ? mimetype : "",
            size > 0 ? ", " : "",
            size > 0 ? size_buf : "",
            (url && url[0]) ? url : "",
            weechat_color("reset"));
        if (written > 0)
            pos += (size_t)written < buf_size - pos
                ? (size_t)written : (buf_size - pos - 1);
        shown++;
    }

    if (shown == 0)
    {
        free(result);
        return NULL;
    }
    return result;
}

static char *
slack_event_format_attachments(struct json_object *msg_json)
{
    struct json_object *att_obj;
    if (!json_object_object_get_ex(msg_json, "attachments", &att_obj))
        return NULL;

    int count = json_object_array_length(att_obj);
    if (count == 0)
        return NULL;

    size_t buf_size = count * 512;
    char *result = malloc(buf_size);
    if (!result)
        return NULL;

    result[0] = '\0';
    size_t pos = 0;

    for (int i = 0; i < count; i++)
    {
        struct json_object *att = json_object_array_get_idx(att_obj, i);

        const char *title = "";
        const char *title_link = "";
        const char *text = "";
        const char *pretext = "";
        const char *author = "";

        struct json_object *obj;
        if (json_object_object_get_ex(att, "title", &obj))
            title = json_object_get_string(obj);
        if (json_object_object_get_ex(att, "title_link", &obj))
            title_link = json_object_get_string(obj);
        if (json_object_object_get_ex(att, "text", &obj))
            text = json_object_get_string(obj);
        if (json_object_object_get_ex(att, "pretext", &obj))
            pretext = json_object_get_string(obj);
        if (json_object_object_get_ex(att, "author_name", &obj))
            author = json_object_get_string(obj);

        if (pos < buf_size - 1)
        {
            int written = 0;
            if (pretext[0])
                written = snprintf(result + pos, buf_size - pos,
                                   "%s%s%s",
                                   i > 0 ? "\n" : "",
                                   pretext, weechat_color("reset"));
            else if (title[0] && title_link[0])
                written = snprintf(result + pos, buf_size - pos,
                                   "%s%s%s%s: %s%s",
                                   i > 0 ? "\n" : "",
                                   author[0] ? author : "",
                                   author[0] ? ": " : "",
                                   title, title_link,
                                   weechat_color("reset"));
            else if (title[0])
                written = snprintf(result + pos, buf_size - pos,
                                   "%s%s%s%s",
                                   i > 0 ? "\n" : "",
                                   author[0] ? author : "",
                                   author[0] ? ": " : "",
                                   title);
            else if (text[0])
                written = snprintf(result + pos, buf_size - pos,
                                   "%s%s",
                                   i > 0 ? "\n" : "", text);

            if (written > 0)
                pos += (size_t)written < buf_size - pos ? (size_t)written : 0;
        }
    }

    return result;
}

/* ============================================================
 * Mention highlighting
 * ============================================================ */

char *
slack_event_format_mentions(struct t_weeslack_workspace *workspace,
                             const char *text,
                             struct t_slack_channel *channel)
{
    (void) workspace;
    (void) channel;

    if (!text)
        return strdup("");

    /* Highlight <@U…>, <!here>, <!channel>, <!everyone>, <!subteam^S…|name>,
     * and plain @here/@channel/@everyone text. */
    size_t len = strlen(text);
    char *result = malloc(len * 4 + 64);
    if (!result)
        return strdup(text);

    const char *src = text;
    char *dst = result;
    char *dst_end = result + len * 4 + 63;

    while (*src && dst + 32 < dst_end)
    {
        if (src[0] == '<' && src[1] == '@')
        {
            /* user mention like <@U12345> or <@U12345|label> */
            const char *end = strchr(src + 2, '>');
            if (end)
            {
                size_t id_len = end - (src + 2);
                char user_id[64];
                const char *bar;
                if (id_len < sizeof(user_id))
                {
                    memcpy(user_id, src + 2, id_len);
                    user_id[id_len] = '\0';
                    bar = strchr(user_id, '|');
                    if (bar)
                        *((char *)bar) = '\0';

                    struct t_slack_user *user = slack_user_search(user_id);
                    const char *name = user
                        ? (user->display_name && user->display_name[0]
                           ? user->display_name : user->name)
                        : (bar ? bar + 1 : user_id);
                    const char *color = user ? slack_user_get_color(user) : "default";

                    dst += snprintf(dst, (size_t)(dst_end - dst), "%s@%s%s",
                                    weechat_color(color),
                                    name,
                                    weechat_color("reset"));
                    src = end + 1;
                    continue;
                }
            }
        }

        /* Slack special mentions: <!here> <!channel> <!everyone>
         * and <!subteam^S123|handle> */
        if (src[0] == '<' && src[1] == '!')
        {
            const char *end = strchr(src + 2, '>');
            if (end)
            {
                const char *body = src + 2;
                size_t blen = (size_t)(end - body);
                const char *label = "mention";
                char label_buf[128];

                if (blen == 4 && memcmp(body, "here", 4) == 0)
                    label = "here";
                else if (blen == 7 && memcmp(body, "channel", 7) == 0)
                    label = "channel";
                else if (blen == 8 && memcmp(body, "everyone", 8) == 0)
                    label = "everyone";
                else if (blen > 8 && memcmp(body, "subteam^", 8) == 0)
                {
                    const char *bar = memchr(body, '|', blen);
                    const char *sid = body + 8;
                    size_t sid_len;
                    char sub_id[64];
                    struct t_slack_subteam *st;

                    if (bar)
                    {
                        size_t l_len = (size_t)(end - (bar + 1));
                        if (l_len >= sizeof(label_buf))
                            l_len = sizeof(label_buf) - 1;
                        memcpy(label_buf, bar + 1, l_len);
                        label_buf[l_len] = '\0';
                        label = label_buf;
                        sid_len = (size_t)(bar - sid);
                    }
                    else
                    {
                        sid_len = blen - 8;
                        label = label_buf;
                        label_buf[0] = '\0';
                    }
                    if (sid_len >= sizeof(sub_id))
                        sid_len = sizeof(sub_id) - 1;
                    memcpy(sub_id, sid, sid_len);
                    sub_id[sid_len] = '\0';
                    st = slack_subteam_search(sub_id);
                    if (st && st->name && st->name[0] && !label_buf[0])
                        label = st->name;
                    else if (!label[0])
                        label = sub_id;
                }
                else
                {
                    /* unknown special — strip brackets */
                    size_t copy = blen < sizeof(label_buf) - 1 ? blen : sizeof(label_buf) - 1;
                    memcpy(label_buf, body, copy);
                    label_buf[copy] = '\0';
                    label = label_buf;
                }

                dst += snprintf(dst, (size_t)(dst_end - dst), "%s@%s%s",
                                weechat_color("yellow"),
                                label,
                                weechat_color("reset"));
                src = end + 1;
                continue;
            }
        }

        /* @here / @channel / @everyone */
        if (src[0] == '@')
        {
            size_t remaining = strlen(src);
            if (remaining >= 5 && memcmp(src, "@here", 5) == 0 &&
                (src[5] == '\0' || src[5] == ' ' || src[5] == ','))
            {
                dst += snprintf(dst, (size_t)(dst_end - dst), "%s@here%s",
                                weechat_color("yellow"),
                                weechat_color("reset"));
                src += 5;
                continue;
            }
            if (remaining >= 8 && memcmp(src, "@channel", 8) == 0 &&
                (src[8] == '\0' || src[8] == ' ' || src[8] == ','))
            {
                dst += snprintf(dst, (size_t)(dst_end - dst), "%s@channel%s",
                                weechat_color("yellow"),
                                weechat_color("reset"));
                src += 8;
                continue;
            }
            if (remaining >= 9 && memcmp(src, "@everyone", 9) == 0 &&
                (src[9] == '\0' || src[9] == ' ' || src[9] == ','))
            {
                dst += snprintf(dst, (size_t)(dst_end - dst), "%s@everyone%s",
                                weechat_color("yellow"),
                                weechat_color("reset"));
                src += 9;
                continue;
            }
        }

        /* emoji shortcodes :name: */
        if (src[0] == ':')
        {
            const char *end = strchr(src + 1, ':');
            if (end && (end - src) < 32)
            {
                /* skip emoji shortcodes for now (just display raw) */
                size_t n = (size_t)(end - src + 1);
                if (dst + n >= dst_end)
                    break;
                memcpy(dst, src, n);
                dst += n;
                src = end + 1;
                continue;
            }
        }

        *dst++ = *src++;
    }
    *dst = '\0';

    return result;
}

/* ============================================================
 * History fetch — uses global HTTP slow queue (wee-slack model)
 * ============================================================ */

/* After mass buffer create, ignore buffer_switch history storms. */
static time_t g_bootstrap_quiet_until = 0;

void
slack_event_bootstrap_quiet(int seconds)
{
    time_t until = time(NULL) + (seconds > 0 ? seconds : 5);
    if (until > g_bootstrap_quiet_until)
        g_bootstrap_quiet_until = until;
}

int
slack_event_in_bootstrap_quiet(void)
{
    return time(NULL) < g_bootstrap_quiet_until;
}

/* History: up to HISTORY_MAX_PAGES pages of HISTORY_PAGE_SIZE on the slow
 * queue. Messages are accumulated then flushed oldest→newest so multi-page
 * loads stay in chronological buffer order without stampeding the API. */
#define SLACK_HISTORY_PAGE_SIZE  100
#define SLACK_HISTORY_MAX_PAGES    5
#define SLACK_MEMBERS_PAGE_SIZE  200
#define SLACK_MEMBERS_MAX_PAGES    3

struct t_slack_history_ctx
{
    struct t_slack_channel *channel;
    int is_replies;
    int page;
    int truncated;
    /* retained message objects (json_object_get) until flush */
    struct json_object **msgs;
    int msg_count;
    int msg_cap;
    char parent_id[64];
    char thread_ts[64];
};

static void
slack_event_history_ctx_free(struct t_slack_history_ctx *ctx)
{
    int i;

    if (!ctx)
        return;
    if (ctx->msgs)
    {
        for (i = 0; i < ctx->msg_count; i++)
        {
            if (ctx->msgs[i])
                json_object_put(ctx->msgs[i]);
        }
        free(ctx->msgs);
    }
    free(ctx);
}

static int
slack_event_history_msg_ts_cmp(const void *a, const void *b)
{
    struct json_object *ja = *(struct json_object * const *)a;
    struct json_object *jb = *(struct json_object * const *)b;
    struct json_object *ta, *tb;
    const char *sa = "", *sb = "";
    SlackTS tsa, tsb;

    if (json_object_object_get_ex(ja, "ts", &ta))
        sa = json_object_get_string(ta);
    if (json_object_object_get_ex(jb, "ts", &tb))
        sb = json_object_get_string(tb);
    tsa = slack_ts_new(sa);
    tsb = slack_ts_new(sb);
    return slack_ts_cmp(tsa, tsb);
}

static int
slack_event_history_append_msg(struct t_slack_history_ctx *ctx,
                               struct json_object *msg)
{
    struct json_object **nm;
    struct json_object *ts_obj;
    const char *ts_str = NULL;
    int i;

    if (!ctx || !msg)
        return 0;

    /* dedupe by ts across pages (cursor/latest can overlap edges) */
    if (json_object_object_get_ex(msg, "ts", &ts_obj))
        ts_str = json_object_get_string(ts_obj);
    if (ts_str && ts_str[0])
    {
        for (i = 0; i < ctx->msg_count; i++)
        {
            struct json_object *ots;
            const char *os = NULL;
            if (ctx->msgs[i] &&
                json_object_object_get_ex(ctx->msgs[i], "ts", &ots))
                os = json_object_get_string(ots);
            if (os && strcmp(os, ts_str) == 0)
                return 1; /* already have it */
        }
    }

    if (ctx->msg_count >= ctx->msg_cap)
    {
        int ncap = ctx->msg_cap ? ctx->msg_cap * 2 : 64;
        nm = realloc(ctx->msgs, (size_t)ncap * sizeof(*nm));
        if (!nm)
            return 0;
        ctx->msgs = nm;
        ctx->msg_cap = ncap;
    }
    ctx->msgs[ctx->msg_count] = json_object_get(msg);
    ctx->msg_count++;
    return 1;
}

static void
slack_event_history_flush(struct t_weeslack_workspace *workspace,
                          struct t_slack_history_ctx *ctx)
{
    struct t_slack_channel *channel;
    int i, loaded = 0;

    if (!ctx)
        return;
    channel = ctx->channel;

    if (ctx->msg_count > 1)
        qsort(ctx->msgs, (size_t)ctx->msg_count, sizeof(ctx->msgs[0]),
              slack_event_history_msg_ts_cmp);

    for (i = 0; i < ctx->msg_count; i++)
    {
        if (ctx->msgs[i])
        {
            slack_event_handle_message(workspace, channel, ctx->msgs[i], 1);
            loaded++;
        }
    }

    if (channel && channel->buffer && loaded > 0)
    {
        weechat_printf(channel->buffer,
                        "%s--- loaded %d messages from history "
                        "(%d page%s%s) ---%s",
                        weechat_prefix("network"),
                        loaded,
                        ctx->page > 0 ? ctx->page : 1,
                        (ctx->page == 1) ? "" : "s",
                        ctx->truncated ? "; truncated, more on Slack" : "",
                        weechat_color("reset"));
    }

    if (channel)
    {
        channel->history_state = 3;
        channel->history_retries = 0;
    }

    slack_event_history_ctx_free(ctx);
}

static void slack_event_history_cb(struct t_weeslack_workspace *workspace,
                                    int return_code, const char *output,
                                    void *user_data);

static int
slack_event_history_request(struct t_weeslack_workspace *workspace,
                            struct t_slack_history_ctx *ctx,
                            const char *cursor,
                            const char *latest)
{
    struct json_object *params;
    const char *method;

    if (!workspace || !ctx || !ctx->channel)
        return 0;

    params = json_object_new_object();
    if (!params)
        return 0;

    if (ctx->is_replies)
    {
        method = "conversations.replies";
        json_object_object_add(params, "channel",
                               json_object_new_string(ctx->parent_id));
        json_object_object_add(params, "ts",
                               json_object_new_string(ctx->thread_ts));
    }
    else
    {
        method = "conversations.history";
        json_object_object_add(params, "channel",
                               json_object_new_string(ctx->channel->id));
    }
    json_object_object_add(params, "limit",
                           json_object_new_int(SLACK_HISTORY_PAGE_SIZE));
    if (cursor && cursor[0])
        json_object_object_add(params, "cursor",
                               json_object_new_string(cursor));
    else if (latest && latest[0])
    {
        /* older API shape: page older than this ts (exclusive) */
        json_object_object_add(params, "latest",
                               json_object_new_string(latest));
        json_object_object_add(params, "inclusive",
                               json_object_new_boolean(0));
    }

    if (!slack_http_request_new_flags(workspace, method, params,
                                      SLACK_HTTP_SLOW,
                                      slack_event_history_cb, ctx))
    {
        json_object_put(params);
        return 0;
    }
    json_object_put(params);
    return 1;
}

static void
slack_event_history_cb(struct t_weeslack_workspace *workspace,
                        int return_code, const char *output,
                        void *user_data)
{
    struct t_slack_history_ctx *ctx = user_data;
    struct t_slack_channel *channel;
    const char *api_name;
    struct json_object *json, *messages_obj;
    const char *next_cursor = NULL;
    int has_more = 0;

    if (!ctx)
        return;

    channel = ctx->channel;
    api_name = ctx->is_replies ? "conversations.replies" : "conversations.history";

    if (return_code != 0 || !output)
    {
        if (ctx->msg_count > 0)
            slack_event_history_flush(workspace, ctx);
        else
        {
            if (channel)
                channel->history_state = 0;
            slack_event_history_ctx_free(ctx);
        }
        return;
    }

    json = slack_json_decode(output);
    if (!json)
    {
        if (ctx->msg_count > 0)
            slack_event_history_flush(workspace, ctx);
        else
        {
            if (channel)
                channel->history_state = 0;
            slack_event_history_ctx_free(ctx);
        }
        return;
    }

    if (slack_api_check_error(workspace, json, api_name))
    {
        if (ctx->msg_count > 0)
            slack_event_history_flush(workspace, ctx);
        else
        {
            if (channel)
                channel->history_state = 0;
            slack_event_history_ctx_free(ctx);
        }
        json_object_put(json);
        return;
    }

    if (json_object_object_get_ex(json, "messages", &messages_obj))
    {
        int count = json_object_array_length(messages_obj);
        int start = ctx->is_replies ? 1 : 0;
        int i;

        for (i = start; i < count; i++)
        {
            struct json_object *msg_obj =
                json_object_array_get_idx(messages_obj, i);
            slack_event_history_append_msg(ctx, msg_obj);
        }
    }

    {
        struct json_object *hm;
        if (json_object_object_get_ex(json, "has_more", &hm))
            has_more = json_object_get_boolean(hm);
    }
    {
        struct json_object *meta, *cobj;
        if (json_object_object_get_ex(json, "response_metadata", &meta) &&
            json_object_object_get_ex(meta, "next_cursor", &cobj))
        {
            next_cursor = json_object_get_string(cobj);
            if (next_cursor && !next_cursor[0])
                next_cursor = NULL;
        }
    }

    ctx->page++;

    {
        char latest_buf[64];
        const char *page_cursor = next_cursor;
        const char *page_latest = NULL;

        latest_buf[0] = '\0';
        /* Prefer cursor; fall back to oldest message ts as latest= for older
         * responses that only set has_more. */
        if (!page_cursor && has_more && messages_obj)
        {
            int mcount = json_object_array_length(messages_obj);
            if (mcount > 0)
            {
                struct json_object *oldest =
                    json_object_array_get_idx(messages_obj, mcount - 1);
                struct json_object *ots;
                if (oldest &&
                    json_object_object_get_ex(oldest, "ts", &ots))
                {
                    const char *ots_s = json_object_get_string(ots);
                    if (ots_s && ots_s[0])
                    {
                        snprintf(latest_buf, sizeof(latest_buf), "%s", ots_s);
                        page_latest = latest_buf;
                    }
                }
            }
        }

        if ((page_cursor || page_latest) &&
            ctx->page < SLACK_HISTORY_MAX_PAGES)
        {
            char *cursor_copy = page_cursor ? strdup(page_cursor) : NULL;
            char *latest_copy = page_latest ? strdup(page_latest) : NULL;

            json_object_put(json);
            if (slack_event_history_request(workspace, ctx,
                                            cursor_copy, latest_copy))
            {
                free(cursor_copy);
                free(latest_copy);
                return;
            }
            free(cursor_copy);
            free(latest_copy);
            ctx->truncated = 1;
            slack_event_history_flush(workspace, ctx);
            return;
        }
    }

    if ((has_more || next_cursor) && ctx->page >= SLACK_HISTORY_MAX_PAGES)
        ctx->truncated = 1;
    else if (has_more || next_cursor)
        ctx->truncated = 1;

    json_object_put(json);
    slack_event_history_flush(workspace, ctx);
}

static void
slack_event_history_start(struct t_weeslack_workspace *workspace,
                           struct t_slack_channel *channel,
                           int is_replies)
{
    struct t_slack_history_ctx *ctx;

    if (!workspace || !channel || !channel->id)
        return;

    if (channel->history_state == 2 || channel->history_state == 3)
        return;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return;
    ctx->channel = channel;
    ctx->is_replies = is_replies;
    ctx->page = 0;
    channel->history_state = 2;

    if (is_replies)
    {
        if (strncmp(channel->id, "thread_", 7) == 0)
        {
            const char *rest = channel->id + 7;
            const char *us = strchr(rest, '_');
            if (us)
            {
                size_t n = (size_t)(us - rest);
                if (n < sizeof(ctx->parent_id))
                {
                    memcpy(ctx->parent_id, rest, n);
                    ctx->parent_id[n] = '\0';
                    snprintf(ctx->thread_ts, sizeof(ctx->thread_ts), "%s",
                             us + 1);
                }
            }
        }

        if (!ctx->parent_id[0] || !ctx->thread_ts[0])
        {
            channel->history_state = 0;
            slack_event_history_ctx_free(ctx);
            return;
        }
    }

    if (!slack_event_history_request(workspace, ctx, NULL, NULL))
    {
        channel->history_state = 0;
        slack_event_history_ctx_free(ctx);
    }
}

void
slack_event_fetch_history(struct t_weeslack_workspace *workspace,
                           struct t_slack_channel *channel)
{
    if (!workspace || !channel)
        return;

    if (channel->history_state == 3)
        return;
    if (channel->history_state == 2)
        return;

    if (channel->type == SLACK_CHANNEL_TYPE_THREAD)
        slack_event_history_start(workspace, channel, 1);
    else
        slack_event_history_start(workspace, channel, 0);
}

void
slack_event_fetch_history_force(struct t_weeslack_workspace *workspace,
                                 struct t_slack_channel *channel)
{
    if (!channel)
        return;
    channel->history_state = 0;
    channel->history_retries = 0;
    slack_event_fetch_history(workspace, channel);
}

void
slack_event_fetch_replies(struct t_weeslack_workspace *workspace,
                           struct t_slack_channel *thread)
{
    if (!workspace || !thread)
        return;
    if (thread->history_state == 3 || thread->history_state == 2)
        return;
    slack_event_history_start(workspace, thread, 1);
}

/* ============================================================
 * Users (needed before history so nicks resolve)
 * ============================================================ */

static void
slack_event_users_cb(struct t_weeslack_workspace *workspace,
                     int return_code, const char *output,
                     void *user_data)
{
    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: users.list failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        slack_event_fetch_bots(workspace);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (!json)
    {
        slack_event_fetch_bots(workspace);
        return;
    }

    if (slack_api_check_error(workspace, json, "users.list"))
    {
        json_object_put(json);
        slack_event_fetch_bots(workspace);
        return;
    }

    struct json_object *members_obj;
    if (json_object_object_get_ex(json, "members", &members_obj))
    {
        int count = json_object_array_length(members_obj);
        for (int i = 0; i < count; i++)
        {
            struct json_object *u_obj = json_object_array_get_idx(members_obj, i);
            struct json_object *id_obj, *name_obj;
            const char *uid = NULL;
            const char *uname = NULL;

            if (json_object_object_get_ex(u_obj, "id", &id_obj))
                uid = json_object_get_string(id_obj);
            if (json_object_object_get_ex(u_obj, "name", &name_obj))
                uname = json_object_get_string(name_obj);

            if (!uid || !uid[0])
                continue;
            if (!uname || !uname[0])
                uname = uid;

            struct t_slack_user *user = slack_user_new(uid, uname, NULL);
            if (user)
                slack_user_update(user, u_obj);
        }
    }

    /* paginate (capped) so large workspaces do not stampede */
#define SLACK_USERS_MAX_PAGES 10
    {
        const char *next_cursor = NULL;
        struct json_object *meta, *cursor_obj;
        int page = 0;

        if (user_data)
            page = (int)(intptr_t)user_data;

        if (json_object_object_get_ex(json, "response_metadata", &meta) &&
            json_object_object_get_ex(meta, "next_cursor", &cursor_obj))
        {
            next_cursor = json_object_get_string(cursor_obj);
            if (next_cursor && !next_cursor[0])
                next_cursor = NULL;
        }

        if (next_cursor && page + 1 < SLACK_USERS_MAX_PAGES)
        {
            char *cursor_copy = strdup(next_cursor);
            json_object_put(json);

            if (cursor_copy)
            {
                struct json_object *params = json_object_new_object();
                json_object_object_add(params, "limit",
                                       json_object_new_int(200));
                json_object_object_add(params, "cursor",
                                       json_object_new_string(cursor_copy));
                free(cursor_copy);
                slack_http_request_new(
                    workspace, "users.list", params,
                    slack_event_users_cb,
                    (void *)(intptr_t)(page + 1));
                json_object_put(params);
            }
            else
                slack_event_fetch_bots(workspace);
            return;
        }
    }

    json_object_put(json);

    {
        int total = 0;
        struct t_slack_user *u;
        for (u = slack_user_list_global(); u; u = u->next)
            total++;
        SLACK_WS_PRINTF(workspace, "%sweeslack: loaded %d users",
                        weechat_prefix("network"), total);
    }

    slack_event_fetch_bots(workspace);
}

void
slack_event_fetch_users(struct t_weeslack_workspace *workspace)
{
    if (!workspace)
        return;

    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "limit", json_object_new_int(200));

    /* user_data = page index for cap */
    slack_http_request_new(workspace, "users.list", params,
                           slack_event_users_cb, (void *)(intptr_t)0);

    json_object_put(params);
}

/* ============================================================
 * Bots directory
 * ============================================================ */

static void
slack_event_bots_cb(struct t_weeslack_workspace *workspace,
                    int return_code, const char *output,
                    void *user_data)
{
    (void) user_data;

    if (return_code == 0 && output)
    {
        struct json_object *json = slack_json_decode(output);
        if (json)
        {
            struct json_object *ok_obj, *err_obj;
            int ok = 0;
            const char *err = NULL;

            if (json_object_object_get_ex(json, "ok", &ok_obj))
                ok = json_object_get_boolean(ok_obj);
            if (json_object_object_get_ex(json, "error", &err_obj))
                err = json_object_get_string(err_obj);

            if (ok)
            {
                struct json_object *bots_obj;
                int loaded = 0;
                if (json_object_object_get_ex(json, "bots", &bots_obj))
                {
                    int count = json_object_array_length(bots_obj);
                    for (int i = 0; i < count; i++)
                    {
                        struct json_object *b = json_object_array_get_idx(bots_obj, i);
                        struct json_object *id_obj, *name_obj;
                        const char *bid = NULL, *bname = NULL;
                        if (json_object_object_get_ex(b, "id", &id_obj))
                            bid = json_object_get_string(id_obj);
                        if (json_object_object_get_ex(b, "name", &name_obj))
                            bname = json_object_get_string(name_obj);
                        if (!bid)
                            continue;
                        struct t_slack_bot *bot = slack_bot_new(
                            bid, bname && bname[0] ? bname : bid);
                        if (bot)
                        {
                            slack_bot_update(bot, b);
                            if (bname && bname[0])
                            {
                                free(bot->username);
                                bot->username = strdup(bname);
                            }
                            loaded++;
                        }
                    }
                }
                SLACK_WS_PRINTF(workspace, "%sweeslack: loaded %d bots",
                                weechat_prefix("network"), loaded);
            }
            else if (err && strcmp(err, "unknown_method") != 0 &&
                     strcmp(err, "not_allowed_token_type") != 0)
            {
                slack_api_check_error(workspace, json, "bots.list");
            }
            json_object_put(json);
        }
    }

    /* Bootstrap path: emoji.list then usergroups → channels (via non-NULL
     * user_data on the emoji callback). */
    slack_http_request_new(workspace, "emoji.list", NULL,
                           slack_event_emoji_cb, (void *)1);
}

void
slack_event_fetch_bots(struct t_weeslack_workspace *workspace)
{
    if (!workspace)
        return;

    /* bots.list is often missing on user tokens — skip noise, continue chain */
    slack_http_request_new(workspace, "bots.list", NULL,
                           slack_event_bots_cb, NULL);
}

static void
slack_event_emoji_cb(struct t_weeslack_workspace *workspace,
                     int return_code, const char *output,
                     void *user_data)
{
    /* user_data non-NULL = continue connect bootstrap (usergroups → channels).
     * emoji_changed / manual refresh must pass NULL so we do not re-list
     * conversations and recreate buffers. */
    int bootstrap = (user_data != NULL);

    if (return_code == 0 && output)
    {
        struct json_object *json = slack_json_decode(output);
        if (json)
        {
            struct json_object *ok_obj, *emoji_obj;
            int ok = 0;
            if (json_object_object_get_ex(json, "ok", &ok_obj))
                ok = json_object_get_boolean(ok_obj);
            if (ok && json_object_object_get_ex(json, "emoji", &emoji_obj))
            {
                struct json_object_iterator it, end;
                int n = 0;

                slack_custom_emoji_clear();
                it = json_object_iter_begin(emoji_obj);
                end = json_object_iter_end(emoji_obj);
                while (!json_object_iter_equal(&it, &end))
                {
                    const char *name = json_object_iter_peek_name(&it);
                    struct json_object *val = json_object_iter_peek_value(&it);
                    const char *v = json_object_get_string(val);
                    if (name && v)
                    {
                        slack_custom_emoji_add(name, v);
                        n++;
                    }
                    json_object_iter_next(&it);
                }
                SLACK_WS_PRINTF(workspace, "%sweeslack: loaded %d custom emoji",
                                weechat_prefix("network"), n);
            }
            json_object_put(json);
        }
    }

    if (bootstrap)
        slack_event_fetch_usergroups(workspace);
}

void
slack_event_fetch_emoji(struct t_weeslack_workspace *workspace)
{
    if (!workspace)
        return;
    /* Refresh only — do not re-enter connect bootstrap. */
    slack_http_request_new(workspace, "emoji.list", NULL,
                           slack_event_emoji_cb, NULL);
}

/* ============================================================
 * User groups
 * ============================================================ */

static void
slack_event_usergroups_cb(struct t_weeslack_workspace *workspace,
                          int return_code, const char *output,
                          void *user_data)
{
    (void) user_data;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: usergroups.list failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        slack_event_fetch_channels(workspace);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (!json)
    {
        slack_event_fetch_channels(workspace);
        return;
    }

    if (slack_api_check_error(workspace, json, "usergroups.list"))
    {
        json_object_put(json);
        /* non-fatal: continue without groups */
        slack_event_fetch_channels(workspace);
        return;
    }

    int loaded = 0;
    struct json_object *groups_obj;
    if (json_object_object_get_ex(json, "usergroups", &groups_obj))
    {
        int count = json_object_array_length(groups_obj);
        for (int i = 0; i < count; i++)
        {
            struct json_object *g = json_object_array_get_idx(groups_obj, i);
            struct json_object *id_obj, *name_obj;
            const char *gid = NULL;
            const char *gname = NULL;

            if (json_object_object_get_ex(g, "id", &id_obj))
                gid = json_object_get_string(id_obj);
            if (json_object_object_get_ex(g, "name", &name_obj))
                gname = json_object_get_string(name_obj);
            if (!gid)
                continue;

            struct t_slack_subteam *st = slack_subteam_new(
                gid, gname && gname[0] ? gname : gid);
            if (st)
            {
                slack_subteam_update(st, g);
                loaded++;
            }
        }
    }

    json_object_put(json);

    SLACK_WS_PRINTF(workspace, "%sweeslack: loaded %d user groups",
                    weechat_prefix("network"), loaded);

    slack_event_fetch_channels(workspace);
}

void
slack_event_fetch_usergroups(struct t_weeslack_workspace *workspace)
{
    if (!workspace)
        return;

    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "include_users",
                           json_object_new_boolean(0));
    json_object_object_add(params, "include_disabled",
                           json_object_new_boolean(0));

    slack_http_request_new(workspace, "usergroups.list", params,
                           slack_event_usergroups_cb, NULL);
    json_object_put(params);
}

/* ============================================================
 * Channel members → nicklist (slow queue, capped pages)
 * ============================================================ */

struct t_slack_members_ctx
{
    struct t_slack_channel *channel;
    int page;
    int total;
};

static void slack_event_members_cb(struct t_weeslack_workspace *workspace,
                                    int return_code, const char *output,
                                    void *user_data);

static int
slack_event_members_request(struct t_weeslack_workspace *workspace,
                            struct t_slack_members_ctx *ctx,
                            const char *cursor)
{
    struct json_object *params;

    if (!workspace || !ctx || !ctx->channel)
        return 0;

    params = json_object_new_object();
    if (!params)
        return 0;

    json_object_object_add(params, "channel",
                           json_object_new_string(ctx->channel->id));
    json_object_object_add(params, "limit",
                           json_object_new_int(SLACK_MEMBERS_PAGE_SIZE));
    if (cursor && cursor[0])
        json_object_object_add(params, "cursor",
                               json_object_new_string(cursor));

    if (!slack_http_request_new_flags(workspace, "conversations.members",
                                      params, SLACK_HTTP_SLOW,
                                      slack_event_members_cb, ctx))
    {
        json_object_put(params);
        return 0;
    }
    json_object_put(params);
    return 1;
}

static void
slack_event_members_cb(struct t_weeslack_workspace *workspace,
                       int return_code, const char *output,
                       void *user_data)
{
    struct t_slack_members_ctx *ctx = user_data;
    struct t_slack_channel *channel;
    struct t_slack_buffer *sbuf;
    const char *next_cursor = NULL;

    if (!ctx)
        return;
    channel = ctx->channel;
    if (!channel)
    {
        free(ctx);
        return;
    }

    if (return_code != 0 || !output)
    {
        if (ctx->total == 0)
            channel->members_loaded = 0;
        free(ctx);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (!json)
    {
        if (ctx->total == 0)
            channel->members_loaded = 0;
        free(ctx);
        return;
    }

    if (slack_api_check_error(workspace, json, "conversations.members"))
    {
        if (ctx->total == 0)
            channel->members_loaded = 0;
        json_object_put(json);
        free(ctx);
        return;
    }

    sbuf = slack_buffer_search_by_channel(channel->id);
    if (!sbuf)
        sbuf = channel->buffer ? slack_buffer_search(channel->buffer) : NULL;

    struct json_object *members_obj;
    if (json_object_object_get_ex(json, "members", &members_obj) && sbuf)
    {
        int count = json_object_array_length(members_obj);
        for (int i = 0; i < count; i++)
        {
            struct json_object *m = json_object_array_get_idx(members_obj, i);
            const char *uid = json_object_get_string(m);
            struct t_slack_user *user;

            if (!uid)
                continue;
            user = slack_user_search(uid);
            if (!user)
                user = slack_user_new(uid, uid, NULL);
            if (user)
            {
                slack_buffer_add_nick(sbuf, user);
                ctx->total++;
            }
        }
    }

    {
        struct json_object *meta, *cobj;
        if (json_object_object_get_ex(json, "response_metadata", &meta) &&
            json_object_object_get_ex(meta, "next_cursor", &cobj))
        {
            next_cursor = json_object_get_string(cobj);
            if (next_cursor && !next_cursor[0])
                next_cursor = NULL;
        }
    }

    ctx->page++;

    if (next_cursor && ctx->page < SLACK_MEMBERS_MAX_PAGES)
    {
        char *cursor_copy = strdup(next_cursor);
        json_object_put(json);
        if (cursor_copy &&
            slack_event_members_request(workspace, ctx, cursor_copy))
        {
            free(cursor_copy);
            return;
        }
        free(cursor_copy);
    }
    else
        json_object_put(json);

    /* members_loaded stays 1 (done / in-progress complete) */
    free(ctx);
}

void
slack_event_fetch_members(struct t_weeslack_workspace *workspace,
                           struct t_slack_channel *channel)
{
    struct t_slack_members_ctx *ctx;

    if (!workspace || !channel || !channel->id)
        return;
    if (channel->members_loaded)
        return;
    if (slack_event_in_bootstrap_quiet())
        return;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return;
    ctx->channel = channel;
    ctx->page = 0;
    ctx->total = 0;

    channel->members_loaded = 1; /* set early to dedupe; cleared on hard fail */

    if (!slack_event_members_request(workspace, ctx, NULL))
    {
        channel->members_loaded = 0;
        free(ctx);
    }
}

/* ============================================================
 * Mark as read
 * ============================================================ */

void
slack_event_mark_read(struct t_weeslack_workspace *workspace,
                       struct t_slack_channel *channel)
{
    char *ts_str = NULL;

    if (!workspace || !channel || !channel->id)
        return;

    if (channel->messages)
        ts_str = slack_ts_to_string(channel->messages->ts);
    else if (!slack_ts_is_empty(channel->last_read))
        ts_str = slack_ts_to_string(channel->last_read);

    if (!ts_str || !ts_str[0])
    {
        free(ts_str);
        /* no messages yet — still clear local hotlist */
        if (channel->buffer)
            slack_buffer_clear_hotlist(channel->buffer);
        channel->unread_count = 0;
        return;
    }

    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel->id));
    json_object_object_add(params, "ts",
                           json_object_new_string(ts_str));
    free(ts_str);

    /* low priority; dropped entirely while API is cooling down */
    slack_http_request_new_flags(workspace, "conversations.mark", params,
                                 SLACK_HTTP_MARK, NULL, NULL);
    json_object_put(params);

    channel->unread_count = 0;
    if (channel->buffer)
        slack_buffer_clear_hotlist(channel->buffer);
}

/* ============================================================
 * Channel list (auto-create buffers on connect)
 * ============================================================ */

static void
slack_event_channels_cb(struct t_weeslack_workspace *workspace,
                         int return_code, const char *output,
                         void *user_data)
{
    (void) user_data;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: conversations.list failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (!json)
        return;

    if (slack_api_check_error(workspace, json, "conversations.list"))
    {
        json_object_put(json);
        return;
    }

    struct json_object *channels_obj;
    if (json_object_object_get_ex(json, "channels", &channels_obj))
    {
        int count = json_object_array_length(channels_obj);
        int created = 0;

        for (int i = 0; i < count; i++)
        {
            struct json_object *ch_obj = json_object_array_get_idx(channels_obj, i);

            struct json_object *id_obj, *name_obj, *is_member_obj;
            struct json_object *is_im_obj, *is_group_obj, *is_private_obj;
            struct json_object *is_mpim_obj, *user_obj;
            const char *ch_id = NULL;
            const char *ch_name = NULL;
            char name_fallback[128];
            int is_member = 0;
            int is_im = 0;

            if (json_object_object_get_ex(ch_obj, "id", &id_obj))
                ch_id = json_object_get_string(id_obj);
            if (json_object_object_get_ex(ch_obj, "name", &name_obj))
                ch_name = json_object_get_string(name_obj);
            if (json_object_object_get_ex(ch_obj, "is_member", &is_member_obj))
                is_member = json_object_get_boolean(is_member_obj);

            if (json_object_object_get_ex(ch_obj, "is_im", &is_im_obj))
                is_im = json_object_get_boolean(is_im_obj);

            /* IMs have no "name" — resolve peer user id via users directory */
            if (is_im && json_object_object_get_ex(ch_obj, "user", &user_obj))
            {
                const char *peer_id = json_object_get_string(user_obj);
                struct t_slack_user *peer = peer_id
                    ? slack_user_search(peer_id) : NULL;
                const char *pretty = peer ? slack_user_best_name(peer) : NULL;

                if (pretty && pretty[0])
                    snprintf(name_fallback, sizeof(name_fallback), "%s", pretty);
                else if (peer_id && peer_id[0])
                    snprintf(name_fallback, sizeof(name_fallback), "%s", peer_id);
                else
                    name_fallback[0] = '\0';

                if (name_fallback[0])
                    ch_name = name_fallback;
            }

            if (!ch_id || !ch_name || !ch_name[0])
                continue;

            /* Open DMs/MPIMs are always "member" conversations */
            if (is_im)
                is_member = 1;
            if (json_object_object_get_ex(ch_obj, "is_mpim", &is_mpim_obj) &&
                json_object_get_boolean(is_mpim_obj))
                is_member = 1;

            /* determine channel type */
            enum slack_channel_type type = SLACK_CHANNEL_TYPE_CHANNEL;

            if (is_im)
            {
                type = SLACK_CHANNEL_TYPE_DM;
            }
            else if (json_object_object_get_ex(ch_obj, "is_mpim", &is_mpim_obj) &&
                     json_object_get_boolean(is_mpim_obj))
            {
                type = SLACK_CHANNEL_TYPE_MPDM;
            }
            else if ((json_object_object_get_ex(ch_obj, "is_group", &is_group_obj) &&
                      json_object_get_boolean(is_group_obj)) ||
                     (json_object_object_get_ex(ch_obj, "is_private", &is_private_obj) &&
                      json_object_get_boolean(is_private_obj)))
            {
                type = SLACK_CHANNEL_TYPE_GROUP;
            }

            struct t_slack_channel *channel = slack_channel_new(ch_id, ch_name, type);
            if (channel)
            {
                slack_channel_update(channel, ch_obj);
                if (is_member)
                    channel->is_member = 1;

                /* refresh DM display name (channel may already exist from earlier) */
                if (is_im && ch_name && ch_name[0] &&
                    (!channel->name || strcmp(channel->name, ch_name) != 0))
                {
                    free(channel->name);
                    channel->name = strdup(ch_name);
                    if (channel->buffer)
                    {
                        weechat_buffer_set(channel->buffer, "short_name", ch_name);
                        weechat_buffer_set(channel->buffer, "localvar_set_channel",
                                           ch_name);
                    }
                }

                if (is_member && !channel->buffer)
                {
                    struct t_slack_buffer *sbuf = slack_buffer_new(workspace, channel);
                    if (sbuf)
                        created++;
                    else
                        SLACK_WS_PRINTF(workspace,
                                        "%sweeslack: failed to create buffer for #%s",
                                        weechat_prefix("error"), ch_name);
                }
            }
        }

        SLACK_WS_PRINTF(workspace, "%sweeslack: loaded %d channels (%d buffers created)",
                        weechat_prefix("network"), count, created);
        /* ignore buffer_switch history storms while buffers settle */
        slack_event_bootstrap_quiet(8);
    }

    {
        const char *next_cursor = NULL;
        struct json_object *meta, *cursor_obj;
        if (json_object_object_get_ex(json, "response_metadata", &meta) &&
            json_object_object_get_ex(meta, "next_cursor", &cursor_obj))
        {
            next_cursor = json_object_get_string(cursor_obj);
            if (next_cursor && !next_cursor[0])
                next_cursor = NULL;
        }

        if (next_cursor)
        {
            char *cursor_copy = strdup(next_cursor);
            json_object_put(json);
            if (cursor_copy)
            {
                struct json_object *params = json_object_new_object();
                json_object_object_add(params, "types",
                    json_object_new_string(
                        "public_channel,private_channel,mpim,im"));
                json_object_object_add(params, "exclude_archived",
                                       json_object_new_boolean(1));
                json_object_object_add(params, "limit",
                                       json_object_new_int(200));
                json_object_object_add(params, "cursor",
                                       json_object_new_string(cursor_copy));
                free(cursor_copy);
                slack_http_request_new(workspace, "conversations.list", params,
                                       slack_event_channels_cb, NULL);
                json_object_put(params);
            }
            return;
        }
    }

    json_object_put(json);
}

void
slack_event_fetch_channels(struct t_weeslack_workspace *workspace)
{
    if (!workspace)
        return;

    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "types",
                           json_object_new_string("public_channel,private_channel,mpim,im"));
    json_object_object_add(params, "exclude_archived",
                           json_object_new_boolean(1));
    json_object_object_add(params, "limit",
                           json_object_new_int(200));

    slack_http_request_new(workspace, "conversations.list", params,
                           slack_event_channels_cb, NULL);

    json_object_put(params);
}

/* ============================================================
 * Typing notifications
 * ============================================================ */

void
slack_event_send_typing(struct t_weeslack_workspace *workspace,
                         const char *channel_id)
{
    if (!workspace || !workspace->ws || !workspace->ws->connected)
        return;

    if (!channel_id)
        return;

    struct json_object *msg = json_object_new_object();
    json_object_object_add(msg, "type", json_object_new_string("typing"));
    json_object_object_add(msg, "channel", json_object_new_string(channel_id));

    slack_ws_send(workspace->ws, msg);

    json_object_put(msg);
}

/* ============================================================
 * Send message
 * ============================================================ */

static void
slack_event_send_message_cb(struct t_weeslack_workspace *workspace,
                             int return_code, const char *output,
                             void *user_data)
{
    (void) workspace;
    (void) user_data;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: failed to send message (rc=%d)",
                        weechat_prefix("error"), return_code);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (!json)
        return;

    slack_api_check_error(workspace, json, "chat.postMessage");

    json_object_put(json);
}

void
slack_event_send_message(struct t_weeslack_workspace *workspace,
                          const char *channel_id,
                          const char *text,
                          const char *thread_ts)
{
    if (!workspace || !channel_id || !text || !text[0])
        return;

    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    json_object_object_add(params, "text",
                           json_object_new_string(text));

    if (thread_ts && thread_ts[0])
    {
        json_object_object_add(params, "thread_ts",
                               json_object_new_string(thread_ts));
    }

    slack_http_request_new(workspace, "chat.postMessage", params,
                           slack_event_send_message_cb, NULL);

    json_object_put(params);
}

/* ============================================================
 * File upload (getUploadURLExternal → PUT → completeUploadExternal)
 * ============================================================ */

struct t_slack_upload_ctx
{
    struct t_weeslack_workspace *workspace;
    char *channel_id;
    char *file_path;
    char *thread_ts;
    char *filename;
    char *file_id;
    char *upload_url;
    long length;
};

static void
slack_event_upload_ctx_free(struct t_slack_upload_ctx *ctx)
{
    if (!ctx)
        return;
    free(ctx->channel_id);
    free(ctx->file_path);
    free(ctx->thread_ts);
    free(ctx->filename);
    free(ctx->file_id);
    free(ctx->upload_url);
    free(ctx);
}

static void
slack_event_upload_complete_cb(struct t_weeslack_workspace *workspace,
                                int return_code, const char *output,
                                void *user_data)
{
    struct t_slack_upload_ctx *ctx = user_data;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: file complete failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        slack_event_upload_ctx_free(ctx);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (json)
    {
        if (!slack_api_check_error(workspace, json, "files.completeUploadExternal"))
        {
            SLACK_WS_PRINTF(workspace, "%sweeslack: uploaded %s",
                            weechat_prefix("network"),
                            ctx && ctx->filename ? ctx->filename : "file");
        }
        json_object_put(json);
    }

    slack_event_upload_ctx_free(ctx);
}

static int
slack_event_upload_put_cb(const void *pointer, void *data,
                           const char *command, int return_code,
                           const char *out, const char *err)
{
    struct t_slack_upload_ctx *ctx = (struct t_slack_upload_ctx *)pointer;
    (void) data;
    (void) command;
    (void) out;

    if (!ctx)
        return WEECHAT_RC_OK;

    if (return_code < 0)
    {
        SLACK_WS_PRINTF(ctx->workspace, "%sweeslack: file PUT failed: %s",
                        weechat_prefix("error"), err ? err : "");
        slack_event_upload_ctx_free(ctx);
        return WEECHAT_RC_OK;
    }

    {
        char files_json[512];
        struct json_object *params = json_object_new_object();

        snprintf(files_json, sizeof(files_json),
                 "[{\"id\":\"%s\",\"title\":\"%s\"}]",
                 ctx->file_id ? ctx->file_id : "",
                 ctx->filename ? ctx->filename : "file");

        json_object_object_add(params, "files",
                               json_object_new_string(files_json));
        json_object_object_add(params, "channel_id",
                               json_object_new_string(ctx->channel_id));
        if (ctx->thread_ts && ctx->thread_ts[0])
        {
            json_object_object_add(params, "thread_ts",
                                   json_object_new_string(ctx->thread_ts));
        }

        slack_http_request_new(ctx->workspace, "files.completeUploadExternal",
                               params, slack_event_upload_complete_cb, ctx);
        json_object_put(params);
    }

    return WEECHAT_RC_OK;
}

static void
slack_event_upload_url_cb(struct t_weeslack_workspace *workspace,
                           int return_code, const char *output,
                           void *user_data)
{
    struct t_slack_upload_ctx *ctx = user_data;
    struct t_hashtable *options;
    char file_arg[1100];

    if (!ctx)
        return;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: getUploadURL failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        slack_event_upload_ctx_free(ctx);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (!json || slack_api_check_error(workspace, json, "files.getUploadURLExternal"))
    {
        if (json)
            json_object_put(json);
        slack_event_upload_ctx_free(ctx);
        return;
    }

    {
        struct json_object *url_obj, *id_obj;
        if (json_object_object_get_ex(json, "upload_url", &url_obj))
            ctx->upload_url = strdup(json_object_get_string(url_obj));
        if (json_object_object_get_ex(json, "file_id", &id_obj))
            ctx->file_id = strdup(json_object_get_string(id_obj));
    }
    json_object_put(json);

    if (!ctx->upload_url || !ctx->file_id)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: upload URL missing fields",
                        weechat_prefix("error"));
        slack_event_upload_ctx_free(ctx);
        return;
    }

    options = weechat_hashtable_new(32, WEECHAT_HASHTABLE_STRING,
                                    WEECHAT_HASHTABLE_STRING, NULL, NULL);
    if (!options)
    {
        slack_event_upload_ctx_free(ctx);
        return;
    }

    snprintf(file_arg, sizeof(file_arg), "@%s", ctx->file_path);

    {
        int argi = 1;
        char key[16];

        slack_http_curl_add_proxy(options, &argi);
        snprintf(key, sizeof(key), "arg%d", argi++);
        weechat_hashtable_set(options, key, "-sS");
        snprintf(key, sizeof(key), "arg%d", argi++);
        weechat_hashtable_set(options, key, "-X");
        snprintf(key, sizeof(key), "arg%d", argi++);
        weechat_hashtable_set(options, key, "POST");
        snprintf(key, sizeof(key), "arg%d", argi++);
        weechat_hashtable_set(options, key, "-T");
        snprintf(key, sizeof(key), "arg%d", argi++);
        weechat_hashtable_set(options, key, file_arg);
        snprintf(key, sizeof(key), "arg%d", argi++);
        weechat_hashtable_set(options, key, ctx->upload_url);
    }

    weechat_hook_process_hashtable(
        "curl", options, 120000,
        &slack_event_upload_put_cb, ctx, NULL);

    weechat_hashtable_free(options);
}

void
slack_event_upload_file(struct t_weeslack_workspace *workspace,
                         const char *channel_id,
                         const char *file_path,
                         const char *thread_ts)
{
    struct t_slack_upload_ctx *ctx;
    struct stat st;
    const char *base;

    if (!workspace || !channel_id || !file_path || !file_path[0])
        return;

    if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: cannot read file %s",
                        weechat_prefix("error"), file_path);
        return;
    }

    base = strrchr(file_path, '/');
    base = base ? base + 1 : file_path;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return;

    ctx->workspace = workspace;
    ctx->channel_id = strdup(channel_id);
    ctx->file_path = strdup(file_path);
    ctx->thread_ts = thread_ts ? strdup(thread_ts) : NULL;
    ctx->filename = strdup(base);
    ctx->length = (long)st.st_size;

    if (!ctx->channel_id || !ctx->file_path || !ctx->filename)
    {
        slack_event_upload_ctx_free(ctx);
        return;
    }

    {
        struct json_object *params = json_object_new_object();
        json_object_object_add(params, "filename",
                               json_object_new_string(ctx->filename));
        json_object_object_add(params, "length",
                               json_object_new_int64(ctx->length));
        slack_http_request_new(workspace, "files.getUploadURLExternal", params,
                               slack_event_upload_url_cb, ctx);
        json_object_put(params);
    }
}

static void
slack_event_set_topic_http_cb(struct t_weeslack_workspace *workspace,
                               int return_code, const char *output,
                               void *user_data)
{
    (void) user_data;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: set topic failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (json)
    {
        if (!slack_api_check_error(workspace, json, "conversations.setTopic"))
        {
            SLACK_WS_PRINTF(workspace, "%sweeslack: topic updated",
                            weechat_prefix("network"));
        }
        json_object_put(json);
    }
}

/*
 * Set channel topic via conversations.setTopic.
 */
void
slack_event_set_topic(struct t_weeslack_workspace *workspace,
                       const char *channel_id,
                       const char *topic)
{
    if (!workspace || !channel_id || !topic)
        return;

    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    json_object_object_add(params, "topic",
                           json_object_new_string(topic));
    slack_http_request_new(workspace, "conversations.setTopic", params,
                           slack_event_set_topic_http_cb, NULL);
    json_object_put(params);
}

struct t_slack_open_dm_ctx
{
    char *peer_id;
    char *peer_name;
};

static void
slack_event_open_dm_http_cb(struct t_weeslack_workspace *workspace,
                             int return_code, const char *output,
                             void *user_data)
{
    struct t_slack_open_dm_ctx *ctx = user_data;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: open DM failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        if (ctx)
        {
            free(ctx->peer_id);
            free(ctx->peer_name);
            free(ctx);
        }
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (!json)
    {
        if (ctx)
        {
            free(ctx->peer_id);
            free(ctx->peer_name);
            free(ctx);
        }
        return;
    }

    if (slack_api_check_error(workspace, json, "conversations.open"))
    {
        json_object_put(json);
        if (ctx)
        {
            free(ctx->peer_id);
            free(ctx->peer_name);
            free(ctx);
        }
        return;
    }

    struct json_object *channel_obj;
    if (json_object_object_get_ex(json, "channel", &channel_obj))
    {
        struct json_object *id_obj;
        if (json_object_object_get_ex(channel_obj, "id", &id_obj))
        {
            const char *channel_id = json_object_get_string(id_obj);
            const char *name = (ctx && ctx->peer_name && ctx->peer_name[0])
                ? ctx->peer_name
                : ((ctx && ctx->peer_id) ? ctx->peer_id : channel_id);
            struct t_slack_channel *channel = slack_channel_search(channel_id);

            if (!channel)
            {
                channel = slack_channel_new(channel_id, name,
                                            SLACK_CHANNEL_TYPE_DM);
                if (channel && ctx && ctx->peer_id)
                {
                    free(channel->user_id);
                    channel->user_id = strdup(ctx->peer_id);
                }
                if (channel && workspace)
                    slack_buffer_new(workspace, channel);
            }
            if (channel && channel->buffer)
                weechat_buffer_set(channel->buffer, "display", "1");
        }
    }

    json_object_put(json);
    if (ctx)
    {
        free(ctx->peer_id);
        free(ctx->peer_name);
        free(ctx);
    }
}

void
slack_event_open_dm(struct t_weeslack_workspace *workspace,
                     const char *user_id_or_name)
{
    struct t_slack_user *user;
    struct t_slack_open_dm_ctx *ctx;
    const char *uid;

    if (!workspace || !user_id_or_name || !user_id_or_name[0])
        return;

    user = slack_user_search_name(user_id_or_name);
    uid = user ? user->id : user_id_or_name;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return;
    ctx->peer_id = strdup(uid);
    ctx->peer_name = user ? strdup(slack_user_best_name(user)) : strdup(uid);
    if (!ctx->peer_id)
    {
        free(ctx->peer_name);
        free(ctx);
        return;
    }

    {
        struct json_object *params = json_object_new_object();
        json_object_object_add(params, "users",
                               json_object_new_string(uid));
        slack_http_request_new(workspace, "conversations.open", params,
                               slack_event_open_dm_http_cb, ctx);
        json_object_put(params);
    }
}

static void
slack_event_simple_api_cb(struct t_weeslack_workspace *workspace,
                           int return_code, const char *output,
                           void *user_data)
{
    const char *ctx = user_data ? (const char *)user_data : "API";

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: %s failed (rc=%d)",
                        weechat_prefix("error"), ctx, return_code);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (json)
    {
        slack_api_check_error(workspace, json, ctx);
        json_object_put(json);
    }
}

void
slack_event_set_dnd(struct t_weeslack_workspace *workspace, int enable)
{
    if (!workspace)
        return;

    if (enable)
    {
        struct json_object *params = json_object_new_object();
        /* 24h num_minutes default when enabling */
        json_object_object_add(params, "num_minutes",
                               json_object_new_int(24 * 60));
        slack_http_request_new(workspace, "dnd.setSnooze", params,
                               slack_event_simple_api_cb, (void *)"dnd.setSnooze");
        json_object_put(params);
    }
    else
    {
        slack_http_request_new(workspace, "dnd.endSnooze", NULL,
                               slack_event_simple_api_cb, (void *)"dnd.endSnooze");
    }
}

void
slack_event_set_presence(struct t_weeslack_workspace *workspace,
                          const char *presence)
{
    if (!workspace || !presence || !presence[0])
        return;

    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "presence",
                           json_object_new_string(presence));
    slack_http_request_new(workspace, "users.setPresence", params,
                           slack_event_simple_api_cb, (void *)"users.setPresence");
    json_object_put(params);
}

struct t_slack_mute_ctx
{
    char *channel_id;
    int mute;
};

static void
slack_event_mute_prefs_set_cb(struct t_weeslack_workspace *workspace,
                               int return_code, const char *output,
                               void *user_data)
{
    struct t_slack_mute_ctx *ctx = user_data;
    (void) return_code;

    if (output)
    {
        struct json_object *json = slack_json_decode(output);
        if (json)
        {
            slack_api_check_error(workspace, json, "users.prefs.set mute");
            json_object_put(json);
        }
    }

    if (ctx)
    {
        free(ctx->channel_id);
        free(ctx);
    }
}

static void
slack_event_mute_prefs_get_cb(struct t_weeslack_workspace *workspace,
                               int return_code, const char *output,
                               void *user_data)
{
    struct t_slack_mute_ctx *ctx = user_data;
    char muted[4096];
    char new_muted[4096];
    int found = 0;

    muted[0] = '\0';
    new_muted[0] = '\0';

    if (!ctx)
        return;

    if (return_code == 0 && output)
    {
        struct json_object *json = slack_json_decode(output);
        if (json && !slack_api_check_error(workspace, json, "users.prefs.get"))
        {
            struct json_object *prefs, *mc;
            if (json_object_object_get_ex(json, "prefs", &prefs) &&
                json_object_object_get_ex(prefs, "muted_channels", &mc))
            {
                const char *s = json_object_get_string(mc);
                if (s)
                    snprintf(muted, sizeof(muted), "%s", s);
            }
        }
        if (json)
            json_object_put(json);
    }

    /* rebuild muted_channels list */
    if (muted[0])
    {
        char *copy = strdup(muted);
        char *save = NULL;
        char *tok;
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
                if (strcmp(tok, ctx->channel_id) == 0)
                {
                    found = 1;
                    if (ctx->mute)
                    {
                        /* keep */
                        if (pos > 0 && pos < sizeof(new_muted) - 1)
                            new_muted[pos++] = ',';
                        snprintf(new_muted + pos, sizeof(new_muted) - pos, "%s",
                                 tok);
                        pos = strlen(new_muted);
                    }
                    /* if unmuting, skip */
                }
                else
                {
                    if (pos > 0 && pos < sizeof(new_muted) - 1)
                        new_muted[pos++] = ',';
                    snprintf(new_muted + pos, sizeof(new_muted) - pos, "%s", tok);
                    pos = strlen(new_muted);
                }
            }
            free(copy);
        }
    }

    if (ctx->mute && !found)
    {
        if (new_muted[0])
            strncat(new_muted, ",", sizeof(new_muted) - strlen(new_muted) - 1);
        strncat(new_muted, ctx->channel_id,
                sizeof(new_muted) - strlen(new_muted) - 1);
    }

    {
        struct json_object *params = json_object_new_object();
        json_object_object_add(params, "muted_channels",
                               json_object_new_string(new_muted));
        slack_http_request_new(workspace, "users.prefs.set", params,
                               slack_event_mute_prefs_set_cb, ctx);
        json_object_put(params);
    }
}

/*
 * Mute/unmute: local notify/hotlist + users.prefs muted_channels when possible.
 */
void
slack_event_set_mute(struct t_weeslack_workspace *workspace,
                      const char *channel_id, int mute)
{
    struct t_slack_channel *channel;
    struct t_slack_buffer *sbuf;
    struct t_slack_mute_ctx *ctx;

    if (!workspace || !channel_id || !channel_id[0])
        return;

    channel = slack_channel_search(channel_id);
    sbuf = slack_buffer_search_by_channel(channel_id);
    if (sbuf)
        slack_buffer_set_muted(sbuf, mute);
    else if (channel)
        channel->is_muted = mute ? 1 : 0;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return;
    ctx->channel_id = strdup(channel_id);
    ctx->mute = mute ? 1 : 0;
    if (!ctx->channel_id)
    {
        free(ctx);
        return;
    }

    slack_http_request_new(workspace, "users.prefs.get", NULL,
                           slack_event_mute_prefs_get_cb, ctx);
}

static void
slack_event_permalink_cb(struct t_weeslack_workspace *workspace,
                          int return_code, const char *output,
                          void *user_data)
{
    struct t_gui_buffer *buffer = user_data;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: permalink failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (!json)
        return;

    if (!slack_api_check_error(workspace, json, "chat.getPermalink"))
    {
        struct json_object *permalink_obj;
        if (json_object_object_get_ex(json, "permalink", &permalink_obj))
        {
            const char *permalink = json_object_get_string(permalink_obj);
            if (permalink)
                weechat_printf(buffer, "%s%s",
                                weechat_prefix("network"), permalink);
        }
    }
    json_object_put(json);
}

void
slack_event_get_permalink(struct t_weeslack_workspace *workspace,
                           const char *channel_id,
                           const char *timestamp,
                           struct t_gui_buffer *buffer)
{
    if (!workspace || !channel_id || !timestamp)
        return;

    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    json_object_object_add(params, "message_ts",
                           json_object_new_string(timestamp));
    slack_http_request_new(workspace, "chat.getPermalink", params,
                           slack_event_permalink_cb, buffer);
    json_object_put(params);
}

void
slack_event_pin_message(struct t_weeslack_workspace *workspace,
                         const char *channel_id,
                         const char *timestamp,
                         int pin)
{
    if (!workspace || !channel_id || !timestamp)
        return;

    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    json_object_object_add(params, "timestamp",
                           json_object_new_string(timestamp));
    slack_http_request_new(workspace,
                           pin ? "pins.add" : "pins.remove",
                           params,
                           slack_event_simple_api_cb,
                           (void *)(pin ? "pins.add" : "pins.remove"));
    json_object_put(params);
}

static void
slack_event_search_http_cb(struct t_weeslack_workspace *workspace,
                            int return_code, const char *output,
                            void *user_data)
{
    struct t_gui_buffer *buffer = user_data;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: search failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (!json)
        return;

    if (slack_api_check_error(workspace, json, "search.messages"))
    {
        json_object_put(json);
        return;
    }

    struct json_object *messages_obj;
    if (json_object_object_get_ex(json, "messages", &messages_obj))
    {
        struct json_object *total_obj;
        int total = 0;
        if (json_object_object_get_ex(messages_obj, "total", &total_obj))
            total = json_object_get_int(total_obj);

        weechat_printf(buffer, "%sweeslack: %d results found",
                        weechat_prefix("network"), total);

        struct json_object *matches_obj;
        if (json_object_object_get_ex(messages_obj, "matches", &matches_obj))
        {
            int count = json_object_array_length(matches_obj);
            for (int i = 0; i < count && i < 20; i++)
            {
                struct json_object *match = json_object_array_get_idx(matches_obj, i);
                struct json_object *text_obj, *ts_obj, *ch_obj, *name_obj;
                struct json_object *user_obj, *username_obj;
                const char *text = "?", *ts = "?", *ch = "?", *who = "";
                char ch_disp[128];
                char *fmt = NULL, *emoji = NULL, *ment = NULL;
                struct t_slack_channel *sch = NULL;
                struct t_slack_user *su = NULL;

                if (json_object_object_get_ex(match, "text", &text_obj))
                    text = json_object_get_string(text_obj);
                if (json_object_object_get_ex(match, "ts", &ts_obj))
                    ts = json_object_get_string(ts_obj);
                if (json_object_object_get_ex(match, "username", &username_obj))
                    who = json_object_get_string(username_obj);
                if (json_object_object_get_ex(match, "user", &user_obj))
                {
                    const char *uid = json_object_get_string(user_obj);
                    su = uid ? slack_user_search(uid) : NULL;
                    if (su)
                        who = slack_user_best_name(su);
                    else if (!who[0] && uid)
                        who = uid;
                }
                if (json_object_object_get_ex(match, "channel", &ch_obj))
                {
                    if (json_object_is_type(ch_obj, json_type_object) &&
                        json_object_object_get_ex(ch_obj, "name", &name_obj))
                        ch = json_object_get_string(name_obj);
                    else if (json_object_is_type(ch_obj, json_type_string))
                    {
                        ch = json_object_get_string(ch_obj);
                        sch = slack_channel_search(ch);
                        if (sch && sch->name && sch->name[0])
                            ch = sch->name;
                    }
                    else if (json_object_is_type(ch_obj, json_type_object))
                    {
                        struct json_object *id_obj;
                        if (json_object_object_get_ex(ch_obj, "id", &id_obj))
                        {
                            const char *cid = json_object_get_string(id_obj);
                            sch = cid ? slack_channel_search(cid) : NULL;
                            if (sch && sch->name)
                                ch = sch->name;
                        }
                    }
                }

                snprintf(ch_disp, sizeof(ch_disp), "%s%s",
                         (sch && (sch->type == SLACK_CHANNEL_TYPE_DM ||
                                  sch->type == SLACK_CHANNEL_TYPE_MPDM))
                             ? "" : "#",
                         ch ? ch : "?");

                fmt = slack_event_render_formatting(text);
                emoji = slack_event_apply_emoji_mode(fmt ? fmt : text);
                ment = slack_event_format_mentions(workspace,
                                                   emoji ? emoji : text, sch);

                weechat_printf(buffer, "%s  [%s] %s%s%s: %.120s%s",
                                weechat_prefix("network"),
                                ch_disp,
                                who && who[0] ? who : "",
                                who && who[0] ? " " : "",
                                ts ? ts : "?",
                                ment ? ment : text,
                                (ment && strlen(ment) > 120) ||
                                (!ment && strlen(text) > 120) ? "..." : "");
                free(fmt);
                free(emoji);
                free(ment);
            }
        }
    }

    json_object_put(json);
}

void
slack_event_search_messages(struct t_weeslack_workspace *workspace,
                             const char *query,
                             struct t_gui_buffer *buffer)
{
    if (!workspace || !query || !query[0])
        return;

    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "query", json_object_new_string(query));
    json_object_object_add(params, "count", json_object_new_int(15));
    slack_http_request_new(workspace, "search.messages", params,
                           slack_event_search_http_cb, buffer);
    json_object_put(params);
}

void
slack_event_react(struct t_weeslack_workspace *workspace,
                   const char *channel_id, const char *timestamp,
                   const char *emoji, int add)
{
    char name[64];
    const char *e;

    if (!workspace || !channel_id || !timestamp || !emoji || !emoji[0])
        return;

    e = emoji;
    if (e[0] == ':')
    {
        size_t n = strlen(e);
        if (n >= 2 && e[n - 1] == ':')
        {
            if (n - 2 < sizeof(name))
            {
                memcpy(name, e + 1, n - 2);
                name[n - 2] = '\0';
                e = name;
            }
        }
    }

    {
        struct json_object *params = json_object_new_object();
        json_object_object_add(params, "channel",
                               json_object_new_string(channel_id));
        json_object_object_add(params, "timestamp",
                               json_object_new_string(timestamp));
        json_object_object_add(params, "name",
                               json_object_new_string(e));
        slack_http_request_new(workspace,
                               add ? "reactions.add" : "reactions.remove",
                               params,
                               slack_event_simple_api_cb,
                               (void *)(add ? "reactions.add" : "reactions.remove"));
        json_object_put(params);
    }
}

void
slack_event_set_subscribe(struct t_weeslack_workspace *workspace,
                           struct t_slack_channel *channel,
                           int subscribe)
{
    struct t_slack_buffer *sbuf;

    if (!channel)
        return;

    (void) workspace;
    channel->is_subscribed = subscribe ? 1 : 0;

    if (channel->buffer)
    {
        weechat_buffer_set(channel->buffer, "localvar_set_slack_subscribed",
                           subscribe ? "1" : "0");
        /* Thread buffers: subscribed → highlight notify, else low noise */
        if (channel->type == SLACK_CHANNEL_TYPE_THREAD)
            weechat_buffer_set(channel->buffer, "notify",
                               subscribe ? "highlight" : "none");
    }

    sbuf = slack_buffer_search_by_channel(channel->id);
    if (sbuf && sbuf->buffer && channel->type == SLACK_CHANNEL_TYPE_THREAD)
        weechat_buffer_set(sbuf->buffer, "notify",
                           subscribe ? "highlight" : "none");
}

/* ============================================================
 * File download (authenticated curl)
 * ============================================================ */

struct t_slack_dl_ctx
{
    struct t_weeslack_workspace *workspace;
    char *path;
    struct t_gui_buffer *buffer;
};

static int
slack_event_download_cb(const void *pointer, void *data,
                        const char *command, int return_code,
                        const char *out, const char *err)
{
    struct t_slack_dl_ctx *ctx = (struct t_slack_dl_ctx *)pointer;
    (void) data;
    (void) command;
    (void) out;

    if (!ctx)
        return WEECHAT_RC_OK;

    if (return_code == 0)
    {
        weechat_printf(ctx->buffer ? ctx->buffer : NULL,
                        "%sweeslack: downloaded %s",
                        weechat_prefix("network"),
                        ctx->path ? ctx->path : "?");
    }
    else
    {
        weechat_printf(ctx->buffer ? ctx->buffer : NULL,
                        "%sweeslack: download failed (rc=%d)%s%s",
                        weechat_prefix("error"), return_code,
                        err && err[0] ? ": " : "",
                        err ? err : "");
    }

    free(ctx->path);
    free(ctx);
    return WEECHAT_RC_OK;
}

void
slack_event_download_file(struct t_weeslack_workspace *workspace,
                           const char *url,
                           struct t_gui_buffer *buffer)
{
    const char *dir_cfg;
    char dir[512];
    char path[768];
    char auth[512];
    const char *base;
    struct t_hashtable *options;
    struct t_slack_dl_ctx *ctx;
    const char *home;

    if (!workspace || !url || !url[0])
        return;

    dir_cfg = weechat_config_string(weeslack_config.download_path);
    if (dir_cfg && dir_cfg[0])
        snprintf(dir, sizeof(dir), "%s", dir_cfg);
    else
    {
        home = getenv("HOME");
        snprintf(dir, sizeof(dir), "%s/Downloads/weeslack",
                 home ? home : "/tmp");
    }

    /* Avoid system("mkdir -p …") — config path must not go through a shell. */
    weechat_mkdir_parents(dir, 0755);

    base = strrchr(url, '/');
    base = base ? base + 1 : "download.bin";
    /* strip query string from basename */
    {
        char base_buf[256];
        const char *q = strchr(base, '?');
        size_t blen = q ? (size_t)(q - base) : strlen(base);
        if (blen == 0)
        {
            snprintf(base_buf, sizeof(base_buf), "download.bin");
        }
        else
        {
            if (blen >= sizeof(base_buf))
                blen = sizeof(base_buf) - 1;
            memcpy(base_buf, base, blen);
            base_buf[blen] = '\0';
            /* strip unsafe path chars */
            for (char *p = base_buf; *p; p++)
            {
                if (*p == '/' || *p == '\\' || *p == '\0')
                    *p = '_';
            }
        }
        snprintf(path, sizeof(path), "%s/%s", dir, base_buf);
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return;
    ctx->workspace = workspace;
    ctx->path = strdup(path);
    ctx->buffer = buffer;

    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", workspace->token);

    options = weechat_hashtable_new(32, WEECHAT_HASHTABLE_STRING,
                                    WEECHAT_HASHTABLE_STRING, NULL, NULL);
    if (!options)
    {
        free(ctx->path);
        free(ctx);
        return;
    }

    {
        int argi = 1;
        char key[16];
        char cookie_h[512];

        slack_http_curl_add_proxy(options, &argi);
        snprintf(key, sizeof(key), "arg%d", argi++);
        weechat_hashtable_set(options, key, "-sSL");
        snprintf(key, sizeof(key), "arg%d", argi++);
        weechat_hashtable_set(options, key, "-H");
        snprintf(key, sizeof(key), "arg%d", argi++);
        weechat_hashtable_set(options, key, auth);
        if (workspace->cookie && workspace->cookie[0])
        {
            snprintf(cookie_h, sizeof(cookie_h), "Cookie: %s%s",
                     (strncmp(workspace->cookie, "d=", 2) == 0) ? "" : "d=",
                     workspace->cookie);
            snprintf(key, sizeof(key), "arg%d", argi++);
            weechat_hashtable_set(options, key, "-H");
            snprintf(key, sizeof(key), "arg%d", argi++);
            weechat_hashtable_set(options, key, cookie_h);
        }
        snprintf(key, sizeof(key), "arg%d", argi++);
        weechat_hashtable_set(options, key, "-o");
        snprintf(key, sizeof(key), "arg%d", argi++);
        weechat_hashtable_set(options, key, path);
        snprintf(key, sizeof(key), "arg%d", argi++);
        weechat_hashtable_set(options, key, url);
    }

    weechat_hook_process_hashtable(
        "curl", options, 120000,
        &slack_event_download_cb, ctx, NULL);
    weechat_hashtable_free(options);

    weechat_printf(buffer, "%sweeslack: downloading to %s ...",
                    weechat_prefix("network"), path);
}

/* ============================================================
 * Stars / bookmarks
 * ============================================================ */

static void
slack_event_stars_list_cb(struct t_weeslack_workspace *workspace,
                           int return_code, const char *output,
                           void *user_data)
{
    struct t_gui_buffer *buffer = user_data;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: stars.list failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (!json)
        return;

    if (slack_api_check_error(workspace, json, "stars.list"))
    {
        json_object_put(json);
        return;
    }

    struct json_object *items;
    int n = 0;
    if (json_object_object_get_ex(json, "items", &items))
    {
        int count = json_object_array_length(items);
        weechat_printf(buffer, "%sweeslack: %d starred items",
                        weechat_prefix("network"), count);
        for (int i = 0; i < count && i < 40; i++)
        {
            struct json_object *it = json_object_array_get_idx(items, i);
            struct json_object *type_obj, *msg_obj, *ch_obj, *ts_obj, *text_obj;
            struct json_object *user_obj;
            const char *type = "?", *ch = "?", *ts = "?", *text = "", *who = "";
            char ch_disp[128];
            char *fmt = NULL, *emoji = NULL, *ment = NULL;
            struct t_slack_channel *sch = NULL;
            struct t_slack_user *su = NULL;

            if (json_object_object_get_ex(it, "type", &type_obj))
                type = json_object_get_string(type_obj);
            if (json_object_object_get_ex(it, "channel", &ch_obj))
            {
                ch = json_object_get_string(ch_obj);
                sch = ch ? slack_channel_search(ch) : NULL;
                if (sch && sch->name && sch->name[0])
                    ch = sch->name;
            }
            if (json_object_object_get_ex(it, "message", &msg_obj))
            {
                if (json_object_object_get_ex(msg_obj, "ts", &ts_obj))
                    ts = json_object_get_string(ts_obj);
                if (json_object_object_get_ex(msg_obj, "text", &text_obj))
                    text = json_object_get_string(text_obj);
                if (json_object_object_get_ex(msg_obj, "user", &user_obj))
                {
                    const char *uid = json_object_get_string(user_obj);
                    su = uid ? slack_user_search(uid) : NULL;
                    who = su ? slack_user_best_name(su) : (uid ? uid : "");
                }
            }

            snprintf(ch_disp, sizeof(ch_disp), "%s%s",
                     (sch && (sch->type == SLACK_CHANNEL_TYPE_DM ||
                              sch->type == SLACK_CHANNEL_TYPE_MPDM))
                         ? "" : "#",
                     ch ? ch : "?");

            fmt = slack_event_render_formatting(text);
            emoji = slack_event_apply_emoji_mode(fmt ? fmt : text);
            ment = slack_event_format_mentions(workspace,
                                               emoji ? emoji : text, sch);

            weechat_printf(buffer, "%s  [%s] %s %s%s%s: %.100s%s",
                            weechat_prefix("network"),
                            type ? type : "?",
                            ch_disp,
                            who && who[0] ? who : "",
                            who && who[0] ? " " : "",
                            ts ? ts : "?",
                            ment ? ment : text,
                            (ment && strlen(ment) > 100) ||
                            (!ment && text && strlen(text) > 100) ? "..." : "");
            free(fmt);
            free(emoji);
            free(ment);
            n++;
        }
        if (count > 40)
            weechat_printf(buffer, "%s  … %d more not shown",
                            weechat_prefix("network"), count - 40);
    }
    if (n == 0)
        weechat_printf(buffer, "%sweeslack: no stars (or no access)",
                        weechat_prefix("network"));
    json_object_put(json);
}

void
slack_event_stars_list(struct t_weeslack_workspace *workspace,
                        struct t_gui_buffer *buffer)
{
    if (!workspace)
        return;
    slack_http_request_new(workspace, "stars.list", NULL,
                           slack_event_stars_list_cb, buffer);
}

void
slack_event_star_message(struct t_weeslack_workspace *workspace,
                          const char *channel_id,
                          const char *timestamp,
                          int add)
{
    if (!workspace || !channel_id || !timestamp)
        return;

    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    json_object_object_add(params, "timestamp",
                           json_object_new_string(timestamp));
    slack_http_request_new(workspace,
                           add ? "stars.add" : "stars.remove",
                           params,
                           slack_event_simple_api_cb,
                           (void *)(add ? "stars.add" : "stars.remove"));
    json_object_put(params);
}

/* ============================================================
 * Join / leave channel
 * ============================================================ */

static void
slack_event_join_cb(struct t_weeslack_workspace *workspace,
                    int return_code, const char *output,
                    void *user_data)
{
    char *name_hint = user_data;
    struct json_object *json, *ch_obj, *id_obj, *name_obj;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: join failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        free(name_hint);
        return;
    }

    json = slack_json_decode(output);
    if (!json)
    {
        free(name_hint);
        return;
    }

    if (slack_api_check_error(workspace, json, "conversations.join"))
    {
        json_object_put(json);
        free(name_hint);
        return;
    }

    if (json_object_object_get_ex(json, "channel", &ch_obj))
    {
        const char *cid = NULL, *cname = NULL;
        enum slack_channel_type ctype = SLACK_CHANNEL_TYPE_CHANNEL;
        struct t_slack_channel *ch;

        if (json_object_object_get_ex(ch_obj, "id", &id_obj))
            cid = json_object_get_string(id_obj);
        if (json_object_object_get_ex(ch_obj, "name", &name_obj))
            cname = json_object_get_string(name_obj);
        if (cid)
        {
            struct json_object *is_priv, *is_im, *is_mpim;
            if (json_object_object_get_ex(ch_obj, "is_im", &is_im) &&
                json_object_get_boolean(is_im))
                ctype = SLACK_CHANNEL_TYPE_DM;
            else if (json_object_object_get_ex(ch_obj, "is_mpim", &is_mpim) &&
                     json_object_get_boolean(is_mpim))
                ctype = SLACK_CHANNEL_TYPE_MPDM;
            else if (json_object_object_get_ex(ch_obj, "is_private", &is_priv) &&
                     json_object_get_boolean(is_priv))
                ctype = SLACK_CHANNEL_TYPE_GROUP;

            ch = slack_channel_search(cid);
            if (!ch)
                ch = slack_channel_new(cid,
                                       cname && cname[0] ? cname : cid,
                                       ctype);
            if (ch)
            {
                slack_channel_update(ch, ch_obj);
                ch->is_member = 1;
                if (!ch->buffer)
                    slack_buffer_new(workspace, ch);
                if (ch->buffer)
                    weechat_buffer_set(ch->buffer, "display", "1");
                SLACK_WS_PRINTF(workspace, "%sweeslack: joined #%s",
                                weechat_prefix("network"),
                                ch->name ? ch->name : cid);
            }
        }
    }
    json_object_put(json);
    free(name_hint);
}

void
slack_event_join_channel(struct t_weeslack_workspace *workspace,
                         const char *name_or_id)
{
    struct json_object *params;
    char *hint;
    const char *arg;

    if (!workspace || !name_or_id || !name_or_id[0])
        return;

    arg = name_or_id;
    if (arg[0] == '#')
        arg++;

    hint = strdup(arg);
    params = json_object_new_object();
    /* conversations.join accepts channel id; name works on some tokens */
    json_object_object_add(params, "channel", json_object_new_string(arg));
    slack_http_request_new(workspace, "conversations.join", params,
                           slack_event_join_cb, hint);
    json_object_put(params);
}

static void
slack_event_leave_cb(struct t_weeslack_workspace *workspace,
                     int return_code, const char *output,
                     void *user_data)
{
    char *channel_id = user_data;
    struct json_object *json;
    struct t_slack_channel *ch;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: leave failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        free(channel_id);
        return;
    }

    json = slack_json_decode(output);
    if (json)
    {
        if (!slack_api_check_error(workspace, json, "conversations.leave"))
        {
            ch = channel_id ? slack_channel_search(channel_id) : NULL;
            if (ch)
            {
                ch->is_member = 0;
                if (ch->buffer)
                {
                    weechat_printf(ch->buffer,
                                    "%sleft channel%s",
                                    weechat_prefix("network"),
                                    weechat_color("reset"));
                    weechat_buffer_set(ch->buffer, "notify", "none");
                }
            }
            SLACK_WS_PRINTF(workspace, "%sweeslack: left %s",
                            weechat_prefix("network"),
                            ch && ch->name ? ch->name
                                : (channel_id ? channel_id : "?"));
        }
        json_object_put(json);
    }
    free(channel_id);
}

void
slack_event_leave_channel(struct t_weeslack_workspace *workspace,
                          const char *channel_id)
{
    struct json_object *params;
    char *id_copy;

    if (!workspace || !channel_id || !channel_id[0])
        return;

    id_copy = strdup(channel_id);
    params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    slack_http_request_new(workspace, "conversations.leave", params,
                           slack_event_leave_cb, id_copy);
    json_object_put(params);
}

void
slack_event_whois(struct t_weeslack_workspace *workspace,
                  const char *name_or_id,
                  struct t_gui_buffer *buffer)
{
    struct t_slack_user *user;
    struct t_gui_buffer *out;
    const char *presence;

    (void) workspace;
    out = buffer ? buffer : NULL;

    if (!name_or_id || !name_or_id[0])
    {
        weechat_printf(out, "%sweeslack: usage: /cslack whois <user>",
                        weechat_prefix("error"));
        return;
    }

    user = slack_user_search(name_or_id);
    if (!user)
    {
        /* try @name / display / real (exact, then case-insensitive substring) */
        struct t_slack_user *u;
        const char *q = name_or_id;
        char qlower[128];
        size_t i;

        if (q[0] == '@')
            q++;
        for (i = 0; q[i] && i + 1 < sizeof(qlower); i++)
        {
            char c = q[i];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            qlower[i] = c;
        }
        qlower[i] = '\0';

        for (u = slack_user_list_global(); u; u = u->next)
        {
            if ((u->name && weechat_strcasecmp(u->name, q) == 0) ||
                (u->display_name && weechat_strcasecmp(u->display_name, q) == 0) ||
                (u->real_name && weechat_strcasecmp(u->real_name, q) == 0) ||
                (u->id && strcmp(u->id, q) == 0))
            {
                user = u;
                break;
            }
        }
        if (!user && qlower[0])
        {
            for (u = slack_user_list_global(); u; u = u->next)
            {
                const char *fields[3];
                int f;
                fields[0] = u->display_name;
                fields[1] = u->real_name;
                fields[2] = u->name;
                for (f = 0; f < 3; f++)
                {
                    const char *s = fields[f];
                    char slow[160];
                    size_t j;
                    if (!s || !s[0])
                        continue;
                    for (j = 0; s[j] && j + 1 < sizeof(slow); j++)
                    {
                        char c = s[j];
                        if (c >= 'A' && c <= 'Z')
                            c = (char)(c - 'A' + 'a');
                        slow[j] = c;
                    }
                    slow[j] = '\0';
                    if (strstr(slow, qlower) != NULL)
                    {
                        user = u;
                        break;
                    }
                }
                if (user)
                    break;
            }
        }
    }

    if (!user)
    {
        weechat_printf(out, "%sweeslack: user not found: %s",
                        weechat_prefix("error"), name_or_id);
        return;
    }

    presence = user->presence ? user->presence : "unknown";
    weechat_printf(out, "%s[%swhois%s] %s%s%s (%s)",
                    weechat_prefix("network"),
                    weechat_color("bold"), weechat_color("reset"),
                    weechat_color(slack_user_get_color(user)),
                    slack_user_best_name(user),
                    weechat_color("reset"),
                    user->id ? user->id : "?");
    if (user->name && user->name[0])
        weechat_printf(out, "%s  handle: @%s",
                        weechat_prefix("network"), user->name);
    if (user->real_name && user->real_name[0])
        weechat_printf(out, "%s  real name: %s",
                        weechat_prefix("network"), user->real_name);
    if (user->display_name && user->display_name[0])
        weechat_printf(out, "%s  display: %s",
                        weechat_prefix("network"), user->display_name);
    weechat_printf(out, "%s  presence: %s",
                    weechat_prefix("network"), presence);
    if (user->status_text && user->status_text[0])
        weechat_printf(out, "%s  status: %s%s%s",
                        weechat_prefix("network"),
                        user->status_emoji ? user->status_emoji : "",
                        user->status_emoji ? " " : "",
                        user->status_text);
    if (user->is_bot)
        weechat_printf(out, "%s  (bot account)",
                        weechat_prefix("network"));
    if (user->is_external)
        weechat_printf(out, "%s  (external / guest)",
                        weechat_prefix("network"));
    if (user->deleted)
        weechat_printf(out, "%s  (deleted)",
                        weechat_prefix("network"));
}
