#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>

#include "weeslack.h"
#include "slack_event.h"
#include "slack_data.h"
#include "slack_buffer.h"
#include "slack_http.h"
#include "slack_ws.h"

static char *slack_event_render_formatting(const char *text);
static char *slack_event_format_files(struct json_object *msg_json);
static char *slack_event_format_attachments(struct json_object *msg_json);
static char *slack_event_apply_emoji_mode(struct t_weeslack_workspace *workspace,
                                          const char *text);
static char *slack_event_format_user(struct t_slack_user *user,
                                     const char *fallback_id,
                                     struct t_slack_channel *channel);
static void slack_event_emoji_cb(struct t_weeslack_workspace *workspace,
                                 int return_code, const char *output,
                                 void *user_data);
static void slack_event_presence_subscribe(struct t_weeslack_workspace *workspace);
static void slack_event_custom_emoji_icat(
    struct t_weeslack_workspace *workspace,
    struct t_gui_buffer *buffer,
    const char *text);
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

/* Custom emoji from emoji.list (name → display replacement), per workspace */
struct t_slack_custom_emoji
{
    char *name;
    char *value; /* unicode, URL, or alias target */
    struct t_weeslack_workspace *workspace;
    struct t_slack_custom_emoji *next;
};

static struct t_slack_custom_emoji *slack_custom_emoji_list = NULL;

/*
 * Standard emoji from weemoji.json (wee-slack file in WeeChat data dir).
 * Separate from workspace custom emoji so emoji.list refresh does not wipe it.
 */
struct t_slack_weemoji
{
    char *name;
    char *unicode;
    struct t_slack_weemoji *next;
};

enum { SLACK_WEEMOJI_BUCKETS = 512 };
static_assert((SLACK_WEEMOJI_BUCKETS & (SLACK_WEEMOJI_BUCKETS - 1)) == 0,
              "weemoji bucket count must be a power of two");
static struct t_slack_weemoji *slack_weemoji_buckets[SLACK_WEEMOJI_BUCKETS];
static int slack_weemoji_count = 0;

static unsigned int
slack_weemoji_hash(const char *name)
{
    unsigned int h = 5381;
    unsigned char c;

    if (!name)
        return 0;
    while ((c = (unsigned char)*name++) != '\0')
        h = ((h << 5) + h) + c;
    return h % SLACK_WEEMOJI_BUCKETS;
}

static void
slack_weemoji_clear(void)
{
    int i;

    for (i = 0; i < SLACK_WEEMOJI_BUCKETS; i++)
    {
        struct t_slack_weemoji *e, *next;
        for (e = slack_weemoji_buckets[i]; e; e = next)
        {
            next = e->next;
            free(e->name);
            free(e->unicode);
            free(e);
        }
        slack_weemoji_buckets[i] = NULL;
    }
    slack_weemoji_count = 0;
}

static void
slack_weemoji_add(const char *name, const char *unicode)
{
    struct t_slack_weemoji *e;
    unsigned int b;

    if (!name || !name[0] || !unicode || !unicode[0])
        return;

    b = slack_weemoji_hash(name);
    for (e = slack_weemoji_buckets[b]; e; e = e->next)
    {
        if (strcmp(e->name, name) == 0)
        {
            free(e->unicode);
            e->unicode = strdup(unicode);
            return;
        }
    }

    e = calloc(1, sizeof(*e));
    if (!e)
        return;
    e->name = strdup(name);
    e->unicode = strdup(unicode);
    if (!e->name || !e->unicode)
    {
        free(e->name);
        free(e->unicode);
        free(e);
        return;
    }
    e->next = slack_weemoji_buckets[b];
    slack_weemoji_buckets[b] = e;
    slack_weemoji_count++;
}

static const char *
slack_weemoji_lookup(const char *name)
{
    struct t_slack_weemoji *e;

    if (!name || !name[0] || slack_weemoji_count <= 0)
        return NULL;
    for (e = slack_weemoji_buckets[slack_weemoji_hash(name)]; e; e = e->next)
    {
        if (strcmp(e->name, name) == 0)
            return e->unicode;
    }
    return NULL;
}

/*
 * Load weemoji.json like wee-slack:
 *   $weechat_data_dir/weemoji.json (or weechat_dir)
 *   else $weechat_sharedir/weemoji.json
 * Silent no-op if missing; keeps static fallback table.
 */
void
slack_event_load_weemoji(void)
{
    const char *data_dir;
    const char *share_dir;
    char path[1024];
    struct json_object *root = NULL;
    struct json_object *old_fmt;
    int path_ok = 0;

    slack_weemoji_clear();

    data_dir = weechat_info_get("weechat_data_dir", "");
    if (!data_dir || !data_dir[0])
        data_dir = weechat_info_get("weechat_dir", "");
    share_dir = weechat_info_get("weechat_sharedir", "");

    if (data_dir && data_dir[0])
    {
        snprintf(path, sizeof(path), "%s/weemoji.json", data_dir);
        if (access(path, R_OK) == 0)
            path_ok = 1;
    }
    if (!path_ok && share_dir && share_dir[0])
    {
        snprintf(path, sizeof(path), "%s/weemoji.json", share_dir);
        if (access(path, R_OK) == 0)
            path_ok = 1;
    }
    if (!path_ok)
        return;

    root = json_object_from_file(path);
    if (!root)
    {
        weechat_printf(NULL,
                       "%sweeslack: failed to parse weemoji.json (%s)",
                       weechat_prefix("error"), path);
        return;
    }

    /* Old wee-slack format was { "emoji": { ... } } */
    if (json_object_object_get_ex(root, "emoji", &old_fmt))
    {
        weechat_printf(NULL,
                       "%sweeslack: weemoji.json is an old format; update it "
                       "(expected name → {unicode, …} map)",
                       weechat_prefix("error"));
        json_object_put(root);
        return;
    }

    if (!json_object_is_type(root, json_type_object))
    {
        weechat_printf(NULL,
                       "%sweeslack: weemoji.json root is not an object",
                       weechat_prefix("error"));
        json_object_put(root);
        return;
    }

    {
        json_object_object_foreach(root, key, val)
        {
            struct json_object *uni_obj, *skins_obj;

            if (!json_object_is_type(val, json_type_object))
                continue;
            if (json_object_object_get_ex(val, "unicode", &uni_obj))
            {
                const char *uni = json_object_get_string(uni_obj);
                if (uni && uni[0])
                    slack_weemoji_add(key, uni);
            }
            if (json_object_object_get_ex(val, "skinVariations", &skins_obj) &&
                json_object_is_type(skins_obj, json_type_object))
            {
                json_object_object_foreach(skins_obj, sk_key, sk_val)
                {
                    struct json_object *sk_name, *sk_uni;
                    const char *sname = NULL, *suni = NULL;

                    (void)sk_key;
                    if (!json_object_is_type(sk_val, json_type_object))
                        continue;
                    if (json_object_object_get_ex(sk_val, "name", &sk_name))
                        sname = json_object_get_string(sk_name);
                    if (json_object_object_get_ex(sk_val, "unicode", &sk_uni))
                        suni = json_object_get_string(sk_uni);
                    if (sname && sname[0] && suni && suni[0])
                        slack_weemoji_add(sname, suni);
                }
            }
        }
    }

    weechat_printf(NULL,
                   "%sweeslack: loaded %d emoji from %s",
                   weechat_prefix("network"),
                   slack_weemoji_count, path);
    json_object_put(root);
}

void
slack_event_unload_weemoji(void)
{
    slack_weemoji_clear();
}

int
slack_event_weemoji_count(void)
{
    return slack_weemoji_count;
}

void
slack_event_weemoji_foreach(void (*cb)(const char *name, const char *unicode,
                                       void *data),
                            void *data)
{
    int i;

    if (!cb)
        return;
    for (i = 0; i < SLACK_WEEMOJI_BUCKETS; i++)
    {
        struct t_slack_weemoji *e;
        for (e = slack_weemoji_buckets[i]; e; e = e->next)
            cb(e->name, e->unicode, data);
    }
}

static void
slack_custom_emoji_clear_workspace(struct t_weeslack_workspace *workspace)
{
    struct t_slack_custom_emoji *e, *next, *prev = NULL;

    for (e = slack_custom_emoji_list; e; e = next)
    {
        next = e->next;
        if (workspace && e->workspace != workspace)
        {
            prev = e;
            continue;
        }
        if (prev)
            prev->next = next;
        else
            slack_custom_emoji_list = next;
        free(e->name);
        free(e->value);
        free(e);
    }
}

static void
slack_custom_emoji_add(struct t_weeslack_workspace *workspace,
                       const char *name, const char *value)
{
    struct t_slack_custom_emoji *e;

    if (!name || !name[0] || !value)
        return;

    e = calloc(1, sizeof(*e));
    if (!e)
        return;
    e->name = strdup(name);
    e->value = strdup(value);
    e->workspace = workspace;
    e->next = slack_custom_emoji_list;
    slack_custom_emoji_list = e;
}

/* Prefer workspace match; fall back to any team (aliases / single-team). */
static const char *
slack_custom_emoji_lookup(struct t_weeslack_workspace *workspace,
                          const char *name)
{
    struct t_slack_custom_emoji *e;
    const char *fallback = NULL;

    for (e = slack_custom_emoji_list; e; e = e->next)
    {
        if (strcmp(e->name, name) != 0)
            continue;
        if (workspace && e->workspace == workspace)
            return e->value;
        if (!fallback)
            fallback = e->value;
    }
    return fallback;
}

void
slack_event_free_workspace_data(struct t_weeslack_workspace *workspace)
{
    if (!workspace)
        return;
    slack_custom_emoji_clear_workspace(workspace);
    slack_user_free_workspace(workspace);
    slack_bot_free_workspace(workspace);
    slack_subteam_free_workspace(workspace);
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
slack_event_replace_emoji(struct t_weeslack_workspace *workspace,
                          const char *text)
{
    if (!text)
        return strdup("");

    size_t len = strlen(text);
    /* unicode replacements can be multi-codepoint (ZWJ sequences) */
    char *result = malloc(len * 8 + 1);
    if (!result)
        return strdup(text);

    const char *src = text;
    char *dst = result;

    while (*src)
    {
        if (*src == ':')
        {
            const char *end = strchr(src + 1, ':');
            /* weemoji names up to ~54 chars; allow skin-tone compound forms */
            if (end && (end - src - 1) > 0 && (end - src - 1) < 64)
            {
                char shortcode[64];
                size_t sc_len = (size_t)(end - src - 1);
                const char *replacement = NULL;
                size_t i;
                int valid = 1;

                memcpy(shortcode, src + 1, sc_len);
                shortcode[sc_len] = '\0';

                /* wee-slack: [a-z0-9_+-]+ (also allow uppercase for safety) */
                for (i = 0; i < sc_len; i++)
                {
                    char c = shortcode[i];
                    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '+' ||
                          c == '-'))
                    {
                        valid = 0;
                        break;
                    }
                }

                if (valid)
                {
                    const char *custom = slack_custom_emoji_lookup(workspace, shortcode);
                    if (custom && custom[0])
                    {
                        /* alias:foo → resolve once; URLs stay as :name: for TUI */
                        if (strncmp(custom, "alias:", 6) == 0)
                        {
                            const char *a = slack_custom_emoji_lookup(workspace, custom + 6);
                            if (!a || !a[0])
                                a = slack_weemoji_lookup(custom + 6);
                            if (a && a[0] && strncmp(a, "http", 4) != 0)
                                custom = a;
                            else
                                custom = NULL;
                        }
                        if (custom && strncmp(custom, "http", 4) != 0)
                            replacement = custom;
                    }
                    if (!replacement)
                        replacement = slack_weemoji_lookup(shortcode);
                    if (!replacement)
                    {
                        const struct slack_emoji_entry *e;
                        for (e = slack_emoji_common; e->name; e++)
                        {
                            if (strcmp(e->name, shortcode) == 0)
                            {
                                replacement = e->unicode;
                                break;
                            }
                        }
                    }
                    if (replacement && replacement[0])
                    {
                        size_t u_len = strlen(replacement);
                        memcpy(dst, replacement, u_len);
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
slack_event_apply_emoji_mode(struct t_weeslack_workspace *workspace,
                             const char *text)
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

    return slack_event_replace_emoji(workspace, text);
}

static char *
slack_event_format_user(struct t_slack_user *user, const char *fallback_id,
                        struct t_slack_channel *channel)
{
    const char *name = "unknown";
    const char *color = "default";
    const char *ext = "";
    char hex_color[16];
    struct t_slack_bot *bot = NULL;
    int is_private = 0;
    int colorize = 1;
    int use_full;

    if (channel &&
        (channel->type == SLACK_CHANNEL_TYPE_DM ||
         channel->type == SLACK_CHANNEL_TYPE_MPDM))
        is_private = 1;

    if (is_private &&
        !weechat_config_boolean(weeslack_config.colorize_private_chats))
        colorize = 0;

    use_full = weechat_config_boolean(weeslack_config.use_full_names);

    if (user)
    {
        if (use_full && user->real_name && user->real_name[0])
            name = user->real_name;
        else if (user->display_name && user->display_name[0])
            name = user->display_name;
        else if (user->real_name && user->real_name[0])
            name = user->real_name;
        else if (user->name && user->name[0])
            name = user->name;
        if (user->is_external)
        {
            ext = weechat_config_string(weeslack_config.external_user_suffix);
            if (!ext)
                ext = "";
        }
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

    char buf[320];
    snprintf(buf, sizeof(buf), "%s%s%s%s",
             weechat_color(color),
             name,
             ext,
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

static void
slack_reaction_build_piece(char *piece, size_t piece_sz,
                           const char *rname, int rcount,
                           char **users, int users_count)
{
    int show_nicks = weechat_config_boolean(weeslack_config.show_reaction_nicks);

    if (show_nicks && users && users_count > 0)
    {
        char nicks[384];
        size_t np = 0;
        int j;

        nicks[0] = '\0';
        for (j = 0; j < users_count && np + 48 < sizeof(nicks); j++)
        {
            struct t_slack_user *u = users[j] ? slack_user_search(users[j])
                                              : NULL;
            const char *nn = u ? slack_user_best_name(u)
                : (users[j] ? users[j] : "?");
            int n = snprintf(nicks + np, sizeof(nicks) - np, "%s%s",
                             np ? ", " : "", nn);
            if (n > 0)
                np += (size_t)n < sizeof(nicks) - np ? (size_t)n : 0;
        }
        if (users_count < rcount && np + 16 < sizeof(nicks))
            snprintf(nicks + np, sizeof(nicks) - np, "%s%s",
                     np ? ", " : "", "and others");
        snprintf(piece, piece_sz, ":%s:(%s)", rname, nicks);
    }
    else
        snprintf(piece, piece_sz, ":%s:%d", rname, rcount);
}

/*
 * Format reactions: " [:name:2 :heart:1]" or with nicks when configured.
 * Own reactions use color_reaction_suffix_added_by_you. Caller frees.
 */
static char *
slack_event_format_reactions(struct t_weeslack_workspace *workspace,
                             struct json_object *msg_json)
{
    struct json_object *reactions_obj;
    size_t cap = 768;
    char *out;
    size_t pos = 0;
    int i, count, any = 0;
    const char *my_id;
    const char *base_col, *mine_col;

    if (!msg_json ||
        !json_object_object_get_ex(msg_json, "reactions", &reactions_obj) ||
        !json_object_is_type(reactions_obj, json_type_array))
        return NULL;

    count = json_object_array_length(reactions_obj);
    if (count <= 0)
        return NULL;

    out = malloc(cap);
    if (!out)
        return NULL;
    out[0] = '\0';

    my_id = (workspace && workspace->my_user_id) ? workspace->my_user_id : NULL;
    base_col = weechat_config_string(weeslack_config.color_reaction_suffix);
    if (!base_col || !base_col[0])
        base_col = "darkgray";
    mine_col = weechat_config_string(
        weeslack_config.color_reaction_suffix_added_by_you);
    if (!mine_col || !mine_col[0])
        mine_col = "lightgreen";

    pos += (size_t)snprintf(out + pos, cap - pos, " %s[",
                            weechat_color(base_col));

    for (i = 0; i < count; i++)
    {
        struct json_object *r = json_object_array_get_idx(reactions_obj, i);
        struct json_object *name_obj, *count_obj, *users_obj;
        const char *rname = NULL;
        int rcount = 0, mine = 0, un = 0, j, written;
        char *uids_stack[32];
        char **uids = NULL;
        char piece[512];

        if (!r)
            continue;
        if (json_object_object_get_ex(r, "name", &name_obj))
            rname = json_object_get_string(name_obj);
        if (json_object_object_get_ex(r, "count", &count_obj))
            rcount = json_object_get_int(count_obj);
        if (!rname || rcount <= 0)
            continue;

        if (json_object_object_get_ex(r, "users", &users_obj) &&
            json_object_is_type(users_obj, json_type_array))
        {
            un = json_object_array_length(users_obj);
            if (un > 32)
                un = 32;
            uids = uids_stack;
            for (j = 0; j < un; j++)
            {
                struct json_object *u = json_object_array_get_idx(users_obj, j);
                const char *uid = json_object_get_string(u);
                uids[j] = (char *)uid;
                if (my_id && uid && strcmp(uid, my_id) == 0)
                    mine = 1;
            }
        }

        slack_reaction_build_piece(piece, sizeof(piece), rname, rcount,
                                   uids, un);

        if (pos + strlen(piece) + 48 >= cap)
        {
            char *n = realloc(out, cap * 2);
            if (!n)
                break;
            out = n;
            cap *= 2;
        }

        if (mine)
            written = snprintf(out + pos, cap - pos, "%s%s%s%s",
                               any ? " " : "",
                               weechat_color(mine_col), piece,
                               weechat_color(base_col));
        else
            written = snprintf(out + pos, cap - pos, "%s%s",
                               any ? " " : "", piece);
        if (written > 0)
            pos += (size_t)written < cap - pos ? (size_t)written : 0;
        any = 1;
    }

    if (!any)
    {
        free(out);
        return NULL;
    }
    if (pos + 8 < cap)
        snprintf(out + pos, cap - pos, "]%s", weechat_color("reset"));
    return out;
}

/* Reactions from the in-memory message model (RTM add/remove). Caller frees. */
static char *
slack_event_format_reactions_msg(struct t_weeslack_workspace *workspace,
                                  struct t_slack_message *msg)
{
    SlackReaction *r;
    size_t cap = 768;
    char *out;
    size_t pos = 0;
    int any = 0;
    const char *my_id;
    const char *base_col, *mine_col;

    if (!msg || !msg->reactions)
        return NULL;

    out = malloc(cap);
    if (!out)
        return NULL;
    out[0] = '\0';
    my_id = (workspace && workspace->my_user_id) ? workspace->my_user_id : NULL;
    base_col = weechat_config_string(weeslack_config.color_reaction_suffix);
    if (!base_col || !base_col[0])
        base_col = "darkgray";
    mine_col = weechat_config_string(
        weeslack_config.color_reaction_suffix_added_by_you);
    if (!mine_col || !mine_col[0])
        mine_col = "lightgreen";

    pos += (size_t)snprintf(out + pos, cap - pos, " %s[",
                            weechat_color(base_col));

    for (r = msg->reactions; r; r = r->next)
    {
        int mine = 0, j, written;
        char piece[512];

        if (!r->name || r->users_count <= 0)
            continue;
        if (my_id)
        {
            for (j = 0; j < r->users_count; j++)
            {
                if (r->users[j] && strcmp(r->users[j], my_id) == 0)
                {
                    mine = 1;
                    break;
                }
            }
        }
        slack_reaction_build_piece(piece, sizeof(piece), r->name,
                                   r->users_count, r->users, r->users_count);

        if (pos + strlen(piece) + 48 >= cap)
        {
            char *n = realloc(out, cap * 2);
            if (!n)
                break;
            out = n;
            cap *= 2;
        }
        if (mine)
            written = snprintf(out + pos, cap - pos, "%s%s%s%s",
                               any ? " " : "",
                               weechat_color(mine_col), piece,
                               weechat_color(base_col));
        else
            written = snprintf(out + pos, cap - pos, "%s%s",
                               any ? " " : "", piece);
        if (written > 0)
            pos += (size_t)written < cap - pos ? (size_t)written : 0;
        any = 1;
    }

    if (!any)
    {
        free(out);
        return NULL;
    }
    if (pos + 8 < cap)
        snprintf(out + pos, cap - pos, "]%s", weechat_color("reset"));
    return out;
}

/*
 * Build message-column text and hdata_update the buffer line for msg->ts.
 * Returns 1 if a line was rewritten.
 */
static int
slack_event_rewrite_message_line(struct t_weeslack_workspace *workspace,
                                  struct t_slack_channel *channel,
                                  struct t_slack_message *msg)
{
    char *formatted_text = NULL;
    char *emoji_text = NULL;
    char *formatted = NULL;
    char *reactions = NULL;
    char *body = NULL;
    size_t body_len;
    int ok = 0;
    const char *edit_color, *del_color;

    if (!channel || !channel->buffer || !msg)
        return 0;

    if (msg->is_deleted)
    {
        del_color = weechat_config_string(weeslack_config.color_deleted);
        if (!del_color || !del_color[0])
            del_color = "red";
        body_len = 64;
        body = malloc(body_len);
        if (!body)
            return 0;
        snprintf(body, body_len, "%s(deleted)%s",
                 weechat_color(del_color), weechat_color("reset"));
        ok = slack_buffer_modify_line(channel->buffer, msg->ts, body);
        free(body);
        return ok;
    }

    formatted_text = slack_event_render_formatting(msg->text ? msg->text : "");
    emoji_text = slack_event_apply_emoji_mode(
        workspace,
        formatted_text ? formatted_text : (msg->text ? msg->text : ""));
    formatted = slack_event_format_mentions(
        workspace, emoji_text ? emoji_text : "", channel);
    reactions = slack_event_format_reactions_msg(workspace, msg);

    body_len = (formatted ? strlen(formatted) : 0) +
               (reactions ? strlen(reactions) : 0) + 64;
    body = malloc(body_len);
    if (!body)
        goto out;

    if (msg->is_edited)
    {
        edit_color = weechat_config_string(weeslack_config.color_edited_suffix);
        if (!edit_color || !edit_color[0])
            edit_color = "yellow";
        snprintf(body, body_len, "%s%s %s(edited)%s",
                 formatted ? formatted : "",
                 reactions ? reactions : "",
                 weechat_color(edit_color),
                 weechat_color("reset"));
    }
    else
    {
        snprintf(body, body_len, "%s%s",
                 formatted ? formatted : "",
                 reactions ? reactions : "");
    }

    ok = slack_buffer_modify_line(channel->buffer, msg->ts, body);

out:
    free(formatted_text);
    free(emoji_text);
    free(formatted);
    free(reactions);
    free(body);
    return ok;
}

/* 1 if text mentions this user (personal highlight). */
static int
slack_event_text_mentions_me(struct t_weeslack_workspace *workspace,
                              const char *text)
{
    char needle[80];
    struct t_slack_user *me;
    const char *dn;

    if (!workspace || !workspace->my_user_id || !text || !text[0])
        return 0;
    snprintf(needle, sizeof(needle), "<@%s", workspace->my_user_id);
    if (strstr(text, needle) != NULL)
        return 1;
    me = slack_user_search(workspace->my_user_id);
    dn = me ? slack_user_best_name(me) : NULL;
    if (dn && dn[0])
    {
        char at[128];
        snprintf(at, sizeof(at), "@%s", dn);
        if (strstr(text, at) != NULL)
            return 1;
    }
    return 0;
}

/* Channel-wide: @channel / @here / @everyone */
static int
slack_event_text_channel_highlight(const char *text)
{
    if (!text)
        return 0;
    if (strstr(text, "<!channel>") || strstr(text, "<!here>") ||
        strstr(text, "<!everyone>"))
        return 1;
    return 0;
}

/* <!subteam^S…> only if we are a member of that usergroup */
static int
slack_event_text_subteam_highlight_me(struct t_weeslack_workspace *workspace,
                                       const char *text)
{
    const char *p;
    struct t_slack_subteam *st;

    if (!text || !workspace)
        return 0;

    for (p = strstr(text, "<!subteam^"); p; p = strstr(p + 1, "<!subteam^"))
    {
        const char *id_start = p + 10; /* after <!subteam^ */
        char sid[64];
        size_t n = 0;

        while (id_start[n] && id_start[n] != '|' && id_start[n] != '>' &&
               n + 1 < sizeof(sid))
        {
            sid[n] = id_start[n];
            n++;
        }
        sid[n] = '\0';
        if (!sid[0])
            continue;
        st = slack_subteam_search(sid);
        if (st && st->is_member)
            return 1;
    }
    return 0;
}

/* Prefs keyword list (case-insensitive substring match). */
static int
slack_event_text_keyword_highlight(struct t_weeslack_workspace *workspace,
                                    const char *text)
{
    char *copy, *tok, *save = NULL;
    int hit = 0;

    if (!workspace || !workspace->highlight_words || !text || !text[0])
        return 0;

    copy = strdup(workspace->highlight_words);
    if (!copy)
        return 0;
    for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
    {
        while (*tok == ' ' || *tok == '\t')
            tok++;
        if (!tok[0])
            continue;
        if (weechat_strcasestr(text, tok))
        {
            hit = 1;
            break;
        }
    }
    free(copy);
    return hit;
}

/* notify_* tags: highlight when message mentions this workspace user. */
static const char *
slack_event_notify_tags(struct t_weeslack_workspace *workspace,
                        struct t_slack_channel *channel,
                        const char *text,
                        int history,
                        SlackTS msg_ts)
{
    int personal, broad, keyword;
    int muted_mode;

    if (history)
    {
        /*
         * wee-slack: backlog = ts <= last_read → no_log + logger_backlog.
         * Unread history (ts > last_read): still silent for hotlist, but
         * keep in WeeChat log (not no_log).
         */
        if (channel && !slack_ts_is_empty(channel->last_read) &&
            !slack_ts_is_empty(msg_ts) &&
            slack_ts_cmp(msg_ts, channel->last_read) <= 0)
            return "no_highlight,notify_none,no_log,logger_backlog";
        return "no_highlight,notify_none,log1,slack_history_unread";
    }

    personal = slack_event_text_mentions_me(workspace, text) ||
               slack_event_text_subteam_highlight_me(workspace, text);
    broad = slack_event_text_channel_highlight(text);
    keyword = slack_event_text_keyword_highlight(workspace, text);

    if (channel && channel->is_muted)
    {
        /* 0=none 1=personal_highlights 2=all_highlights 3=all */
        muted_mode = weechat_config_integer(
            weeslack_config.muted_channels_activity);
        if (muted_mode == 0)
            return "no_highlight,notify_none,log1";
        if (muted_mode == 3)
        {
            if (personal || broad || keyword)
                return "notify_highlight,log1,slack_mention";
            return "notify_message,log1";
        }
        if (muted_mode == 1)
        {
            if (personal || keyword)
                return "notify_highlight,log1,slack_mention";
            return "no_highlight,notify_none,log1";
        }
        /* all_highlights */
        if (personal || broad || keyword)
            return "notify_highlight,log1,slack_mention";
        return "no_highlight,notify_none,log1";
    }

    if (personal || broad || keyword)
        return "notify_highlight,log1,slack_mention";
    return "notify_message,log1";
}

/*
 * Build WeeChat buffer highlight_words: self nick(s), @handles for member
 * usergroups, plus prefs keywords (wee-slack style).
 */
void
slack_event_apply_highlight_words(struct t_weeslack_workspace *workspace)
{
    char buf[2048];
    size_t pos = 0;
    struct t_slack_user *me;
    struct t_slack_subteam *st;
    struct t_slack_channel *ch;
    const char *dn;

    if (!workspace)
        return;

    buf[0] = '\0';

#define HW_APPEND(s) do { \
    const char *_s = (s); \
    size_t _l; \
    if (!_s || !_s[0]) break; \
    _l = strlen(_s); \
    if (pos + _l + 2 >= sizeof(buf)) break; \
    if (pos > 0) buf[pos++] = ','; \
    memcpy(buf + pos, _s, _l); \
    pos += _l; \
    buf[pos] = '\0'; \
} while (0)

    if (workspace->my_user_id)
    {
        me = slack_user_search(workspace->my_user_id);
        dn = me ? slack_user_best_name(me) : NULL;
        if (dn && dn[0])
        {
            char at[256];
            HW_APPEND(dn);
            snprintf(at, sizeof(at), "@%.250s", dn);
            HW_APPEND(at);
        }
        if (me && me->name && me->name[0] &&
            (!dn || strcmp(me->name, dn) != 0))
        {
            char at[256];
            HW_APPEND(me->name);
            snprintf(at, sizeof(at), "@%.250s", me->name);
            HW_APPEND(at);
        }
    }

    for (st = slack_subteam_list_global(); st; st = st->next)
    {
        if (!st->is_member)
            continue;
        if (st->handle && st->handle[0])
        {
            char at[256];
            HW_APPEND(st->handle);
            if (st->handle[0] != '@')
            {
                snprintf(at, sizeof(at), "@%.250s", st->handle);
                HW_APPEND(at);
            }
        }
    }

    if (workspace->highlight_words && workspace->highlight_words[0])
    {
        char *copy = strdup(workspace->highlight_words);
        char *tok, *save = NULL;
        if (copy)
        {
            for (tok = strtok_r(copy, ",", &save); tok;
                 tok = strtok_r(NULL, ",", &save))
            {
                while (*tok == ' ' || *tok == '\t')
                    tok++;
                if (tok[0])
                    HW_APPEND(tok);
            }
            free(copy);
        }
    }

#undef HW_APPEND

    for (ch = slack_channel_list_global(); ch; ch = ch->next)
    {
        if (!ch->buffer)
            continue;
        if (ch->workspace && ch->workspace != workspace)
            continue;
        weechat_buffer_set(ch->buffer, "highlight_words",
                           buf[0] ? buf : "");
    }
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

    /*
     * Live traffic only: reopen a buffer if the user closed it (model kept).
     * History must not recreate buffers for closed channels (rate-limit /
     * noise); use /cslack join|show or wait for a live message.
     */
    if (!history && !channel->buffer && workspace)
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
                /* Prefer in-place rewrite (wee-slack hdata_update). */
                if (!slack_event_rewrite_message_line(workspace, channel, msg) &&
                    channel->buffer)
                {
                    const char *edit_color;
                    char *rendered = slack_event_render_formatting(new_text);
                    char *with_emoji = slack_event_apply_emoji_mode(
                        workspace, rendered ? rendered : new_text);
                    edit_color = weechat_config_string(
                        weeslack_config.color_edited_suffix);
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
            {
                msg->is_deleted = 1;
                if (!slack_event_rewrite_message_line(workspace, channel, msg) &&
                    channel->buffer)
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
        }
        return;
    }

    if (subtype && (strcmp(subtype, "channel_topic") == 0 ||
                    strcmp(subtype, "channel_purpose") == 0))
    {
        struct json_object *topic_obj = NULL;
        const char *field = (strcmp(subtype, "channel_purpose") == 0)
            ? "purpose" : "topic";
        const char *topic = NULL;
        free(text_owned);
        if (json_object_object_get_ex(msg_json, field, &topic_obj) && topic_obj)
        {
            /* RTM may send a plain string or {"value":"..."} */
            if (json_object_is_type(topic_obj, json_type_string))
                topic = json_object_get_string(topic_obj);
            else if (json_object_is_type(topic_obj, json_type_object))
            {
                struct json_object *val;
                if (json_object_object_get_ex(topic_obj, "value", &val))
                    topic = json_object_get_string(val);
            }
        }
        if (!topic || !topic[0])
        {
            /* Fallback: message text after "set the channel topic: " */
            struct json_object *text_obj;
            if (json_object_object_get_ex(msg_json, "text", &text_obj))
                topic = json_object_get_string(text_obj);
        }
        if (topic && topic[0])
        {
            if (strcmp(field, "purpose") == 0)
            {
                free(channel->purpose);
                channel->purpose = strdup(topic);
            }
            else
            {
                free(channel->topic);
                channel->topic = strdup(topic);
            }

            if (channel->buffer)
            {
                const char *disp = slack_channel_display_topic(channel);
                weechat_buffer_set(channel->buffer, "title",
                                   disp ? disp : "");
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
            if (ju)
            {
                struct t_slack_buffer *sb =
                    slack_buffer_search_by_channel(channel->id);
                if (sb)
                    slack_buffer_add_nick(sb, ju);
            }
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
            if (lu)
            {
                struct t_slack_buffer *sb =
                    slack_buffer_search_by_channel(channel->id);
                if (sb)
                    slack_buffer_remove_nick(sb, lu);
            }
        }
        free(text_owned);
        return;
    }

    if (subtype && (strcmp(subtype, "pinned_item") == 0 ||
                    strcmp(subtype, "unpinned_item") == 0))
    {
        int pinned = (strcmp(subtype, "pinned_item") == 0);
        if (channel->buffer)
        {
            struct t_slack_user *pu = user_id ? slack_user_search(user_id) : NULL;
            const char *pname = pu ? slack_user_best_name(pu)
                : (user_id ? user_id : "?");
            weechat_printf(channel->buffer,
                            "%s%s a message%s%s",
                            weechat_prefix("network"),
                            pinned ? "pinned" : "unpinned",
                            pname ? " — " : "",
                            pname ? pname : "");
        }
        free(text_owned);
        return;
    }

    if (subtype && strcmp(subtype, "channel_name") == 0)
    {
        struct json_object *name_obj;
        free(text_owned);
        if (json_object_object_get_ex(msg_json, "name", &name_obj))
        {
            const char *n = json_object_get_string(name_obj);
            if (n && n[0])
            {
                free(channel->name);
                channel->name = strdup(n);
                if (channel->buffer)
                {
                    char short_name[128];
                    snprintf(short_name, sizeof(short_name), "#%s", n);
                    weechat_buffer_set(channel->buffer, "short_name", short_name);
                    weechat_buffer_set(channel->buffer,
                                       "localvar_set_channel", n);
                    weechat_printf(channel->buffer,
                                    "%schannel renamed to #%s",
                                    weechat_prefix("network"), n);
                }
            }
        }
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
         * look.auto_open_threads:
         *   0=off  1=subscribed or @mention  2=all live replies
         * Mode 2 storms buffers + conversations.replies — opt-in only.
         * Explicit: /cslack thread <ts>.
         */
        if (thread && !thread->buffer && !history)
        {
            int mode = weechat_config_integer(
                weeslack_config.auto_open_threads);
            int open_it = 0;
            struct t_slack_message *parent_msg;

            if (mode >= 2)
                open_it = 1;
            else if (mode == 1)
            {
                open_it = thread->is_subscribed;
                if (!open_it && text && text[0] &&
                    slack_event_text_mentions_me(workspace, text))
                    open_it = 1;
                if (!open_it)
                {
                    parent_msg = slack_message_search(
                        channel->messages, slack_ts_new(thread_ts));
                    if (parent_msg && parent_msg->subscribed)
                        open_it = 1;
                }
            }
            if (open_it)
            {
                slack_buffer_new(workspace, thread);
                {
                    struct t_slack_buffer *tb =
                        slack_buffer_search_by_channel(thread->id);
                    if (tb)
                        weechat_buffer_set(tb->buffer, "notify", "highlight");
                }
                slack_event_fetch_replies(workspace, thread);
            }
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
            char *emoji_text = slack_event_apply_emoji_mode(workspace, formatted_text);
            char *formatted = slack_event_format_mentions(workspace, emoji_text, channel);
            char *reactions = slack_event_format_reactions(workspace, msg_json);
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
                     slack_event_notify_tags(workspace, channel, text, history,
                                             ts),
                     ts_tag ? ts_tag : "0");

            /* wee-slack style: [ Thread: $hash Replies: N ] */
            {
                const char *h = msg->hash;
                if (h && h[0])
                {
                    weechat_printf_date_tags(
                        channel->buffer,
                        ts.sec,
                        tags,
                        "%s\t%s%s  %s[ Thread: $%s Replies: %d ]%s",
                        nick_str,
                        formatted ? formatted : "",
                        reactions ? reactions : "",
                        weechat_color(suffix_color),
                        h, rc,
                        weechat_color("reset"));
                }
                else
                {
                    weechat_printf_date_tags(
                        channel->buffer,
                        ts.sec,
                        tags,
                        "%s\t%s%s  %s[%d replies]%s",
                        nick_str,
                        formatted ? formatted : "",
                        reactions ? reactions : "",
                        weechat_color(suffix_color),
                        rc,
                        weechat_color("reset"));
                }
            }

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
            free(reactions);
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
        char *emoji_text = slack_event_apply_emoji_mode(workspace, formatted_text);
        char *formatted = slack_event_format_mentions(workspace, emoji_text, channel);
        char *reactions = slack_event_format_reactions(workspace, msg_json);
        char *files = slack_event_format_files(msg_json);
        char *attachments = slack_event_format_attachments(msg_json);
        char tags[256];
        char *ts_tag = slack_ts_to_string(ts);
        const char *bcast = "";
        char bcast_buf[64];
        char me_buf[768];
        int is_broadcast = (subtype &&
                            strcmp(subtype, "thread_broadcast") == 0);

        if ((thread_ts && !is_parent && display_channel == channel) ||
            is_broadcast)
        {
            const char *pfx = weechat_config_string(
                weeslack_config.thread_broadcast_prefix);
            if (!pfx || !pfx[0])
                pfx = is_broadcast ? "broadcast" : "thread";
            /* Prefix when showing a reply in the parent (config, no open
             * thread buffer, or explicit thread_broadcast). */
            snprintf(bcast_buf, sizeof(bcast_buf), " %s[%s]%s",
                     weechat_color("cyan"), pfx, weechat_color("reset"));
            bcast = bcast_buf;
        }

        /* /me style: * nick does something */
        if (is_me)
        {
            const char *plain_nick = user ? slack_user_best_name(user)
                : (user_id ? user_id : "?");
            snprintf(me_buf, sizeof(me_buf), "%s* %s %s%s%s",
                     weechat_color("magenta"),
                     plain_nick ? plain_nick : "?",
                     formatted ? formatted : "",
                     reactions ? reactions : "",
                     weechat_color("reset"));
        }

        snprintf(tags, sizeof(tags),
                 "%s,slack_ts_%s%s",
                 slack_event_notify_tags(workspace, display_channel, text,
                                         history, ts),
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
                "%s\t%s%s%s",
                nick_str,
                formatted ? formatted : "",
                reactions ? reactions : "",
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

        /* Live only — history would storm the download queue. */
        if (!history && files && files[0])
            slack_event_auto_download_message_files(workspace, channel,
                                                     msg_json,
                                                     display_channel->buffer);

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

        /* Custom emoji images via /icat (live; gated on icat_enabled + /icat). */
        if (!history && text && text[0])
            slack_event_custom_emoji_icat(workspace, display_channel->buffer,
                                          text);

        free(nick_str);
        free(formatted_text);
        free(emoji_text);
        free(formatted);
        free(reactions);
        free(files);
        free(attachments);

        /* unhide_buffers_with_activity (live only, skip muted) */
        if (!history && display_channel->buffer &&
            !display_channel->is_muted &&
            weechat_config_boolean(weeslack_config.unhide_buffers_with_activity))
        {
            if (weechat_buffer_get_integer(display_channel->buffer, "hidden"))
                weechat_buffer_set(display_channel->buffer, "hidden", "0");
        }

        /*
         * notify_subscribed_threads: 0=auto (only if not showing in channel
         * and not auto-opening threads), 1=always, 2=never.
         */
        if (!history && thread_ts && thread_ts[0] && channel &&
            channel->is_subscribed && workspace)
        {
            int mode = weechat_config_integer(
                weeslack_config.notify_subscribed_threads);
            int do_notify = 0;

            if (mode == 1)
                do_notify = 1;
            else if (mode == 0)
            {
                int in_ch = weechat_config_boolean(
                    weeslack_config.thread_messages_in_channel);
                if (!in_ch)
                    do_notify = 1;
            }
            if (do_notify)
            {
                const char *cname = channel->name ? channel->name : channel->id;
                SLACK_WS_PRINTF(
                    workspace,
                    "%ssubscribed thread in #%s: %s",
                    weechat_prefix("network"),
                    cname,
                    text && text[0] ? text : "(reply)");
            }
        }
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
        if (workspace->ws)
            workspace->ws->reconnect_delay = 1;
        SLACK_WS_PRINTF(workspace, "%sweeslack: connected to %s as %s",
                        weechat_prefix("network"),
                        workspace->name,
                        workspace->my_user_id ? workspace->my_user_id : "?");
        /* Subscribe to presence so Here/Away nicklist can update (wee-slack). */
        slack_event_presence_subscribe(workspace);
        return;
    }

    /* Server asks client to disconnect and open a new RTM session */
    if (strcmp(type, "goodbye") == 0)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: server goodbye — reconnecting…",
                        weechat_prefix("network"));
        workspace->connected = 0;
        if (workspace->ws)
            slack_ws_reconnect(workspace->ws);
        return;
    }

    /* Prefer this URL on next reconnect (still re-issue rtm.connect as backup). */
    if (strcmp(type, "reconnect_url") == 0)
    {
        struct json_object *url_obj;
        if (json_object_object_get_ex(json, "url", &url_obj))
        {
            const char *url = json_object_get_string(url_obj);
            if (url && url[0])
            {
                free(workspace->ws_url);
                workspace->ws_url = strdup(url);
            }
        }
        return;
    }

    if (strcmp(type, "error") == 0)
    {
        struct json_object *err_obj, *code_obj, *msg_obj;
        int code = 0;
        const char *emsg = NULL;

        if (json_object_object_get_ex(json, "error", &err_obj))
        {
            if (json_object_object_get_ex(err_obj, "code", &code_obj))
                code = json_object_get_int(code_obj);
            if (json_object_object_get_ex(err_obj, "msg", &msg_obj))
                emsg = json_object_get_string(msg_obj);
        }
        SLACK_WS_PRINTF(workspace, "%sweeslack: RTM error %d: %s",
                        weechat_prefix("error"), code,
                        emsg ? emsg : "(no message)");
        /* fatal-ish codes: force reconnect with fresh rtm.connect */
        if (code == 1 || code == 2 || code == 3)
        {
            workspace->connected = 0;
            if (workspace->ws)
                slack_ws_reconnect(workspace->ws);
        }
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
                slack_channel_set_workspace(channel, workspace);
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

    if (strcmp(type, "presence_change") == 0 ||
        strcmp(type, "manual_presence_change") == 0)
    {
        struct json_object *presence_obj, *user_obj, *users_obj;
        const char *presence = NULL;
        int is_manual = (strcmp(type, "manual_presence_change") == 0);

        if (json_object_object_get_ex(json, "presence", &presence_obj))
            presence = json_object_get_string(presence_obj);
        if (!presence || !presence[0])
            return;

        /* Batch form: { "users": ["U1","U2"], "presence": "active" } */
        if (json_object_object_get_ex(json, "users", &users_obj) &&
            json_object_is_type(users_obj, json_type_array))
        {
            int n = json_object_array_length(users_obj);
            int i;
            for (i = 0; i < n; i++)
            {
                struct json_object *u = json_object_array_get_idx(users_obj, i);
                const char *user_id = json_object_get_string(u);
                struct t_slack_user *user = user_id ? slack_user_search(user_id)
                    : NULL;
                if (!user)
                    continue;
                free(user->presence);
                user->presence = strdup(presence);
                slack_buffer_update_user_presence(user);
                if (workspace->my_user_id && user_id &&
                    strcmp(user_id, workspace->my_user_id) == 0)
                {
                    free(workspace->my_presence);
                    workspace->my_presence = strdup(presence);
                    if (is_manual)
                        workspace->my_manual_away =
                            (strcmp(presence, "away") == 0);
                    weechat_bar_item_update("slack_away");
                }
            }
        }

        /* Single form: { "user": "U1", "presence": "away" } */
        if (json_object_object_get_ex(json, "user", &user_obj))
        {
            const char *user_id = json_object_get_string(user_obj);
            struct t_slack_user *user = user_id ? slack_user_search(user_id)
                : NULL;
            if (user)
            {
                free(user->presence);
                user->presence = strdup(presence);
                slack_buffer_update_user_presence(user);
            }
            if (workspace->my_user_id && user_id &&
                strcmp(user_id, workspace->my_user_id) == 0)
            {
                free(workspace->my_presence);
                workspace->my_presence = strdup(presence);
                if (is_manual)
                    workspace->my_manual_away =
                        (strcmp(presence, "away") == 0);
                weechat_bar_item_update("slack_away");
            }
        }
        return;
    }

    if (strcmp(type, "pref_change") == 0)
    {
        struct json_object *name_obj, *value_obj;
        const char *pref = NULL;
        const char *value = NULL;

        if (json_object_object_get_ex(json, "name", &name_obj))
            pref = json_object_get_string(name_obj);
        if (json_object_object_get_ex(json, "value", &value_obj))
            value = json_object_get_string(value_obj);

        if (pref && value && strcmp(pref, "highlight_words") == 0)
        {
            free(workspace->highlight_words);
            workspace->highlight_words = value[0] ? strdup(value) : NULL;
            slack_event_apply_highlight_words(workspace);
            SLACK_WS_PRINTF(workspace,
                            "%shighlight words updated from Slack prefs",
                            weechat_prefix("network"));
            return;
        }

        if (pref && value && strcmp(pref, "muted_channels") == 0)
        {
            /* Reset all, then apply list (value is comma-separated ids). */
            struct t_slack_channel *ch;
            char *copy, *tok, *save = NULL;
            int muted_n = 0;

            for (ch = slack_channel_list_global(); ch; ch = ch->next)
            {
                if (ch->is_muted)
                {
                    struct t_slack_buffer *sbuf =
                        slack_buffer_search_by_channel(ch->id);
                    ch->is_muted = 0;
                    if (sbuf)
                        slack_buffer_set_muted(sbuf, 0);
                }
            }

            copy = strdup(value);
            if (copy)
            {
                for (tok = strtok_r(copy, ",", &save); tok;
                     tok = strtok_r(NULL, ",", &save))
                {
                    struct t_slack_buffer *sbuf;
                    while (*tok == ' ')
                        tok++;
                    if (!tok[0])
                        continue;
                    ch = slack_channel_search(tok);
                    if (!ch)
                        continue;
                    ch->is_muted = 1;
                    sbuf = slack_buffer_search_by_channel(ch->id);
                    if (sbuf)
                        slack_buffer_set_muted(sbuf, 1);
                    muted_n++;
                }
                free(copy);
            }
            SLACK_WS_PRINTF(workspace,
                            "%sweeslack: mute prefs updated (%d channel%s)",
                            weechat_prefix("network"), muted_n,
                            muted_n == 1 ? "" : "s");
        }
        else if (pref && value && strcmp(pref, "highlight_words") == 0)
        {
            /* Apply highlight words to all channel buffers. */
            struct t_slack_channel *ch;
            for (ch = slack_channel_list_global(); ch; ch = ch->next)
            {
                if (ch->buffer)
                    weechat_buffer_set(ch->buffer, "highlight_words", value);
            }
            SLACK_WS_PRINTF(workspace,
                            "%sweeslack: highlight words updated",
                            weechat_prefix("network"));
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
                char *old_handle = NULL;
                int was_update = (strcmp(type, "subteam_updated") == 0);

                if (!st)
                    st = slack_subteam_new(subteam_id, subteam_id, workspace);

                if (st)
                {
                    struct json_object *name_obj;
                    struct json_object *handle_obj;
                    struct json_object *desc_obj;

                    if (st->handle)
                        old_handle = strdup(st->handle);

                    if (json_object_object_get_ex(subteam_obj, "name", &name_obj))
                    {
                        free(st->name);
                        st->name = strdup(json_object_get_string(name_obj));
                    }
                    if (json_object_object_get_ex(subteam_obj, "handle",
                                                   &handle_obj))
                    {
                        free(st->handle);
                        st->handle = strdup(
                            json_object_get_string(handle_obj));
                    }
                    if (json_object_object_get_ex(subteam_obj, "description",
                                                   &desc_obj))
                    {
                        free(st->description);
                        st->description = strdup(
                            json_object_get_string(desc_obj));
                    }

                    /* users array on RTM subteam update */
                    {
                        struct json_object *users_obj;
                        if (json_object_object_get_ex(subteam_obj, "users",
                                                       &users_obj) &&
                            json_object_is_type(users_obj, json_type_array))
                        {
                            int uc = json_object_array_length(users_obj);
                            char **members = calloc((size_t)uc, sizeof(char *));
                            int j;
                            if (members)
                            {
                                for (j = 0; j < st->members_count; j++)
                                    free(st->members[j]);
                                free(st->members);
                                for (j = 0; j < uc; j++)
                                {
                                    struct json_object *u =
                                        json_object_array_get_idx(users_obj, j);
                                    const char *uid = json_object_get_string(u);
                                    members[j] = uid ? strdup(uid) : NULL;
                                }
                                st->members = members;
                                st->members_count = uc;
                            }
                        }
                    }
                    slack_subteam_set_member_flag(st, workspace->my_user_id);
                    slack_event_apply_highlight_words(workspace);

                    if (was_update &&
                        weechat_config_boolean(
                            weeslack_config.notify_usergroup_handle_updated) &&
                        st->handle &&
                        (!old_handle ||
                         strcmp(old_handle, st->handle) != 0))
                    {
                        SLACK_WS_PRINTF(
                            workspace,
                            "%susergroup handle updated: %s%s%s → @%s",
                            weechat_prefix("network"),
                            old_handle ? "@" : "",
                            old_handle ? old_handle : "(new)",
                            old_handle ? "" : "",
                            st->handle);
                    }
                    free(old_handle);
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

    if (strcmp(type, "thread_subscribed") == 0 ||
        strcmp(type, "thread_unsubscribed") == 0 ||
        strcmp(type, "thread_marked") == 0)
    {
        struct json_object *sub_obj, *ch_obj, *ts_obj, *lr_obj;
        const char *cid = NULL, *tts = NULL, *last_read = NULL;
        int sub = (strcmp(type, "thread_subscribed") == 0);
        int marked = (strcmp(type, "thread_marked") == 0);
        struct t_slack_channel *parent, *thread;
        struct t_slack_message *msg;

        if (json_object_object_get_ex(json, "subscription", &sub_obj))
        {
            if (json_object_object_get_ex(sub_obj, "channel", &ch_obj))
                cid = json_object_get_string(ch_obj);
            if (json_object_object_get_ex(sub_obj, "thread_ts", &ts_obj))
                tts = json_object_get_string(ts_obj);
            if (json_object_object_get_ex(sub_obj, "last_read", &lr_obj))
                last_read = json_object_get_string(lr_obj);
        }
        if (cid && tts)
        {
            parent = slack_channel_search(cid);
            if (parent)
            {
                msg = slack_message_search(parent->messages, slack_ts_new(tts));
                if (msg && !marked)
                    msg->subscribed = sub ? 1 : 0;
            }
            if (parent)
            {
                thread = slack_thread_channel_find(parent, tts);
                if (thread)
                {
                    if (!marked)
                    {
                        thread->is_subscribed = sub ? 1 : 0;
                        if (thread->buffer)
                            weechat_buffer_set(thread->buffer, "notify",
                                               sub ? "highlight" : "none");
                    }
                    if (last_read && last_read[0])
                    {
                        thread->last_read = slack_ts_new(last_read);
                        thread->unread_count = 0;
                        if (thread->buffer)
                            slack_buffer_clear_hotlist(thread->buffer);
                    }
                }
            }
            if (!marked)
            {
                SLACK_WS_PRINTF(workspace,
                                "%sthread %s %s",
                                weechat_prefix("network"),
                                tts,
                                sub ? "subscribed" : "unsubscribed");
            }
        }
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
                    u = slack_user_new(user_id, user_id, workspace);
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
                                          workspace);
                }
                if (user)
                {
                    struct t_slack_channel *ch;
                    char status_title[512];

                    slack_user_update(user, user_obj);
                    slack_buffer_update_user_presence(user);

                    /* wee-slack: mirror status onto the 1:1 DM title */
                    for (ch = slack_channel_list_global(); ch; ch = ch->next)
                    {
                        if (ch->type != SLACK_CHANNEL_TYPE_DM || !ch->buffer ||
                            !ch->user_id || strcmp(ch->user_id, uid) != 0)
                            continue;
                        if (user->status_text && user->status_text[0])
                        {
                            if (user->status_emoji && user->status_emoji[0])
                                snprintf(status_title, sizeof(status_title),
                                         "%s %s", user->status_emoji,
                                         user->status_text);
                            else
                                snprintf(status_title, sizeof(status_title),
                                         "%s", user->status_text);
                        }
                        else
                            status_title[0] = '\0';
                        weechat_buffer_set(ch->buffer, "title", status_title);
                    }
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

    /*
     * Channel lifecycle while connected (wee-slack parity):
     *  - channel_created: model only, is_member=0, no buffer
     *  - channel_joined / group_joined / mpim_joined: member + open buffer
     *  - im_created: model only; im_open opens + hotlist
     *  - im_open: open existing DM buffer (or create from id)
     */
    if (strcmp(type, "channel_created") == 0 ||
        strcmp(type, "channel_joined") == 0 ||
        strcmp(type, "group_joined") == 0 ||
        strcmp(type, "im_created") == 0 ||
        strcmp(type, "im_open") == 0 ||
        strcmp(type, "mpim_joined") == 0)
    {
        struct json_object *channel_obj = NULL;
        enum slack_channel_type ctype = SLACK_CHANNEL_TYPE_CHANNEL;
        const char *cid = NULL, *cname = NULL;
        int open_buffer = 0;
        int is_member = 0;
        int announce_created = 0;

        if (strcmp(type, "im_created") == 0 || strcmp(type, "im_open") == 0)
            ctype = SLACK_CHANNEL_TYPE_DM;
        else if (strcmp(type, "mpim_joined") == 0)
            ctype = SLACK_CHANNEL_TYPE_MPDM;
        else if (strcmp(type, "group_joined") == 0)
            ctype = SLACK_CHANNEL_TYPE_GROUP;

        if (strcmp(type, "channel_joined") == 0 ||
            strcmp(type, "group_joined") == 0 ||
            strcmp(type, "mpim_joined") == 0 ||
            strcmp(type, "im_open") == 0)
        {
            open_buffer = 1;
            is_member = 1;
        }
        else if (strcmp(type, "channel_created") == 0)
            announce_created = 1;
        else if (strcmp(type, "im_created") == 0)
            is_member = 1; /* we own the IM; open deferred to im_open */

        /* channel field may be object (joined/created) or bare id (im_open) */
        if (json_object_object_get_ex(json, "channel", &channel_obj))
        {
            if (json_object_is_type(channel_obj, json_type_string))
            {
                cid = json_object_get_string(channel_obj);
            }
            else if (json_object_is_type(channel_obj, json_type_object))
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
                            slack_channel_set_workspace(ch, workspace);
                    }
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
                        ch->is_member = is_member;
                        if (open_buffer && !ch->buffer)
                            slack_buffer_new(workspace, ch);
                        if (open_buffer && ch->buffer &&
                            strcmp(type, "im_open") == 0)
                            weechat_buffer_set(ch->buffer, "hotlist", "2");
                        if (open_buffer)
                            SLACK_WS_PRINTF(
                                workspace,
                                "%sweeslack: joined %s",
                                weechat_prefix("network"),
                                ch->name ? ch->name : cid);
                        else if (announce_created)
                            SLACK_WS_PRINTF(
                                workspace,
                                "%sweeslack: channel created: %s",
                                weechat_prefix("network"),
                                ch->name ? ch->name : cid);
                    }
                    return;
                }
            }
        }

        /* im_open often only has channel id string */
        if (cid && cid[0])
        {
            struct t_slack_channel *ch = slack_channel_search(cid);
            struct json_object *user_obj;

            if (!ch)
            {
                const char *dm_name = cid;
                if (json_object_object_get_ex(json, "user", &user_obj))
                {
                    const char *peer = json_object_get_string(user_obj);
                    struct t_slack_user *u = peer ? slack_user_search(peer)
                                                 : NULL;
                    if (u)
                        dm_name = slack_user_best_name(u);
                }
                ch = slack_channel_new(cid, dm_name, ctype);
                if (ch)
                    slack_channel_set_workspace(ch, workspace);
                if (ch &&
                    json_object_object_get_ex(json, "user", &user_obj))
                {
                    free(ch->user_id);
                    ch->user_id = strdup(json_object_get_string(user_obj));
                }
            }
            if (ch)
            {
                ch->is_member = is_member;
                if (open_buffer && !ch->buffer)
                    slack_buffer_new(workspace, ch);
                if (open_buffer && ch->buffer &&
                    strcmp(type, "im_open") == 0)
                    weechat_buffer_set(ch->buffer, "hotlist", "2");
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
                                      workspace);
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
        /* In-place suffix update like wee-slack change_message */
        if (slack_event_rewrite_message_line(workspace, channel, msg))
            return;
    }

    /* Fallback notice if the original line was not found (scrolled off / no history) */
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
                int ignore_alt = weechat_config_boolean(
                    weeslack_config.unfurl_ignore_alt_text);

                if (pipe && !ignore_alt)
                {
                    slack_fmt_append(result, &pos, cap, "%s#%.*s%s",
                                     weechat_color("cyan"),
                                     (int)(end - pipe - 1), pipe + 1,
                                     weechat_color("reset"));
                }
                else
                {
                    const char *id_end = pipe ? pipe : end;
                    slack_fmt_append(result, &pos, cap, "%s#%.*s%s",
                                     weechat_color("cyan"),
                                     (int)(id_end - (src + 2)), src + 2,
                                     weechat_color("reset"));
                }
                src = end + 1;
                continue;
            }
        }

        /* URL <http...> or <http...|text> (unfurl_* options) */
        if (src[0] == '<')
        {
            const char *end = strchr(src + 1, '>');
            if (end && (src[1] == 'h' || src[1] == 'm'))
            {
                const char *pipe = memchr(src + 1, '|', end - (src + 1));
                const char *mode = weechat_config_string(
                    weeslack_config.unfurl_auto_link_display);
                int ignore_alt = weechat_config_boolean(
                    weeslack_config.unfurl_ignore_alt_text);

                if (!mode || !mode[0])
                    mode = "both";

                if (pipe && !ignore_alt)
                {
                    int url_len = (int)(pipe - (src + 1));
                    int lab_len = (int)(end - pipe - 1);
                    const char *url = src + 1;
                    const char *lab = pipe + 1;
                    /* "text"/"url" only when label equals bare host path-ish */
                    int same = 0;

                    if (lab_len > 0 && url_len >= lab_len)
                    {
                        /* label matches full URL or URL without scheme:// */
                        if (url_len == lab_len &&
                            strncmp(url, lab, (size_t)lab_len) == 0)
                            same = 1;
                        else
                        {
                            const char *bare = strstr(url, "://");
                            if (bare)
                            {
                                bare += 3;
                                if ((int)strlen(bare) == lab_len &&
                                    strncmp(bare, lab, (size_t)lab_len) == 0)
                                    same = 1;
                            }
                        }
                    }

                    if (same && strcmp(mode, "text") == 0)
                        slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                         weechat_color("cyan"),
                                         lab_len, lab,
                                         weechat_color("reset"));
                    else if (same && strcmp(mode, "url") == 0)
                        slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                         weechat_color("cyan"),
                                         url_len, url,
                                         weechat_color("reset"));
                    else if (strcmp(mode, "url") == 0 && same)
                        slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                         weechat_color("cyan"),
                                         url_len, url,
                                         weechat_color("reset"));
                    else if (same && strcmp(mode, "both") == 0)
                        /* auto-link: still one side is enough when identical */
                        slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                         weechat_color("cyan"),
                                         lab_len, lab,
                                         weechat_color("reset"));
                    else
                        /* distinct label: url (label) unless mode=text */
                        if (strcmp(mode, "text") == 0)
                            slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                             weechat_color("cyan"),
                                             lab_len, lab,
                                             weechat_color("reset"));
                        else if (strcmp(mode, "url") == 0)
                            slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                             weechat_color("cyan"),
                                             url_len, url,
                                             weechat_color("reset"));
                        else
                            slack_fmt_append(result, &pos, cap,
                                             "%s%.*s%s (%.*s)",
                                             weechat_color("cyan"),
                                             url_len, url,
                                             weechat_color("reset"),
                                             lab_len, lab);
                }
                else
                {
                    /* no label, or ignore alt → show URL only */
                    const char *u_end = pipe ? pipe : end;
                    slack_fmt_append(result, &pos, cap, "%s%.*s%s",
                                     weechat_color("cyan"),
                                     (int)(u_end - (src + 1)), src + 1,
                                     weechat_color("reset"));
                }
                src = end + 1;
                continue;
            }
        }

        /* blockquote: > text at line start */
        if (src[0] == '>' &&
            (src == text || src[-1] == '\n') &&
            (src[1] == ' ' || src[1] == '>'))
        {
            const char *line_end = strchr(src, '\n');
            size_t qlen = line_end ? (size_t)(line_end - src) : strlen(src);
            const char *body = src;
            /* skip leading > and spaces */
            while (*body == '>' || *body == ' ')
                body++;
            slack_fmt_append(result, &pos, cap, "%s│ %.*s%s",
                             weechat_color("lightblue"),
                             (int)(src + qlen - body), body,
                             weechat_color("reset"));
            src += qlen;
            continue;
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
    int count;
    size_t buf_size, pos;
    char *result;
    int shown = 0;
    int link_previews;

    int color_mode;

    if (!json_object_object_get_ex(msg_json, "attachments", &att_obj))
        return NULL;

    count = json_object_array_length(att_obj);
    if (count == 0)
        return NULL;

    /* 0=none 1=prefix 2=all */
    color_mode = weechat_config_integer(weeslack_config.colorize_attachments);
    if (color_mode < 0)
        color_mode = 1;

    link_previews = weechat_config_boolean(weeslack_config.link_previews);

    buf_size = (size_t)count * 512;
    result = malloc(buf_size);
    if (!result)
        return NULL;

    result[0] = '\0';
    pos = 0;

    for (int i = 0; i < count; i++)
    {
        struct json_object *att = json_object_array_get_idx(att_obj, i);
        struct json_object *obj;

        const char *title = "";
        const char *title_link = "";
        const char *text = "";
        const char *pretext = "";
        const char *author = "";
        int is_link_preview = 0;
        int written = 0;

        if (!att)
            continue;

        /* Skip URL unfurls when link_previews is off */
        if (!link_previews)
        {
            if (json_object_object_get_ex(att, "from_url", &obj) ||
                json_object_object_get_ex(att, "original_url", &obj) ||
                json_object_object_get_ex(att, "service_url", &obj))
                is_link_preview = 1;
            if (is_link_preview)
                continue;
        }

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
        if (!title)
            title = "";
        if (!title_link)
            title_link = "";
        if (!text)
            text = "";
        if (!pretext)
            pretext = "";
        if (!author)
            author = "";

        if (pos < buf_size - 1)
        {
            const char *c_on = "";
            const char *c_off = "";

            if (color_mode == 2)
            {
                c_on = weechat_color("darkgray");
                c_off = weechat_color("reset");
            }
            else if (color_mode == 1)
            {
                c_on = weechat_color("darkgray");
                c_off = weechat_color("reset");
            }
            /* color_mode 0: plain */

            if (pretext[0])
                written = snprintf(result + pos, buf_size - pos,
                                   "%s%s%s%s",
                                   shown > 0 ? "\n" : "",
                                   c_on, pretext, c_off);
            else if (title[0] && title_link[0])
                written = snprintf(result + pos, buf_size - pos,
                                   "%s%s%s%s%s: %s%s",
                                   shown > 0 ? "\n" : "",
                                   c_on,
                                   author[0] ? author : "",
                                   author[0] ? ": " : "",
                                   title, title_link,
                                   c_off);
            else if (title[0])
                written = snprintf(result + pos, buf_size - pos,
                                   "%s%s%s%s%s%s",
                                   shown > 0 ? "\n" : "",
                                   c_on,
                                   author[0] ? author : "",
                                   author[0] ? ": " : "",
                                   title, c_off);
            else if (text[0])
                written = snprintf(result + pos, buf_size - pos,
                                   "%s%s%s%s",
                                   shown > 0 ? "\n" : "",
                                   color_mode == 2 ? c_on : "",
                                   text,
                                   color_mode == 2 ? c_off : "");

            if (written > 0)
            {
                pos += (size_t)written < buf_size - pos ? (size_t)written : 0;
                shown++;
            }
        }
    }

    if (shown == 0)
    {
        free(result);
        return NULL;
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

struct t_slack_bg_hist_ctx
{
    struct t_weeslack_workspace *workspace;
};

static int
slack_event_background_history_cb(const void *pointer, void *data,
                                   int remaining_calls)
{
    struct t_slack_bg_hist_ctx *ctx = (struct t_slack_bg_hist_ctx *)pointer;
    struct t_weeslack_workspace *ws;
    struct t_slack_channel *ch;
    int queued = 0;

    (void)data;
    (void)remaining_calls;

    if (!ctx)
        return WEECHAT_RC_OK;
    ws = ctx->workspace;
    free(ctx);

    if (!ws || !ws->connected)
        return WEECHAT_RC_OK;
    if (!weechat_config_boolean(weeslack_config.background_load_all_history))
        return WEECHAT_RC_OK;

    /*
     * Slow-queue history for open member channels. Cap per run so huge
     * workspaces do not enqueue hundreds of jobs at once.
     */
    {
        int max_ch = weechat_config_integer(
            weeslack_config.background_history_max);
        if (max_ch <= 0)
            max_ch = 200; /* 0 = soft unlimited */
        if (max_ch > 200)
            max_ch = 200;

        for (ch = slack_channel_list_global(); ch; ch = ch->next)
        {
            if (!ch->buffer || !ch->is_member)
                continue;
            if (ch->workspace && ch->workspace != ws)
                continue;
            if (ch->type == SLACK_CHANNEL_TYPE_THREAD)
                continue;
            if (ch->history_state != 0)
                continue;
            slack_event_fetch_history(ws, ch);
            queued++;
            if (queued >= max_ch)
                break;
        }
    }

    if (queued > 0)
    {
        SLACK_WS_PRINTF(ws,
                        "%sweeslack: background history queued for %d "
                        "channel%s (slow queue)",
                        weechat_prefix("network"), queued,
                        queued == 1 ? "" : "s");
    }
    return WEECHAT_RC_OK;
}

void
slack_event_schedule_background_history(struct t_weeslack_workspace *workspace)
{
    struct t_slack_bg_hist_ctx *ctx;

    if (!workspace)
        return;
    if (!weechat_config_boolean(weeslack_config.background_load_all_history))
        return;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return;
    ctx->workspace = workspace;

    /* After bootstrap quiet (~8s) plus cushion. */
    weechat_hook_timer(9000, 0, 1, &slack_event_background_history_cb,
                        ctx, NULL);
}

/* History: multi-page on the slow queue. Page size from history_fetch_count;
 * max pages from look.history_max_pages (default 5, hard max 20). */
enum
{
    SLACK_HISTORY_PAGE_SIZE         = 100,
    SLACK_HISTORY_MAX_PAGES_DEFAULT = 5,
    SLACK_HISTORY_MAX_PAGES_SOFT    = 20,
    SLACK_MEMBERS_PAGE_SIZE         = 200,
    SLACK_MEMBERS_MAX_PAGES_DEFAULT = 3,
    SLACK_MEMBERS_MAX_PAGES_SOFT    = 500,
    SLACK_MEMBERS_UNLIMITED_PAGES   = 1000000,
    SLACK_MEMBERS_MAX_USERINFO      = 40,
    SLACK_USERS_MAX_PAGES           = 10
};

static_assert(SLACK_HISTORY_PAGE_SIZE > 0, "history page size");
static_assert(SLACK_MEMBERS_PAGE_SIZE > 0, "members page size");
static_assert(SLACK_HISTORY_MAX_PAGES_SOFT >= SLACK_HISTORY_MAX_PAGES_DEFAULT,
              "history soft max covers default");
static_assert(SLACK_MEMBERS_MAX_PAGES_SOFT >= SLACK_MEMBERS_MAX_PAGES_DEFAULT,
              "members soft max covers default");
static_assert(SLACK_MEMBERS_UNLIMITED_PAGES > SLACK_MEMBERS_MAX_PAGES_SOFT,
              "unlimited sentinel must exceed soft max");

static int
slack_event_members_max_pages(void)
{
    int n = weechat_config_integer(weeslack_config.members_max_pages);

    /* 0 = unlimited (paginate until Slack returns no cursor; still slow queue). */
    if (n == 0)
        return SLACK_MEMBERS_UNLIMITED_PAGES;
    if (n < 0)
        return SLACK_MEMBERS_MAX_PAGES_DEFAULT;
    if (n > SLACK_MEMBERS_MAX_PAGES_SOFT)
        return SLACK_MEMBERS_MAX_PAGES_SOFT;
    return n;
}

static int
slack_event_history_max_pages(void)
{
    int n = weechat_config_integer(weeslack_config.history_max_pages);
    if (n < 1)
        return SLACK_HISTORY_MAX_PAGES_DEFAULT;
    if (n > SLACK_HISTORY_MAX_PAGES_SOFT)
        return SLACK_HISTORY_MAX_PAGES_SOFT;
    return n;
}

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
    {
        int page = weechat_config_integer(weeslack_config.history_fetch_count);
        if (page < 1)
            page = SLACK_HISTORY_PAGE_SIZE;
        if (page > 1000)
            page = 1000;
        json_object_object_add(params, "limit",
                               json_object_new_int(page));
    }
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
            ctx->page < slack_event_history_max_pages())
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

    if ((has_more || next_cursor) &&
        ctx->page >= slack_event_history_max_pages())
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

            struct t_slack_user *user = slack_user_new(uid, uname, workspace);
            if (user)
            {
                slack_user_update(user, u_obj);
                /*
                 * wee-slack put is_bot members in team.bots, not team.users.
                 * Keep a SlackUser record for message resolution, but also
                 * register a bot entry and leave nicklist filtering to
                 * slack_user_hide_from_nicklist().
                 */
                if (user->is_bot || user->is_app_user)
                {
                    struct t_slack_bot *bot = slack_bot_new(uid, uname, workspace);
                    if (bot)
                        slack_bot_update(bot, u_obj);
                }
            }
        }
    }

    /* paginate (capped) so large workspaces do not stampede */

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

    /* Drop bots/apps/slackbot that may already be on nicklists */
    slack_buffer_purge_hidden_nicks();

    /* Presence subscription once we know the user directory */
    if (workspace->connected)
        slack_event_presence_subscribe(workspace);

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
                            bid, bname && bname[0] ? bname : bid, workspace);
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

                slack_custom_emoji_clear_workspace(workspace);
                it = json_object_iter_begin(emoji_obj);
                end = json_object_iter_end(emoji_obj);
                while (!json_object_iter_equal(&it, &end))
                {
                    const char *name = json_object_iter_peek_name(&it);
                    struct json_object *val = json_object_iter_peek_value(&it);
                    const char *v = json_object_get_string(val);
                    if (name && v)
                    {
                        slack_custom_emoji_add(workspace, name, v);
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
                gid, gname && gname[0] ? gname : gid, workspace);
            if (st)
            {
                slack_subteam_update(st, g);
                /* API uses "users" array; update also handles "members" */
                {
                    struct json_object *users_obj;
                    if (json_object_object_get_ex(g, "users", &users_obj) &&
                        json_object_is_type(users_obj, json_type_array))
                    {
                        int uc = json_object_array_length(users_obj);
                        char **members = calloc((size_t)uc, sizeof(char *));
                        int j;
                        if (members)
                        {
                            for (j = 0; j < st->members_count; j++)
                                free(st->members[j]);
                            free(st->members);
                            for (j = 0; j < uc; j++)
                            {
                                struct json_object *u =
                                    json_object_array_get_idx(users_obj, j);
                                const char *uid = json_object_get_string(u);
                                members[j] = uid ? strdup(uid) : NULL;
                            }
                            st->members = members;
                            st->members_count = uc;
                        }
                    }
                }
                slack_subteam_set_member_flag(st, workspace->my_user_id);
                loaded++;
            }
        }
    }

    json_object_put(json);

    SLACK_WS_PRINTF(workspace, "%sweeslack: loaded %d user groups",
                    weechat_prefix("network"), loaded);

    slack_event_apply_highlight_words(workspace);
    slack_event_fetch_channels(workspace);
}

void
slack_event_fetch_usergroups(struct t_weeslack_workspace *workspace)
{
    if (!workspace)
        return;

    struct json_object *params = json_object_new_object();
    /* include_users so we can set is_member for highlight_words */
    json_object_object_add(params, "include_users",
                           json_object_new_boolean(1));
    json_object_object_add(params, "include_disabled",
                           json_object_new_boolean(0));

    slack_http_request_new(workspace, "usergroups.list", params,
                           slack_event_usergroups_cb, NULL);
    json_object_put(params);
}

struct t_slack_usergroup_users_ctx
{
    char *handle;
    struct t_gui_buffer *buffer;
};

static void
slack_event_usergroup_users_cb(struct t_weeslack_workspace *workspace,
                                int return_code, const char *output,
                                void *user_data)
{
    struct t_slack_usergroup_users_ctx *ctx = user_data;
    struct json_object *json, *users_obj;
    struct t_gui_buffer *out;
    int i, n, count;

    out = (ctx && ctx->buffer) ? ctx->buffer : NULL;

    if (return_code != 0 || !output)
    {
        weechat_printf(out, "%sweeslack: usergroups.users.list failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        goto done;
    }

    json = slack_json_decode(output);
    if (!json)
        goto done;

    if (slack_api_check_error(workspace, json, "usergroups.users.list"))
    {
        json_object_put(json);
        goto done;
    }

    if (!json_object_object_get_ex(json, "users", &users_obj) ||
        !json_object_is_type(users_obj, json_type_array))
    {
        weechat_printf(out, "%sweeslack: usergroup %s: no users",
                        weechat_prefix("network"),
                        (ctx && ctx->handle) ? ctx->handle : "?");
        json_object_put(json);
        goto done;
    }

    count = json_object_array_length(users_obj);
    weechat_printf(out, "%susergroup %s%s%s — %d member%s:",
                    weechat_prefix("network"),
                    weechat_color("green"),
                    (ctx && ctx->handle) ? ctx->handle : "?",
                    weechat_color("reset"),
                    count, count == 1 ? "" : "s");

    n = 0;
    for (i = 0; i < count; i++)
    {
        struct json_object *uid_obj = json_object_array_get_idx(users_obj, i);
        const char *uid;
        struct t_slack_user *user;
        const char *disp;

        if (!uid_obj)
            continue;
        uid = json_object_get_string(uid_obj);
        if (!uid || !uid[0])
            continue;
        user = slack_user_search(uid);
        disp = user ? slack_user_best_name(user) : uid;
        weechat_printf(out, "  %s%s%s (%s)",
                        weechat_color("cyan"),
                        disp ? disp : uid,
                        weechat_color("reset"),
                        uid);
        n++;
    }
    if (n == 0)
        weechat_printf(out, "  (empty)");

    json_object_put(json);

done:
    if (ctx)
    {
        free(ctx->handle);
        free(ctx);
    }
}

void
slack_event_usergroup_list_users(struct t_weeslack_workspace *workspace,
                                  const char *handle_or_id,
                                  struct t_gui_buffer *buffer)
{
    struct t_slack_subteam *st;
    struct t_slack_usergroup_users_ctx *ctx;
    const char *key;
    char handle_buf[128];
    const char *ug_id = NULL;
    struct json_object *params;

    if (!workspace || !handle_or_id || !handle_or_id[0])
        return;

    key = handle_or_id;
    if (key[0] == '@')
        key++;

    /* Accept S… id or @handle / handle */
    if (key[0] == 'S')
        ug_id = key;
    else
    {
        for (st = slack_subteam_list_global(); st; st = st->next)
        {
            if (st->handle && weechat_strcasecmp(st->handle, key) == 0)
            {
                ug_id = st->id;
                break;
            }
            if (st->name && weechat_strcasecmp(st->name, key) == 0)
            {
                ug_id = st->id;
                break;
            }
        }
    }

    if (!ug_id || !ug_id[0])
    {
        weechat_printf(buffer, "%sweeslack: unknown usergroup: %s",
                        weechat_prefix("error"), handle_or_id);
        return;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return;
    if (handle_or_id[0] == '@')
        snprintf(handle_buf, sizeof(handle_buf), "%s", handle_or_id);
    else if (key[0] != 'S')
        snprintf(handle_buf, sizeof(handle_buf), "@%s", key);
    else
        snprintf(handle_buf, sizeof(handle_buf), "%s", key);
    ctx->handle = strdup(handle_buf);
    ctx->buffer = buffer;

    params = json_object_new_object();
    if (!params)
    {
        free(ctx->handle);
        free(ctx);
        return;
    }
    json_object_object_add(params, "usergroup",
                           json_object_new_string(ug_id));
    slack_http_request_new(workspace, "usergroups.users.list", params,
                           slack_event_usergroup_users_cb, ctx);
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
    int unknown_lookups; /* users.info calls this members fetch (capped) */
};

/* Cap users.info for unknown member ids (wee-slack style, rate-limit safe). */


struct t_slack_userinfo_ctx
{
    char *channel_id;
    char *user_id;
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

/*
 * Like wee-slack: only nicklist people in the known human user map.
 * Unknown member ids → users.info (slow queue); bots never get a nick.
 */
static void
slack_event_userinfo_cb(struct t_weeslack_workspace *workspace,
                        int return_code, const char *output,
                        void *user_data)
{
    struct t_slack_userinfo_ctx *ctx = user_data;
    struct json_object *json, *user_obj;
    struct t_slack_user *user;
    struct t_slack_buffer *sbuf;

    if (!ctx)
        return;

    if (return_code == 0 && output)
    {
        json = slack_json_decode(output);
        if (json && !slack_api_check_error(workspace, json, "users.info"))
        {
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
                    user = slack_user_new(uid,
                                         uname && uname[0] ? uname : uid,
                                         workspace);
                    if (user)
                    {
                        slack_user_update(user, user_obj);
                        /* wee-slack: is_bot → bots map, not users/nicklist */
                        if (user->is_bot || user->is_app_user)
                        {
                            struct t_slack_bot *bot =
                                slack_bot_new(uid,
                                              uname && uname[0] ? uname : uid,
                                              workspace);
                            if (bot)
                                slack_bot_update(bot, user_obj);
                        }
                        else if (ctx->channel_id)
                        {
                            sbuf = slack_buffer_search_by_channel(
                                ctx->channel_id);
                            if (sbuf)
                                slack_buffer_add_nick(sbuf, user);
                        }
                    }
                }
            }
        }
        if (json)
            json_object_put(json);
    }

    free(ctx->channel_id);
    free(ctx->user_id);
    free(ctx);
}

static void
slack_event_members_request_userinfo(struct t_weeslack_workspace *workspace,
                                     const char *channel_id,
                                     const char *user_id)
{
    struct t_slack_userinfo_ctx *ctx;
    struct json_object *params;

    if (!workspace || !user_id || !user_id[0])
        return;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return;
    ctx->channel_id = channel_id ? strdup(channel_id) : NULL;
    ctx->user_id = strdup(user_id);
    if (!ctx->user_id)
    {
        free(ctx->channel_id);
        free(ctx);
        return;
    }

    params = json_object_new_object();
    json_object_object_add(params, "user", json_object_new_string(user_id));
    /* slow lane — same as members/history, avoids stampede */
    if (!slack_http_request_new_flags(workspace, "users.info", params,
                                      SLACK_HTTP_SLOW,
                                      slack_event_userinfo_cb, ctx))
    {
        free(ctx->channel_id);
        free(ctx->user_id);
        free(ctx);
    }
    json_object_put(params);
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

            /*
             * wee-slack: nicklist only from team.users (humans). Unknown ids
             * get users.info; is_bot accounts are stored as bots and never
             * nicklisted. Do not invent stubs for the roster.
             */
            user = slack_user_search(uid);
            if (!user)
            {
                if (ctx->unknown_lookups < SLACK_MEMBERS_MAX_USERINFO)
                {
                    slack_event_members_request_userinfo(workspace,
                                                        channel->id, uid);
                    ctx->unknown_lookups++;
                }
                continue;
            }
            if (slack_user_hide_from_nicklist(user))
                continue;

            slack_buffer_add_nick(sbuf, user);
            ctx->total++;
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

    if (next_cursor && ctx->page < slack_event_members_max_pages())
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

void
slack_event_refresh_members(struct t_weeslack_workspace *workspace,
                            struct t_slack_channel *channel)
{
    struct t_slack_buffer *sbuf;

    if (!workspace || !channel)
        return;

    channel->members_loaded = 0;
    sbuf = slack_buffer_search_by_channel(channel->id);
    if (!sbuf && channel->buffer)
        sbuf = slack_buffer_search(channel->buffer);
    if (sbuf)
        slack_buffer_clear_nicks(sbuf);

    slack_event_fetch_members(workspace, channel);
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
                slack_channel_set_workspace(channel, workspace);
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
        /* Optional: queue history after quiet (look.background_load_all_history) */
        slack_event_schedule_background_history(workspace);
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

    /* After all channel pages: apply Slack mute prefs to buffers */
    slack_event_load_mute_prefs(workspace);
}

static void
slack_event_channel_info_cb(struct t_weeslack_workspace *workspace,
                             int return_code, const char *output,
                             void *user_data)
{
    char *channel_id = user_data;
    struct t_slack_channel *channel;
    struct json_object *json, *ch_obj;

    if (!channel_id)
        return;

    channel = slack_channel_search(channel_id);
    free(channel_id);

    if (return_code != 0 || !output || !channel)
        return;

    json = slack_json_decode(output);
    if (!json)
        return;

    if (slack_api_check_error(workspace, json, "conversations.info"))
    {
        json_object_put(json);
        return;
    }

    channel->info_fetched = 1;
    if (json_object_object_get_ex(json, "channel", &ch_obj))
        slack_channel_update(channel, ch_obj);

    json_object_put(json);
}

void
slack_event_fetch_channel_info(struct t_weeslack_workspace *workspace,
                                struct t_slack_channel *channel)
{
    struct json_object *params;
    char *id_copy;

    if (!workspace || !channel || !channel->id)
        return;
    if (channel->info_fetched)
        return;
    if (channel->type == SLACK_CHANNEL_TYPE_THREAD)
        return;

    /* Mark early so buffer_switch does not stampede info requests */
    channel->info_fetched = 1;

    id_copy = strdup(channel->id);
    if (!id_copy)
        return;

    params = json_object_new_object();
    if (!params)
    {
        free(id_copy);
        channel->info_fetched = 0;
        return;
    }
    json_object_object_add(params, "channel",
                           json_object_new_string(channel->id));
    /* slow queue — not urgent relative to history */
    slack_http_request_new_flags(workspace, "conversations.info", params,
                                  SLACK_HTTP_SLOW,
                                  slack_event_channel_info_cb, id_copy);
    json_object_put(params);
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

/* Map WeeChat underline (0x1f) to Slack marker (default _), strip IRC bold. */
static char *
slack_event_map_outgoing_format(const char *text)
{
    const char *map;
    char *out;
    size_t i, j, len;

    if (!text)
        return strdup("");
    len = strlen(text);
    out = malloc(len + 1);
    if (!out)
        return NULL;
    map = weechat_config_string(weeslack_config.map_underline_to);
    if (!map || !map[0])
        map = "_";
    for (i = 0, j = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)text[i];
        if (c == 0x1f) /* underline → Slack italic/bold marker */
            out[j++] = map[0];
        else if (c == 0x02) /* bold → * */
            out[j++] = '*';
        else if (c == 0x1d) /* italic → _ */
            out[j++] = '_';
        else
            out[j++] = (char)c;
    }
    out[j] = '\0';
    return out;
}

void
slack_event_send_message(struct t_weeslack_workspace *workspace,
                          const char *channel_id,
                          const char *text,
                          const char *thread_ts)
{
    struct json_object *params;
    char *mapped;

    if (!workspace || !channel_id || !text || !text[0])
        return;

    mapped = slack_event_map_outgoing_format(text);
    params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    json_object_object_add(params, "text",
                           json_object_new_string(mapped ? mapped : text));

    if (thread_ts && thread_ts[0])
    {
        json_object_object_add(params, "thread_ts",
                               json_object_new_string(thread_ts));
    }

    slack_http_request_new(workspace, "chat.postMessage", params,
                           slack_event_send_message_cb, NULL);

    json_object_put(params);
    free(mapped);
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

static void
slack_event_upload_put_done(void *user_data, int ok, long http_code)
{
    struct t_slack_upload_ctx *ctx = user_data;
    char files_json[512];
    struct json_object *params;

    if (!ctx)
        return;

    if (!ok)
    {
        SLACK_WS_PRINTF(ctx->workspace,
                        "%sweeslack: file PUT failed (HTTP %ld)",
                        weechat_prefix("error"), http_code);
        slack_event_upload_ctx_free(ctx);
        return;
    }

    params = json_object_new_object();
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

static void
slack_event_upload_url_cb(struct t_weeslack_workspace *workspace,
                           int return_code, const char *output,
                           void *user_data)
{
    struct t_slack_upload_ctx *ctx = user_data;

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

    /* Binary PUT via libcurl multi (async; no external curl CLI). */
    if (!slack_http_curl_put_file(ctx->upload_url, ctx->file_path, NULL,
                                  slack_event_upload_put_done, ctx))
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: could not start file PUT",
                        weechat_prefix("error"));
        slack_event_upload_ctx_free(ctx);
    }
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
    char *peer_id;     /* single-peer id, or NULL for mpim */
    char *peer_name;   /* buffer display name */
    int is_mpdm;
};

static void
slack_event_open_dm_ctx_free(struct t_slack_open_dm_ctx *ctx)
{
    if (!ctx)
        return;
    free(ctx->peer_id);
    free(ctx->peer_name);
    free(ctx);
}

static void
slack_event_open_dm_http_cb(struct t_weeslack_workspace *workspace,
                             int return_code, const char *output,
                             void *user_data)
{
    struct t_slack_open_dm_ctx *ctx = user_data;
    enum slack_channel_type ctype;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: open DM failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        slack_event_open_dm_ctx_free(ctx);
        return;
    }

    struct json_object *json = slack_json_decode(output);
    if (!json)
    {
        slack_event_open_dm_ctx_free(ctx);
        return;
    }

    if (slack_api_check_error(workspace, json, "conversations.open"))
    {
        json_object_put(json);
        slack_event_open_dm_ctx_free(ctx);
        return;
    }

    ctype = (ctx && ctx->is_mpdm) ? SLACK_CHANNEL_TYPE_MPDM
                                  : SLACK_CHANNEL_TYPE_DM;

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
                channel = slack_channel_new(channel_id, name, ctype);
                if (channel)
                    slack_channel_set_workspace(channel, workspace);
                if (channel && ctx && ctx->peer_id && !ctx->is_mpdm)
                {
                    free(channel->user_id);
                    channel->user_id = strdup(ctx->peer_id);
                }
                if (channel && workspace)
                    slack_buffer_new(workspace, channel);
            }
            else if (channel->buffer == NULL && workspace)
            {
                slack_buffer_new(workspace, channel);
            }
            if (channel && channel->buffer &&
                weechat_config_boolean(weeslack_config.switch_buffer_on_join))
                weechat_buffer_set(channel->buffer, "display", "1");
        }
    }

    json_object_put(json);
    slack_event_open_dm_ctx_free(ctx);
}

/*
 * Open a 1:1 DM or multi-party IM.
 * user_spec: "alice", "U123", or "alice,bob,@carol" (comma-separated).
 */
void
slack_event_open_dm(struct t_weeslack_workspace *workspace,
                     const char *user_spec)
{
    struct t_slack_open_dm_ctx *ctx;
    char *copy, *save, *tok;
    char users_csv[1024];
    char names_buf[512];
    size_t csv_len = 0, names_len = 0;
    int n_users = 0;
    char *first_id = NULL;

    if (!workspace || !user_spec || !user_spec[0])
        return;

    copy = strdup(user_spec);
    if (!copy)
        return;

    users_csv[0] = '\0';
    names_buf[0] = '\0';

    for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
    {
        struct t_slack_user *user;
        const char *uid, *disp;
        size_t ulen, dlen;

        while (*tok == ' ' || *tok == '\t' || *tok == '@')
            tok++;
        if (!tok[0])
            continue;

        user = slack_user_search_name_ws(workspace, tok);
        if (!user)
            user = slack_user_search_ws(workspace, tok);
        uid = user ? user->id : tok;
        disp = user ? slack_user_best_name(user) : tok;
        if (!uid || !uid[0])
            continue;

        ulen = strlen(uid);
        if (csv_len + ulen + 2 >= sizeof(users_csv))
            break;
        if (csv_len > 0)
            users_csv[csv_len++] = ',';
        memcpy(users_csv + csv_len, uid, ulen);
        csv_len += ulen;
        users_csv[csv_len] = '\0';

        dlen = strlen(disp);
        if (names_len + dlen + 2 < sizeof(names_buf))
        {
            if (names_len > 0)
                names_buf[names_len++] = ',';
            memcpy(names_buf + names_len, disp, dlen);
            names_len += dlen;
            names_buf[names_len] = '\0';
        }

        if (!first_id)
            first_id = strdup(uid);
        n_users++;
    }
    free(copy);

    if (n_users == 0)
    {
        free(first_id);
        SLACK_WS_PRINTF(workspace, "%sweeslack: no users to open DM with",
                        weechat_prefix("error"));
        return;
    }

    /* Include self in MPDM members (Slack expects it). */
    if (n_users > 1 && workspace->my_user_id && workspace->my_user_id[0])
    {
        const char *me = workspace->my_user_id;
        if (!strstr(users_csv, me))
        {
            size_t mlen = strlen(me);
            if (csv_len + mlen + 2 < sizeof(users_csv))
            {
                users_csv[csv_len++] = ',';
                memcpy(users_csv + csv_len, me, mlen);
                csv_len += mlen;
                users_csv[csv_len] = '\0';
            }
        }
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
    {
        free(first_id);
        return;
    }
    ctx->is_mpdm = (n_users > 1) ? 1 : 0;
    ctx->peer_id = first_id;
    ctx->peer_name = strdup(names_buf[0] ? names_buf
                                         : (first_id ? first_id : "dm"));
    if (!ctx->peer_name)
    {
        slack_event_open_dm_ctx_free(ctx);
        return;
    }

    {
        struct json_object *params = json_object_new_object();
        json_object_object_add(params, "users",
                               json_object_new_string(users_csv));
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
        if (!slack_api_check_error(workspace, json, ctx))
        {
            SLACK_WS_PRINTF(workspace, "%sweeslack: %s ok",
                            weechat_prefix("network"), ctx);
        }
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

void
slack_event_set_profile_status(struct t_weeslack_workspace *workspace,
                                const char *status_emoji,
                                const char *status_text)
{
    struct json_object *params, *profile;
    char emoji_buf[64];
    const char *emoji = status_emoji ? status_emoji : "";
    const char *text = status_text ? status_text : "";

    if (!workspace)
        return;

    /* Accept :name: or name */
    if (emoji[0] == ':' && emoji[1])
    {
        size_t n = strlen(emoji);
        if (n >= 3 && emoji[n - 1] == ':' && n - 2 < sizeof(emoji_buf))
        {
            memcpy(emoji_buf, emoji + 1, n - 2);
            emoji_buf[n - 2] = '\0';
            emoji = emoji_buf;
        }
    }

    profile = json_object_new_object();
    if (!profile)
        return;
    json_object_object_add(profile, "status_emoji",
                           json_object_new_string(emoji));
    json_object_object_add(profile, "status_text",
                           json_object_new_string(text));

    params = json_object_new_object();
    if (!params)
    {
        json_object_put(profile);
        return;
    }
    /* form-encoded as profile={...JSON...} */
    json_object_object_add(params, "profile", profile);
    slack_http_request_new(workspace, "users.profile.set", params,
                           slack_event_simple_api_cb,
                           (void *)"users.profile.set");
    json_object_put(params);
}

static void
slack_event_create_channel_cb(struct t_weeslack_workspace *workspace,
                               int return_code, const char *output,
                               void *user_data)
{
    char *name_hint = user_data;
    struct json_object *json, *ch_obj, *id_obj, *name_obj;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: create channel failed (rc=%d)",
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

    if (slack_api_check_error(workspace, json, "conversations.create"))
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
        struct json_object *is_priv;

        if (json_object_object_get_ex(ch_obj, "id", &id_obj))
            cid = json_object_get_string(id_obj);
        if (json_object_object_get_ex(ch_obj, "name", &name_obj))
            cname = json_object_get_string(name_obj);
        if (json_object_object_get_ex(ch_obj, "is_private", &is_priv) &&
            json_object_get_boolean(is_priv))
            ctype = SLACK_CHANNEL_TYPE_GROUP;

        if (cid)
        {
            ch = slack_channel_search(cid);
            if (!ch)
                ch = slack_channel_new(cid,
                                       cname && cname[0] ? cname : cid,
                                       ctype);
            if (ch)
            {
                slack_channel_set_workspace(ch, workspace);
                slack_channel_update(ch, ch_obj);
                ch->is_member = 1;
                if (!ch->buffer)
                    slack_buffer_new(workspace, ch);
                if (ch->buffer &&
                    weechat_config_boolean(weeslack_config.switch_buffer_on_join))
                    weechat_buffer_set(ch->buffer, "display", "1");
                SLACK_WS_PRINTF(workspace, "%sweeslack: created #%s",
                                weechat_prefix("network"),
                                ch->name ? ch->name : cid);
            }
        }
    }
    else
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: created channel %s",
                        weechat_prefix("network"),
                        name_hint ? name_hint : "");
    }

    json_object_put(json);
    free(name_hint);
}

void
slack_event_create_channel(struct t_weeslack_workspace *workspace,
                            const char *name, int is_private)
{
    struct json_object *params;
    const char *arg;
    char *hint;

    if (!workspace || !name || !name[0])
        return;

    arg = name;
    if (arg[0] == '#')
        arg++;

    hint = strdup(arg);
    params = json_object_new_object();
    json_object_object_add(params, "name", json_object_new_string(arg));
    json_object_object_add(params, "is_private",
                           json_object_new_boolean(is_private ? 1 : 0));
    slack_http_request_new(workspace, "conversations.create", params,
                           slack_event_create_channel_cb, hint);
    json_object_put(params);
}

void
slack_event_invite_user(struct t_weeslack_workspace *workspace,
                         const char *channel_id,
                         const char *user_id_or_name)
{
    struct json_object *params;
    struct t_slack_user *user;
    const char *uid;

    if (!workspace || !channel_id || !channel_id[0] ||
        !user_id_or_name || !user_id_or_name[0])
        return;

    uid = user_id_or_name;
    if (uid[0] == '@')
        uid++;

    /* Resolve nick → user id when not already a U… id */
    if (!(uid[0] == 'U' || uid[0] == 'W'))
    {
        user = slack_user_search_name_ws(workspace, uid);
        if (!user)
        {
            SLACK_WS_PRINTF(workspace, "%sweeslack: user not found: %s",
                            weechat_prefix("error"), user_id_or_name);
            return;
        }
        uid = user->id;
    }

    params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    json_object_object_add(params, "users",
                           json_object_new_string(uid));
    slack_http_request_new(workspace, "conversations.invite", params,
                           slack_event_simple_api_cb,
                           (void *)"conversations.invite");
    json_object_put(params);
}

void
slack_event_send_me_message(struct t_weeslack_workspace *workspace,
                             const char *channel_id,
                             const char *text,
                             const char *thread_ts)
{
    struct json_object *params;

    if (!workspace || !channel_id || !text || !text[0])
        return;

    params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    json_object_object_add(params, "text",
                           json_object_new_string(text));
    if (thread_ts && thread_ts[0])
    {
        json_object_object_add(params, "thread_ts",
                               json_object_new_string(thread_ts));
    }
    slack_http_request_new(workspace, "chat.meMessage", params,
                           slack_event_send_message_cb, NULL);
    json_object_put(params);
}

void
slack_event_update_message(struct t_weeslack_workspace *workspace,
                            const char *channel_id,
                            const char *timestamp,
                            const char *text)
{
    struct json_object *params;

    if (!workspace || !channel_id || !timestamp || !text)
        return;

    params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    json_object_object_add(params, "ts",
                           json_object_new_string(timestamp));
    json_object_object_add(params, "text",
                           json_object_new_string(text));
    slack_http_request_new(workspace, "chat.update", params,
                           slack_event_simple_api_cb, (void *)"chat.update");
    json_object_put(params);
}

void
slack_event_delete_message(struct t_weeslack_workspace *workspace,
                            const char *channel_id,
                            const char *timestamp)
{
    struct json_object *params;

    if (!workspace || !channel_id || !timestamp)
        return;

    params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    json_object_object_add(params, "ts",
                           json_object_new_string(timestamp));
    slack_http_request_new(workspace, "chat.delete", params,
                           slack_event_simple_api_cb, (void *)"chat.delete");
    json_object_put(params);
}

void
slack_event_slash_command(struct t_weeslack_workspace *workspace,
                           const char *channel_id,
                           const char *command,
                           const char *text)
{
    struct json_object *params;
    char cmd_buf[128];
    const char *cmd;

    if (!workspace || !channel_id || !command || !command[0])
        return;

    if (command[0] == '/')
        cmd = command;
    else
    {
        snprintf(cmd_buf, sizeof(cmd_buf), "/%s", command);
        cmd = cmd_buf;
    }

    params = json_object_new_object();
    if (!params)
        return;
    json_object_object_add(params, "command", json_object_new_string(cmd));
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    json_object_object_add(params, "text",
                           json_object_new_string(text ? text : ""));
    slack_http_request_new(workspace, "chat.command", params,
                           slack_event_simple_api_cb, (void *)"chat.command");
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
 * Subscribe to presence for Here/Away nicklist (RTM presence_sub).
 * Cap at 750 ids like wee-slack (API JSON size limit).
 */
static void
slack_event_presence_subscribe(struct t_weeslack_workspace *workspace)
{
    struct json_object *msg, *ids;
    struct t_slack_user *u;
    int n = 0;
    const int max_ids = 750;

    if (!workspace || !workspace->ws || !workspace->ws->connected ||
        !workspace->ws->handshake_done)
        return;

    msg = json_object_new_object();
    ids = json_object_new_array();
    if (!msg || !ids)
    {
        if (msg)
            json_object_put(msg);
        if (ids)
            json_object_put(ids);
        return;
    }

    json_object_object_add(msg, "type", json_object_new_string("presence_sub"));

    for (u = slack_user_list_global(); u && n < max_ids; u = u->next)
    {
        if (!u->id || !u->id[0])
            continue;
        if (slack_user_hide_from_nicklist(u))
            continue;
        json_object_array_add(ids, json_object_new_string(u->id));
        n++;
    }

    /* Always include self */
    if (workspace->my_user_id && workspace->my_user_id[0])
    {
        int has_self = 0;
        int i;
        for (i = 0; i < n; i++)
        {
            struct json_object *e = json_object_array_get_idx(ids, i);
            const char *id = json_object_get_string(e);
            if (id && strcmp(id, workspace->my_user_id) == 0)
            {
                has_self = 1;
                break;
            }
        }
        if (!has_self)
            json_object_array_add(ids,
                                  json_object_new_string(workspace->my_user_id));
    }

    json_object_object_add(msg, "ids", ids);

    if (n > 0 || (workspace->my_user_id && workspace->my_user_id[0]))
    {
        if (slack_ws_send(workspace->ws, msg) == 0)
        {
            SLACK_WS_PRINTF(workspace,
                            "%sweeslack: subscribed presence for %d user%s",
                            weechat_prefix("network"), n,
                            n == 1 ? "" : "s");
        }
    }

    json_object_put(msg);
}

/* Apply muted_channels from users.prefs.get after connect (no set). */
static void
slack_event_connect_prefs_cb(struct t_weeslack_workspace *workspace,
                             int return_code, const char *output,
                             void *user_data)
{
    struct json_object *json, *prefs, *mc, *hw;
    const char *s;
    int muted_n = 0;

    (void) user_data;

    if (return_code != 0 || !output)
        return;

    json = slack_json_decode(output);
    if (!json)
        return;
    if (slack_api_check_error(workspace, json, "users.prefs.get"))
    {
        json_object_put(json);
        return;
    }

    if (json_object_object_get_ex(json, "prefs", &prefs))
    {
        if (json_object_object_get_ex(prefs, "muted_channels", &mc))
        {
            s = json_object_get_string(mc);
            if (s && s[0])
            {
                char *copy = strdup(s);
                char *tok, *save = NULL;

                if (copy)
                {
                    for (tok = strtok_r(copy, ",", &save); tok;
                         tok = strtok_r(NULL, ",", &save))
                    {
                        struct t_slack_channel *ch;
                        struct t_slack_buffer *sbuf;

                        while (*tok == ' ')
                            tok++;
                        if (!tok[0])
                            continue;
                        ch = slack_channel_search(tok);
                        if (!ch)
                            continue;
                        ch->is_muted = 1;
                        sbuf = slack_buffer_search_by_channel(ch->id);
                        if (sbuf)
                            slack_buffer_set_muted(sbuf, 1);
                        muted_n++;
                    }
                    free(copy);
                }
            }
        }

        if (json_object_object_get_ex(prefs, "highlight_words", &hw))
        {
            s = json_object_get_string(hw);
            free(workspace->highlight_words);
            workspace->highlight_words = (s && s[0]) ? strdup(s) : NULL;
        }
    }

    slack_event_apply_highlight_words(workspace);

    if (muted_n > 0)
    {
        SLACK_WS_PRINTF(workspace,
                        "%sweeslack: applied mute to %d channel%s from prefs",
                        weechat_prefix("network"), muted_n,
                        muted_n == 1 ? "" : "s");
    }

    json_object_put(json);

    /* Presence sub needs users loaded; re-send after full directory is up. */
    if (workspace->connected)
        slack_event_presence_subscribe(workspace);
}

void
slack_event_load_mute_prefs(struct t_weeslack_workspace *workspace)
{
    if (!workspace)
        return;
    slack_http_request_new(workspace, "users.prefs.get", NULL,
                           slack_event_connect_prefs_cb, NULL);
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
                emoji = slack_event_apply_emoji_mode(workspace, fmt ? fmt : text);
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
    const char *parent_id = NULL;
    const char *thread_ts = NULL;
    char parent_buf[128];
    char ts_buf[64];

    if (!channel)
        return;

    channel->is_subscribed = subscribe ? 1 : 0;

    /* Resolve parent channel + thread_ts for API */
    if (channel->type == SLACK_CHANNEL_TYPE_THREAD && channel->id)
    {
        const char *prefix = "thread_";
        if (strncmp(channel->id, prefix, 7) == 0)
        {
            const char *rest = channel->id + 7;
            const char *us = strchr(rest, '_');
            if (us && (size_t)(us - rest) < sizeof(parent_buf))
            {
                memcpy(parent_buf, rest, (size_t)(us - rest));
                parent_buf[us - rest] = '\0';
                parent_id = parent_buf;
                thread_ts = us + 1;
            }
        }
    }

    if (workspace && parent_id && thread_ts && thread_ts[0])
    {
        struct json_object *params = json_object_new_object();
        const char *last_read = thread_ts;

        if (channel->last_message_ts && channel->last_message_ts[0])
            last_read = channel->last_message_ts;
        snprintf(ts_buf, sizeof(ts_buf), "%s", last_read);

        json_object_object_add(params, "channel",
                               json_object_new_string(parent_id));
        json_object_object_add(params, "thread_ts",
                               json_object_new_string(thread_ts));
        json_object_object_add(params, "last_read",
                               json_object_new_string(ts_buf));
        slack_http_request_new(
            workspace,
            subscribe ? "subscriptions.thread.add"
                      : "subscriptions.thread.remove",
            params,
            slack_event_simple_api_cb,
            (void *)(subscribe ? "subscriptions.thread.add"
                               : "subscriptions.thread.remove"));
        json_object_put(params);
    }

    if (channel->buffer)
    {
        weechat_buffer_set(channel->buffer, "localvar_set_slack_subscribed",
                           subscribe ? "1" : "0");
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
 * File download (authenticated libcurl multi)
 * ============================================================ */

struct t_slack_dl_ctx
{
    struct t_weeslack_workspace *workspace;
    char *path;
    struct t_gui_buffer *buffer;
    char *mimetype;
    int quiet; /* auto-download: less chat noise */
};

/* ---- Kitty /icat previews (weechat-icat), Xepher-style ---- */

static int
slack_event_icat_command_registered(void)
{
    struct t_infolist *il;
    int found;

    /* "command,icat" → hooks named icat of type command */
    il = weechat_infolist_get("hook", NULL, "command,icat");
    if (!il)
        return 0;
    found = (weechat_infolist_next(il) != 0);
    weechat_infolist_free(il);
    return found;
}

/*
 * True only when /icat exists. Caches a positive result; keeps re-probing
 * until success so a mid-session /script load icat.py is picked up.
 * Warns once if enabled but missing — never runs /icat when absent
 * (would print "unknown command" into the buffer).
 */
static int
slack_event_icat_available(struct t_gui_buffer *warn_buf)
{
    static int confirmed;
    static int warned;

    if (confirmed)
        return 1;
    if (!slack_event_icat_command_registered())
    {
        if (!warned &&
            weechat_config_boolean(weeslack_config.icat_enabled))
        {
            warned = 1;
            weechat_printf(
                warn_buf,
                "%sweeslack: look.icat_enabled is on but /icat is not "
                "available — load weechat-icat (e.g. /script load icat.py)",
                weechat_prefix("error"));
        }
        return 0;
    }
    confirmed = 1;
    return 1;
}

static int
slack_event_is_image_mime(const char *mime)
{
    return (mime && strncmp(mime, "image/", 6) == 0);
}

static int
slack_event_is_image_path(const char *path)
{
    const char *dot = NULL;
    const char *p;
    char ext[16];
    size_t n;

    if (!path || !path[0])
        return 0;
    for (p = path; *p && *p != '?'; p++)
    {
        if (*p == '.')
            dot = p;
    }
    if (!dot || !dot[1])
        return 0;
    n = 0;
    for (p = dot; *p && *p != '?' && n + 1 < sizeof(ext); p++)
        ext[n++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    ext[n] = '\0';
    return (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0 ||
            strcmp(ext, ".png") == 0 || strcmp(ext, ".gif") == 0 ||
            strcmp(ext, ".webp") == 0 || strcmp(ext, ".bmp") == 0);
}

/* PNG / JPEG pixel size for /icat -columns/-rows (Xepher util). */
static void
slack_event_read_image_dimensions(const char *path,
                                   unsigned int *out_w, unsigned int *out_h)
{
    FILE *fp;
    unsigned char hdr[24];
    size_t n;

    *out_w = 0;
    *out_h = 0;
    if (!path)
        return;
    fp = fopen(path, "rb");
    if (!fp)
        return;
    n = fread(hdr, 1, sizeof(hdr), fp);
    if (n >= 24 &&
        hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G')
    {
        *out_w = ((unsigned int)hdr[16] << 24) | ((unsigned int)hdr[17] << 16) |
                 ((unsigned int)hdr[18] << 8) | (unsigned int)hdr[19];
        *out_h = ((unsigned int)hdr[20] << 24) | ((unsigned int)hdr[21] << 16) |
                 ((unsigned int)hdr[22] << 8) | (unsigned int)hdr[23];
        fclose(fp);
        return;
    }
    if (n >= 3 && hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF)
    {
        unsigned char buf[65536];
        size_t total;
        size_t i;

        fseek(fp, 2, SEEK_SET);
        total = fread(buf, 1, sizeof(buf), fp);
        for (i = 0; i + 8 < total; i++)
        {
            if (buf[i] == 0xFF &&
                (buf[i + 1] == 0xC0 || buf[i + 1] == 0xC1 ||
                 buf[i + 1] == 0xC2))
            {
                *out_h = ((unsigned int)buf[i + 5] << 8) |
                         (unsigned int)buf[i + 6];
                *out_w = ((unsigned int)buf[i + 7] << 8) |
                         (unsigned int)buf[i + 8];
                break;
            }
        }
    }
    fclose(fp);
}

static void
slack_event_try_icat_preview(struct t_gui_buffer *buffer, const char *path,
                              const char *mimetype)
{
    unsigned int pw, ph, cols, rows;
    char cmd[1024];
    char path_q[768];
    size_t i, j;

    if (!path || !path[0] || !buffer)
        return;
    if (!weechat_config_boolean(weeslack_config.icat_enabled))
        return;
    if (!slack_event_is_image_mime(mimetype) &&
        !slack_event_is_image_path(path))
        return;
    if (!slack_event_icat_available(buffer))
        return;

    slack_event_read_image_dimensions(path, &pw, &ph);
    cols = 40;
    if (pw == 0 || ph == 0)
        rows = 10;
    else
    {
        rows = (unsigned int)(((double)cols * (double)ph / (double)pw) + 0.5);
        if (rows < 1)
            rows = 1;
        if (rows > 20)
            rows = 20;
    }

    /* Quote path for /icat; strip double-quotes from path content. */
    path_q[0] = '"';
    j = 1;
    for (i = 0; path[i] && j + 2 < sizeof(path_q); i++)
    {
        if (path[i] == '"' || path[i] == '\n' || path[i] == '\r')
            continue;
        path_q[j++] = path[i];
    }
    path_q[j++] = '"';
    path_q[j] = '\0';

    snprintf(cmd, sizeof(cmd),
             "/icat -print_immediately -quiet -columns %u -rows %u %s",
             cols, rows, path_q);
    weechat_command(buffer, cmd);
}

/* ---- Custom emoji → cache + /icat (when look.icat_enabled + /icat) ----
 * WeeChat chat lines are text; graphics use Kitty via /icat (same as files).
 * Gate: look.icat_enabled AND /icat registered (probe, one-time warn).
 */

struct t_slack_emoji_icat_ctx
{
    struct t_gui_buffer *buffer;
    char *path;
    char *name;
};

/* Compact tile for custom emoji (not full attachment size). */
static void
slack_event_icat_emoji_tile(struct t_gui_buffer *buffer, const char *path,
                            const char *shortcode)
{
    unsigned int pw, ph, cols, rows;
    char cmd[1024], path_q[768];
    size_t pi, pj;

    if (!buffer || !path || !path[0])
        return;
    if (!weechat_config_boolean(weeslack_config.icat_enabled))
        return;
    if (!slack_event_icat_available(buffer))
        return;

    slack_event_read_image_dimensions(path, &pw, &ph);
    cols = 4;
    if (pw == 0 || ph == 0)
        rows = 2;
    else
    {
        rows = (unsigned int)(((double)cols * (double)ph / (double)pw) + 0.5);
        if (rows < 1)
            rows = 1;
        if (rows > 4)
            rows = 4;
    }

    path_q[0] = '"';
    pj = 1;
    for (pi = 0; path[pi] && pj + 2 < sizeof(path_q); pi++)
    {
        if (path[pi] == '"' || path[pi] == '\n' || path[pi] == '\r')
            continue;
        path_q[pj++] = path[pi];
    }
    path_q[pj++] = '"';
    path_q[pj] = '\0';

    if (shortcode && shortcode[0])
        weechat_printf(buffer, "%s:%s:",
                        weechat_prefix("network"), shortcode);
    snprintf(cmd, sizeof(cmd),
             "/icat -print_immediately -quiet -columns %u -rows %u %s",
             cols, rows, path_q);
    weechat_command(buffer, cmd);
}

static void
slack_event_emoji_icat_done(void *user_data, int ok, long http_code)
{
    struct t_slack_emoji_icat_ctx *ctx = user_data;

    (void)http_code;
    if (!ctx)
        return;
    if (ok && ctx->path && ctx->buffer)
        slack_event_icat_emoji_tile(ctx->buffer, ctx->path, ctx->name);
    else if (!ok && ctx->path)
        unlink(ctx->path); /* incomplete download */
    free(ctx->path);
    free(ctx->name);
    free(ctx);
}

static int
slack_event_emoji_cache_path(struct t_weeslack_workspace *workspace,
                             const char *name, const char *url,
                             char *out, size_t out_sz)
{
    const char *data_dir, *wskey, *ext;
    char dir[512];
    char safe[96];
    size_t i, j;
    const char *dot, *q;

    if (!name || !name[0] || !out || out_sz < 32)
        return 0;

    data_dir = weechat_info_get("weechat_data_dir", "");
    if (!data_dir || !data_dir[0])
        data_dir = weechat_info_get("weechat_dir", "");
    if (!data_dir || !data_dir[0])
        return 0;

    wskey = "default";
    if (workspace)
    {
        if (workspace->domain && workspace->domain[0])
            wskey = workspace->domain;
        else if (workspace->id && workspace->id[0])
            wskey = workspace->id;
    }

    j = 0;
    for (i = 0; name[i] && j + 1 < sizeof(safe); i++)
    {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+')
            safe[j++] = c;
        else
            safe[j++] = '_';
    }
    safe[j] = '\0';
    if (!safe[0])
        return 0;

    ext = ".png";
    if (url)
    {
        q = strchr(url, '?');
        dot = NULL;
        for (i = 0; url[i] && (!q || &url[i] < q); i++)
        {
            if (url[i] == '.')
                dot = &url[i];
        }
        if (dot)
        {
            if (strncmp(dot, ".gif", 4) == 0 || strncmp(dot, ".GIF", 4) == 0)
                ext = ".gif";
            else if (strncmp(dot, ".jpg", 4) == 0 ||
                     strncmp(dot, ".jpeg", 5) == 0)
                ext = ".jpg";
            else if (strncmp(dot, ".webp", 5) == 0)
                ext = ".webp";
        }
    }

    snprintf(dir, sizeof(dir), "%s/weeslack/emoji/%s", data_dir, wskey);
    weechat_mkdir_parents(dir, 0755);
    snprintf(out, out_sz, "%s/%s%s", dir, safe, ext);
    return 1;
}

/*
 * Scan message text for :custom: shortcodes whose value is an image URL.
 * Cache under data_dir/weeslack/emoji/ and /icat when available.
 * Live messages only (caller); max 4 distinct emoji per message.
 */
static void
slack_event_custom_emoji_icat(struct t_weeslack_workspace *workspace,
                              struct t_gui_buffer *buffer,
                              const char *text)
{
    const char *src;
    int shown = 0;
    char seen[4][64];
    int n_seen = 0;

    if (!workspace || !buffer || !text || !text[0])
        return;
    if (!weechat_config_boolean(weeslack_config.icat_enabled))
        return;
    if (!slack_event_icat_available(buffer))
        return;

    src = text;
    while (*src && shown < 4)
    {
        const char *end;
        char shortcode[64];
        size_t sc_len, i;
        const char *custom, *url;
        char path[768];
        struct stat st;
        int already = 0;
        int valid = 1;

        if (*src != ':')
        {
            src++;
            continue;
        }
        end = strchr(src + 1, ':');
        if (!end || end <= src + 1 || (end - src - 1) >= 64)
        {
            src++;
            continue;
        }
        sc_len = (size_t)(end - src - 1);
        memcpy(shortcode, src + 1, sc_len);
        shortcode[sc_len] = '\0';
        src = end + 1;

        for (i = 0; i < sc_len; i++)
        {
            char c = shortcode[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-'))
            {
                valid = 0;
                break;
            }
        }
        if (!valid)
            continue;

        for (i = 0; i < (size_t)n_seen; i++)
        {
            if (strcmp(seen[i], shortcode) == 0)
            {
                already = 1;
                break;
            }
        }
        if (already)
            continue;

        custom = slack_custom_emoji_lookup(workspace, shortcode);
        if (!custom || !custom[0])
            continue;
        if (strncmp(custom, "alias:", 6) == 0)
        {
            custom = slack_custom_emoji_lookup(workspace, custom + 6);
            if (!custom || !custom[0])
                continue;
        }
        if (strncmp(custom, "http://", 7) != 0 &&
            strncmp(custom, "https://", 8) != 0)
            continue;
        url = custom;

        if (n_seen < 4)
        {
            snprintf(seen[n_seen], sizeof(seen[n_seen]), "%s", shortcode);
            n_seen++;
        }

        if (!slack_event_emoji_cache_path(workspace, shortcode, url,
                                          path, sizeof(path)))
            continue;

        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
        {
            slack_event_icat_emoji_tile(buffer, path, shortcode);
            shown++;
            continue;
        }

        {
            struct t_slack_emoji_icat_ctx *ctx =
                calloc(1, sizeof(*ctx));
            if (!ctx)
                continue;
            ctx->buffer = buffer;
            ctx->path = strdup(path);
            ctx->name = strdup(shortcode);
            if (!ctx->path)
            {
                free(ctx->name);
                free(ctx);
                continue;
            }
            /* Public CDN; no auth required for most custom emoji. */
            if (!slack_http_curl_get_file(url, path, NULL, NULL,
                                          slack_event_emoji_icat_done, ctx))
            {
                free(ctx->path);
                free(ctx->name);
                free(ctx);
            }
            else
                shown++;
        }
    }
}

static void
slack_event_download_done(void *user_data, int ok, long http_code)
{
    struct t_slack_dl_ctx *ctx = user_data;

    if (!ctx)
        return;

    if (ok)
    {
        if (!ctx->quiet)
        {
            weechat_printf(ctx->buffer ? ctx->buffer : NULL,
                            "%sweeslack: downloaded %s",
                            weechat_prefix("network"),
                            ctx->path ? ctx->path : "?");
        }
        /* Kitty preview only after local file exists (Slack needs auth). */
        slack_event_try_icat_preview(ctx->buffer, ctx->path, ctx->mimetype);
    }
    else
    {
        if (ctx->path)
            unlink(ctx->path);
        if (!ctx->quiet)
        {
            weechat_printf(ctx->buffer ? ctx->buffer : NULL,
                            "%sweeslack: download failed (HTTP %ld)",
                            weechat_prefix("error"), http_code);
        }
    }

    free(ctx->path);
    free(ctx->mimetype);
    free(ctx);
}

/* Sanitize a single path component (origin or filename). */
static void
slack_event_sanitize_path_component(char *s)
{
    if (!s)
        return;
    for (; *s; s++)
    {
        if (*s == '/' || *s == '\\' || (unsigned char)*s < 32)
            *s = '_';
    }
}

/*
 * Layout (Xepher-style):
 *   <root>/weeslack/<origin>/<YYYY-MM-DD>/<file>
 * root = look.download_path, else $XDG_DOWNLOAD_DIR, else ~/Downloads
 */
static int
slack_event_download_build_path(char *path, size_t path_size,
                                const char *origin,
                                const char *preferred_name,
                                const char *url)
{
    const char *dir_cfg, *xdg, *home;
    char root[384];
    char origin_s[160];
    char name_s[256];
    char date_s[32];
    char dir[640];
    time_t now;
    struct tm tm_now;
    struct stat st;
    int suffix;

    if (!path || path_size < 32)
        return 0;

    dir_cfg = weechat_config_string(weeslack_config.download_path);
    if (dir_cfg && dir_cfg[0])
        snprintf(root, sizeof(root), "%s", dir_cfg);
    else
    {
        xdg = getenv("XDG_DOWNLOAD_DIR");
        if (xdg && xdg[0])
            snprintf(root, sizeof(root), "%s", xdg);
        else
        {
            home = getenv("HOME");
            snprintf(root, sizeof(root), "%s/Downloads",
                     home ? home : "/tmp");
        }
    }

    /* strip trailing slashes from root */
    {
        size_t n = strlen(root);
        while (n > 1 && root[n - 1] == '/')
            root[--n] = '\0';
    }

    if (origin && origin[0])
        snprintf(origin_s, sizeof(origin_s), "%s", origin);
    else
        snprintf(origin_s, sizeof(origin_s), "misc");
    slack_event_sanitize_path_component(origin_s);

    now = time(NULL);
    if (localtime_r(&now, &tm_now))
        snprintf(date_s, sizeof(date_s), "%04d-%02d-%02d",
                 tm_now.tm_year + 1900,
                 tm_now.tm_mon + 1,
                 tm_now.tm_mday);
    else
        snprintf(date_s, sizeof(date_s), "unknown");

    /* preferred filename, else URL basename */
    name_s[0] = '\0';
    if (preferred_name && preferred_name[0])
        snprintf(name_s, sizeof(name_s), "%s", preferred_name);
    else if (url && url[0])
    {
        const char *base = strrchr(url, '/');
        const char *q;
        size_t blen;

        base = base ? base + 1 : url;
        q = strchr(base, '?');
        blen = q ? (size_t)(q - base) : strlen(base);
        if (blen == 0)
            snprintf(name_s, sizeof(name_s), "download.bin");
        else
        {
            if (blen >= sizeof(name_s))
                blen = sizeof(name_s) - 1;
            memcpy(name_s, base, blen);
            name_s[blen] = '\0';
        }
    }
    if (!name_s[0])
        snprintf(name_s, sizeof(name_s), "download.bin");
    slack_event_sanitize_path_component(name_s);

    /* <root>/weeslack/<origin>/<YYYY-MM-DD> */
    snprintf(dir, sizeof(dir), "%s/weeslack/%s/%s", root, origin_s, date_s);
    weechat_mkdir_parents(dir, 0755);

    snprintf(path, path_size, "%s/%s", dir, name_s);

    /* uniqueness: file, file.1, file.2, … (Xepher-style) */
    suffix = 1;
    while (stat(path, &st) == 0 && suffix < 10000)
    {
        snprintf(path, path_size, "%s/%s.%d", dir, name_s, suffix);
        suffix++;
    }

    return 1;
}

static void
slack_event_download_file_ex(struct t_weeslack_workspace *workspace,
                              const char *url,
                              struct t_gui_buffer *buffer,
                              const char *origin,
                              const char *preferred_name,
                              const char *mimetype,
                              int quiet)
{
    char path[768];
    char auth[512];
    char cookie_h[512];
    const char *cookie = NULL;
    struct t_slack_dl_ctx *ctx;

    if (!workspace || !url || !url[0] || !workspace->token)
        return;

    if (!slack_event_download_build_path(path, sizeof(path), origin,
                                          preferred_name, url))
        return;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return;
    ctx->workspace = workspace;
    ctx->path = strdup(path);
    ctx->buffer = buffer;
    ctx->mimetype = (mimetype && mimetype[0]) ? strdup(mimetype) : NULL;
    ctx->quiet = quiet ? 1 : 0;
    if (!ctx->path)
    {
        free(ctx->mimetype);
        free(ctx);
        return;
    }

    snprintf(auth, sizeof(auth), "Bearer %s", workspace->token);
    if (workspace->cookie && workspace->cookie[0])
    {
        snprintf(cookie_h, sizeof(cookie_h), "%s%s",
                 (strncmp(workspace->cookie, "d=", 2) == 0) ? "" : "d=",
                 workspace->cookie);
        cookie = cookie_h;
    }

    if (!slack_http_curl_get_file(url, path, auth, cookie,
                                  slack_event_download_done, ctx))
    {
        if (!quiet)
        {
            weechat_printf(buffer, "%sweeslack: could not start download",
                            weechat_prefix("error"));
        }
        free(ctx->path);
        free(ctx->mimetype);
        free(ctx);
        return;
    }

    if (!quiet)
    {
        weechat_printf(buffer, "%sweeslack: downloading to %s ...",
                        weechat_prefix("network"), path);
    }
}

void
slack_event_download_file(struct t_weeslack_workspace *workspace,
                           const char *url,
                           struct t_gui_buffer *buffer,
                           const char *origin,
                           const char *preferred_name)
{
    slack_event_download_file_ex(workspace, url, buffer, origin,
                                  preferred_name, NULL, 0);
}

void
slack_event_auto_download_message_files(struct t_weeslack_workspace *workspace,
                                         struct t_slack_channel *channel,
                                         struct json_object *msg_json,
                                         struct t_gui_buffer *buffer)
{
    struct json_object *files_obj;
    int count, i;
    char origin[192];
    const char *team, *ch_name;
    int auto_all, want_icat;

    if (!workspace || !msg_json)
        return;

    auto_all = weechat_config_boolean(weeslack_config.auto_download_files);
    want_icat = weechat_config_boolean(weeslack_config.icat_enabled);
    /* Images still download when only icat is on (private URLs need auth). */
    if (!auto_all && !want_icat)
        return;
    /* Don't start image fetches that will only fail/spam if /icat missing. */
    if (!auto_all && want_icat && !slack_event_icat_available(buffer))
        return;

    if (!json_object_object_get_ex(msg_json, "files", &files_obj))
        return;

    count = json_object_array_length(files_obj);
    if (count <= 0)
        return;

    team = workspace->name ? workspace->name : workspace->id;
    ch_name = (channel && channel->name) ? channel->name
        : ((channel && channel->id) ? channel->id : "misc");
    if (team && team[0])
        snprintf(origin, sizeof(origin), "%s.%s", team, ch_name);
    else
        snprintf(origin, sizeof(origin), "%s", ch_name);

    for (i = 0; i < count; i++)
    {
        struct json_object *f = json_object_array_get_idx(files_obj, i);
        struct json_object *obj, *mode_obj;
        const char *url = NULL;
        const char *title = NULL;
        const char *name = NULL;
        const char *filetype = NULL;
        const char *mimetype = NULL;
        char preferred[256];
        int is_image;

        if (!f)
            continue;
        if (json_object_object_get_ex(f, "mode", &mode_obj) &&
            json_object_get_string(mode_obj) &&
            strcmp(json_object_get_string(mode_obj), "tombstone") == 0)
            continue;

        if (json_object_object_get_ex(f, "url_private_download", &obj))
            url = json_object_get_string(obj);
        if ((!url || !url[0]) &&
            json_object_object_get_ex(f, "url_private", &obj))
            url = json_object_get_string(obj);
        if (!url || !url[0])
            continue;

        if (json_object_object_get_ex(f, "title", &obj))
            title = json_object_get_string(obj);
        if (json_object_object_get_ex(f, "name", &obj))
            name = json_object_get_string(obj);
        if (json_object_object_get_ex(f, "filetype", &obj))
            filetype = json_object_get_string(obj);
        if (json_object_object_get_ex(f, "mimetype", &obj))
            mimetype = json_object_get_string(obj);

        preferred[0] = '\0';
        if (name && name[0])
            snprintf(preferred, sizeof(preferred), "%s", name);
        else if (title && title[0])
        {
            if (filetype && filetype[0] &&
                !strstr(title, filetype))
                snprintf(preferred, sizeof(preferred), "%s.%s",
                         title, filetype);
            else
                snprintf(preferred, sizeof(preferred), "%s", title);
        }

        is_image = slack_event_is_image_mime(mimetype) ||
                   slack_event_is_image_path(preferred[0] ? preferred : url);

        if (!auto_all && !is_image)
            continue;

        slack_event_download_file_ex(workspace, url, buffer, origin,
                                      preferred[0] ? preferred : NULL,
                                      mimetype, 1 /* quiet */);
    }
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
            emoji = slack_event_apply_emoji_mode(workspace, fmt ? fmt : text);
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
                slack_channel_set_workspace(ch, workspace);
                slack_channel_update(ch, ch_obj);
                ch->is_member = 1;
                if (!ch->buffer)
                    slack_buffer_new(workspace, ch);
                if (ch->buffer &&
                    weechat_config_boolean(weeslack_config.switch_buffer_on_join))
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
    struct t_slack_channel *ch;

    if (!workspace || !name_or_id || !name_or_id[0])
        return;

    arg = name_or_id;
    if (arg[0] == '#')
        arg++;

    /*
     * Already a member with a known model: just reopen the buffer (user
     * closed it earlier) — no API join, no rate-limit hit.
     */
    ch = slack_channel_search(arg);
    if (!ch)
    {
        for (ch = slack_channel_list_global(); ch; ch = ch->next)
        {
            if (ch->workspace && ch->workspace != workspace)
                continue;
            if (ch->name && weechat_strcasecmp(ch->name, arg) == 0)
                break;
        }
    }
    if (ch && ch->is_member)
    {
        if (!ch->buffer)
            slack_buffer_new(workspace, ch);
        if (ch->buffer)
        {
            weechat_buffer_set(ch->buffer, "hidden", "0");
            if (weechat_config_boolean(weeslack_config.switch_buffer_on_join))
                weechat_buffer_set(ch->buffer, "display", "1");
        }
        return;
    }

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
        if (!slack_api_check_error(workspace, json, "conversations.leave/close"))
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
    struct t_slack_channel *ch;
    const char *method = "conversations.leave";

    if (!workspace || !channel_id || !channel_id[0])
        return;

    /* DMs / MPDMs use conversations.close; public/private use leave. */
    ch = slack_channel_search(channel_id);
    if (ch && (ch->type == SLACK_CHANNEL_TYPE_DM ||
               ch->type == SLACK_CHANNEL_TYPE_MPDM))
        method = "conversations.close";

    id_copy = strdup(channel_id);
    params = json_object_new_object();
    json_object_object_add(params, "channel",
                           json_object_new_string(channel_id));
    slack_http_request_new(workspace, method, params,
                           slack_event_leave_cb, id_copy);
    json_object_put(params);
}

struct t_slack_whois_ctx
{
    char *user_id;
    struct t_gui_buffer *buffer;
};

/* defined later — live presence for whois */
static void slack_event_presence_cb(struct t_weeslack_workspace *workspace,
                                    int return_code, const char *output,
                                    void *user_data);

void
slack_event_whois(struct t_weeslack_workspace *workspace,
                  const char *name_or_id,
                  struct t_gui_buffer *buffer)
{
    struct t_slack_user *user;
    struct t_gui_buffer *out;
    const char *presence;

    out = buffer ? buffer : NULL;

    if (!name_or_id || !name_or_id[0])
    {
        weechat_printf(out, "%sweeslack: usage: /cslack whois <user>",
                        weechat_prefix("error"));
        return;
    }

    user = slack_user_search_ws(workspace, name_or_id);
    if (!user)
        user = slack_user_search_name_ws(workspace, name_or_id);
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
            if (workspace && u->workspace && u->workspace != workspace)
                continue;
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

    /* live presence via users.getPresence */
    if (workspace && user->id && user->id[0])
    {
        struct t_slack_whois_ctx *ctx = calloc(1, sizeof(*ctx));
        struct json_object *params;

        if (ctx)
        {
            ctx->user_id = strdup(user->id);
            ctx->buffer = out;
            params = json_object_new_object();
            json_object_object_add(params, "user",
                                   json_object_new_string(user->id));
            slack_http_request_new(workspace, "users.getPresence", params,
                                   slack_event_presence_cb, ctx);
            json_object_put(params);
        }
    }
}

/* ============================================================
 * RTM reconnect (fresh rtm.connect URL — no full bootstrap)
 * ============================================================ */

static void
slack_event_rtm_reconnect_cb(struct t_weeslack_workspace *workspace,
                             int return_code, const char *output,
                             void *user_data)
{
    struct json_object *json, *url_obj;

    (void) user_data;

    if (return_code != 0 || !output)
    {
        SLACK_WS_PRINTF(workspace,
                        "%sweeslack: rtm.connect (reconnect) failed (rc=%d)",
                        weechat_prefix("error"), return_code);
        if (workspace && workspace->ws)
            slack_ws_reconnect(workspace->ws);
        return;
    }

    json = slack_json_decode(output);
    if (!json)
    {
        if (workspace && workspace->ws)
            slack_ws_reconnect(workspace->ws);
        return;
    }

    if (slack_api_check_error(workspace, json, "rtm.connect"))
    {
        json_object_put(json);
        if (workspace && workspace->ws)
            slack_ws_reconnect(workspace->ws);
        return;
    }

    if (json_object_object_get_ex(json, "url", &url_obj))
    {
        const char *ws_url = json_object_get_string(url_obj);
        if (!workspace->ws)
            workspace->ws = slack_ws_new(workspace);
        if (workspace->ws && ws_url)
        {
            SLACK_WS_PRINTF(workspace,
                            "%sweeslack: reconnected session — new websocket URL",
                            weechat_prefix("network"));
            slack_ws_connect(workspace->ws, ws_url);
        }
    }

    json_object_put(json);
}

void
slack_event_rtm_reconnect(struct t_weeslack_workspace *workspace)
{
    struct json_object *params;

    if (!workspace || !workspace->token || !workspace->token[0])
        return;

    workspace->connected = 0;
    params = json_object_new_object();
    json_object_object_add(params, "batch_presence_aware",
                           json_object_new_int(1));
    slack_http_request_new(workspace, "rtm.connect", params,
                           slack_event_rtm_reconnect_cb, NULL);
    json_object_put(params);
}

/* ============================================================
 * Directory refresh (users + emoji, no channel re-list)
 * ============================================================ */

static void
slack_event_refresh_users_cb(struct t_weeslack_workspace *workspace,
                             int return_code, const char *output,
                             void *user_data)
{
    (void) user_data;

    /* Reuse users.list parsing by calling the normal users path without
     * continuing bootstrap: parse here lightly then fetch emoji. */
    if (return_code == 0 && output)
    {
        struct json_object *json = slack_json_decode(output);
        if (json && !slack_api_check_error(workspace, json, "users.list"))
        {
            struct json_object *members_obj;
            if (json_object_object_get_ex(json, "members", &members_obj))
            {
                int count = json_object_array_length(members_obj);
                int i;
                for (i = 0; i < count; i++)
                {
                    struct json_object *u_obj =
                        json_object_array_get_idx(members_obj, i);
                    struct json_object *id_obj, *name_obj;
                    const char *uid = NULL, *uname = NULL;
                    if (json_object_object_get_ex(u_obj, "id", &id_obj))
                        uid = json_object_get_string(id_obj);
                    if (json_object_object_get_ex(u_obj, "name", &name_obj))
                        uname = json_object_get_string(name_obj);
                    if (!uid)
                        continue;
                    if (!uname || !uname[0])
                        uname = uid;
                    {
                        struct t_slack_user *user = slack_user_new(uid, uname,
                                                                   workspace);
                        if (user)
                        {
                            slack_user_update(user, u_obj);
                            if (user->is_bot || user->is_app_user)
                            {
                                struct t_slack_bot *bot =
                                    slack_bot_new(uid, uname, workspace);
                                if (bot)
                                    slack_bot_update(bot, u_obj);
                            }
                        }
                    }
                }
            }
            {
                int total = 0;
                struct t_slack_user *u;
                for (u = slack_user_list_global(); u; u = u->next)
                    total++;
                SLACK_WS_PRINTF(workspace, "%sweeslack: refreshed %d users",
                                weechat_prefix("network"), total);
            }
            slack_buffer_purge_hidden_nicks();
        }
        if (json)
            json_object_put(json);
    }
    else
    {
        SLACK_WS_PRINTF(workspace, "%sweeslack: users refresh failed (rc=%d)",
                        weechat_prefix("error"), return_code);
    }

    /* refresh emoji map only (no bootstrap chain) */
    slack_event_fetch_emoji(workspace);
}

void
slack_event_refresh_directory(struct t_weeslack_workspace *workspace)
{
    struct json_object *params;

    if (!workspace)
        return;

    params = json_object_new_object();
    json_object_object_add(params, "limit", json_object_new_int(200));
    slack_http_request_new(workspace, "users.list", params,
                           slack_event_refresh_users_cb, NULL);
    json_object_put(params);
}

/* ============================================================
 * Presence for whois
 * ============================================================ */

static void
slack_event_presence_cb(struct t_weeslack_workspace *workspace,
                        int return_code, const char *output,
                        void *user_data)
{
    struct t_slack_whois_ctx *ctx = user_data;
    struct json_object *json, *pres_obj, *online_obj;
    const char *presence = NULL;
    int online = -1;
    struct t_slack_user *user;

    (void) workspace;

    if (!ctx)
        return;

    if (return_code == 0 && output)
    {
        json = slack_json_decode(output);
        if (json && !slack_api_check_error(workspace, json, "users.getPresence"))
        {
            if (json_object_object_get_ex(json, "presence", &pres_obj))
                presence = json_object_get_string(pres_obj);
            if (json_object_object_get_ex(json, "online", &online_obj))
                online = json_object_get_boolean(online_obj) ? 1 : 0;

            user = ctx->user_id ? slack_user_search(ctx->user_id) : NULL;
            if (user && presence && presence[0])
            {
                free(user->presence);
                user->presence = strdup(presence);
            }

            weechat_printf(ctx->buffer,
                            "%s  presence (live): %s%s%s",
                            weechat_prefix("network"),
                            presence ? presence : "?",
                            online >= 0 ? (online ? " (online)" : " (away)") : "",
                            "");
        }
        if (json)
            json_object_put(json);
    }

    free(ctx->user_id);
    free(ctx);
}
