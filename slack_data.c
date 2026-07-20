#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <openssl/sha.h>

#include "weeslack.h"
#include "slack_data.h"

/* ============================================================
 * SlackTS
 * ============================================================ */

SlackTS
slack_ts_zero(void)
{
    SlackTS ts = { 0, 0 };
    return ts;
}

SlackTS
slack_ts_new(const char *ts_str)
{
    SlackTS ts = { 0, 0 };

    if (!ts_str || !ts_str[0])
        return ts;

    const char *dot = strchr(ts_str, '.');
    if (dot)
    {
        ts.sec = (time_t)strtol(ts_str, NULL, 10);
        ts.usec = strtol(dot + 1, NULL, 10);
    }
    else
    {
        ts.sec = (time_t)strtol(ts_str, NULL, 10);
        ts.usec = 0;
    }

    return ts;
}

SlackTS
slack_ts_from_time(time_t t)
{
    SlackTS ts;
    ts.sec = t;
    ts.usec = 0;
    return ts;
}

int
slack_ts_cmp(SlackTS a, SlackTS b)
{
    if (a.sec < b.sec)
        return -1;
    if (a.sec > b.sec)
        return 1;
    if (a.usec < b.usec)
        return -1;
    if (a.usec > b.usec)
        return 1;
    return 0;
}

int
slack_ts_eq(SlackTS a, SlackTS b)
{
    return a.sec == b.sec && a.usec == b.usec;
}

int
slack_ts_is_empty(SlackTS ts)
{
    return ts.sec == 0 && ts.usec == 0;
}

char *
slack_ts_to_string(SlackTS ts)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%ld.%06ld", (long)ts.sec, ts.usec);
    return strdup(buf);
}

/* ============================================================
 * SlackUser
 * ============================================================ */

static struct t_slack_user *slack_user_list = NULL;

struct t_slack_user *
slack_user_new(const char *id, const char *name, const char *team_id)
{
    struct t_slack_user *user;

    if (!id || !name)
        return NULL;

    user = slack_user_search(id);
    if (user)
        return user;

    user = calloc(1, sizeof(struct t_slack_user));
    if (!user)
        return NULL;

    user->id = strdup(id);
    user->name = strdup(name);
    user->presence = strdup("unknown");

    user->next = slack_user_list;
    user->prev = NULL;
    if (slack_user_list)
        slack_user_list->prev = user;
    slack_user_list = user;

    (void) team_id;

    return user;
}

struct t_slack_user *
slack_user_search(const char *id)
{
    struct t_slack_user *user;

    if (!id)
        return NULL;

    for (user = slack_user_list; user; user = user->next)
    {
        if (strcmp(user->id, id) == 0)
            return user;
    }
    return NULL;
}

struct t_slack_user *
slack_user_list_global(void)
{
    return slack_user_list;
}

void
slack_user_free(struct t_slack_user *user)
{
    if (!user)
        return;

    if (user->prev)
        user->prev->next = user->next;
    else
        slack_user_list = user->next;

    if (user->next)
        user->next->prev = user->prev;

    free(user->id);
    free(user->name);
    free(user->real_name);
    free(user->display_name);
    free(user->color);
    free(user->avatar_72);
    free(user->presence);
    free(user->status_emoji);
    free(user->status_text);
    if (user->profile_json)
        json_object_put(user->profile_json);
    free(user);
}

const char *
slack_user_get_color(struct t_slack_user *user)
{
    if (!user)
        return "default";

    if (user->color && user->color[0])
        return user->color;

    return "default";
}

const char *
slack_user_best_name(struct t_slack_user *user)
{
    if (!user)
        return NULL;

    if (user->display_name && user->display_name[0])
        return user->display_name;
    if (user->real_name && user->real_name[0])
        return user->real_name;
    if (user->name && user->name[0])
        return user->name;
    if (user->id && user->id[0])
        return user->id;
    return "unknown";
}

int
slack_user_hide_from_nicklist(struct t_slack_user *user)
{
    const char *n;

    if (!user)
        return 1;

    if (user->deleted)
        return 1;
    if (user->is_bot)
        return 1;
    if (user->is_app_user)
        return 1;

    /* Classic Slackbot */
    if (user->id && strcmp(user->id, "USLACKBOT") == 0)
        return 1;

    n = user->name;
    if (n && n[0])
    {
        if (weechat_strcasecmp(n, "slackbot") == 0)
            return 1;
        if (weechat_strcasecmp(n, "uslackbot") == 0)
            return 1;
        /* Common integration bot handles (still show as message authors) */
        if (weechat_strncasecmp(n, "google_drive", 12) == 0)
            return 1;
        if (weechat_strncasecmp(n, "google-drive", 12) == 0)
            return 1;
        if (weechat_strncasecmp(n, "gdrive", 6) == 0)
            return 1;
    }

    n = user->display_name;
    if (n && n[0])
    {
        if (weechat_strcasecmp(n, "slackbot") == 0)
            return 1;
        if (weechat_strncasecmp(n, "google drive", 12) == 0)
            return 1;
    }

    return 0;
}

void
slack_user_update(struct t_slack_user *user, struct json_object *json)
{
    struct json_object *obj;

    if (!user || !json)
        return;

    /* users.list top-level "name" is the @handle (e.g. johndoe) */
    if (json_object_object_get_ex(json, "name", &obj))
    {
        const char *n = json_object_get_string(obj);
        if (n && n[0])
        {
            free(user->name);
            user->name = strdup(n);
        }
    }

    if (json_object_object_get_ex(json, "real_name", &obj))
    {
        free(user->real_name);
        user->real_name = strdup(json_object_get_string(obj));
    }

    /* rarely top-level; usually under profile (handled below) */
    if (json_object_object_get_ex(json, "display_name", &obj))
    {
        free(user->display_name);
        user->display_name = strdup(json_object_get_string(obj));
    }

    if (json_object_object_get_ex(json, "color", &obj))
    {
        free(user->color);
        user->color = strdup(json_object_get_string(obj));
    }

    if (json_object_object_get_ex(json, "deleted", &obj))
        user->deleted = json_object_get_boolean(obj);

    if (json_object_object_get_ex(json, "is_bot", &obj))
        user->is_bot = json_object_get_boolean(obj);

    if (json_object_object_get_ex(json, "is_app_user", &obj))
        user->is_app_user = json_object_get_boolean(obj);

    if (json_object_object_get_ex(json, "presence", &obj))
    {
        free(user->presence);
        user->presence = strdup(json_object_get_string(obj));
    }

    if (json_object_object_get_ex(json, "profile", &obj))
    {
        if (user->profile_json)
            json_object_put(user->profile_json);
        user->profile_json = json_object_get(obj);

        struct json_object *avatar;
        if (json_object_object_get_ex(obj, "image_72", &avatar))
        {
            free(user->avatar_72);
            user->avatar_72 = strdup(json_object_get_string(avatar));
        }

        /* Slack puts display/real names under profile for users.list */
        struct json_object *disp;
        if (json_object_object_get_ex(obj, "display_name", &disp))
        {
            const char *d = json_object_get_string(disp);
            if (d && d[0])
            {
                free(user->display_name);
                user->display_name = strdup(d);
            }
        }

        struct json_object *real;
        if (json_object_object_get_ex(obj, "real_name", &real))
        {
            const char *r = json_object_get_string(real);
            if (r && r[0])
            {
                free(user->real_name);
                user->real_name = strdup(r);
            }
        }

        struct json_object *status_emoji;
        if (json_object_object_get_ex(obj, "status_emoji", &status_emoji))
        {
            free(user->status_emoji);
            user->status_emoji = strdup(json_object_get_string(status_emoji));
        }

        struct json_object *status_text;
        if (json_object_object_get_ex(obj, "status_text", &status_text))
        {
            free(user->status_text);
            user->status_text = strdup(json_object_get_string(status_text));
        }
    }
}

/* ============================================================
 * SlackBot
 * ============================================================ */

static struct t_slack_bot *slack_bot_list = NULL;

struct t_slack_bot *
slack_bot_new(const char *id, const char *name)
{
    struct t_slack_bot *bot;

    if (!id || !name)
        return NULL;

    bot = slack_bot_search(id);
    if (bot)
        return bot;

    bot = calloc(1, sizeof(struct t_slack_bot));
    if (!bot)
        return NULL;

    bot->id = strdup(id);
    bot->name = strdup(name);

    bot->next = slack_bot_list;
    bot->prev = NULL;
    if (slack_bot_list)
        slack_bot_list->prev = bot;
    slack_bot_list = bot;

    return bot;
}

struct t_slack_bot *
slack_bot_search(const char *id)
{
    struct t_slack_bot *bot;

    if (!id)
        return NULL;

    for (bot = slack_bot_list; bot; bot = bot->next)
    {
        if (strcmp(bot->id, id) == 0)
            return bot;
    }
    return NULL;
}

struct t_slack_bot *
slack_bot_list_global(void)
{
    return slack_bot_list;
}

const char *
slack_bot_best_name(struct t_slack_bot *bot)
{
    if (!bot)
        return NULL;
    if (bot->username && bot->username[0])
        return bot->username;
    if (bot->name && bot->name[0])
        return bot->name;
    if (bot->real_name && bot->real_name[0])
        return bot->real_name;
    return bot->id;
}

void
slack_bot_update(struct t_slack_bot *bot, struct json_object *json)
{
    struct json_object *obj;

    if (!bot || !json)
        return;

    if (json_object_object_get_ex(json, "name", &obj))
    {
        free(bot->name);
        bot->name = strdup(json_object_get_string(obj));
    }
    if (json_object_object_get_ex(json, "deleted", &obj))
        bot->deleted = json_object_get_boolean(obj);

    if (json_object_object_get_ex(json, "icons", &obj) ||
        json_object_object_get_ex(json, "profile", &obj))
    {
        struct json_object *img;
        if (json_object_object_get_ex(obj, "image_72", &img))
        {
            free(bot->avatar_72);
            bot->avatar_72 = strdup(json_object_get_string(img));
        }
    }

    /* bots.list entries use "name"; some payloads use username */
    if (json_object_object_get_ex(json, "username", &obj) ||
        json_object_object_get_ex(json, "bot_user_id", &obj))
    {
        /* prefer explicit name fields already handled */
    }
}

struct t_slack_user *
slack_user_search_name(const char *name)
{
    struct t_slack_user *user;
    const char *q;

    if (!name || !name[0])
        return NULL;

    q = name;
    if (q[0] == '@')
        q++;

    /* exact id */
    user = slack_user_search(q);
    if (user)
        return user;

    for (user = slack_user_list; user; user = user->next)
    {
        if (user->name && weechat_strcasecmp(user->name, q) == 0)
            return user;
        if (user->display_name && weechat_strcasecmp(user->display_name, q) == 0)
            return user;
        if (user->real_name && weechat_strcasecmp(user->real_name, q) == 0)
            return user;
    }
    return NULL;
}

void
slack_bot_free(struct t_slack_bot *bot)
{
    if (!bot)
        return;

    if (bot->prev)
        bot->prev->next = bot->next;
    else
        slack_bot_list = bot->next;

    if (bot->next)
        bot->next->prev = bot->prev;

    free(bot->id);
    free(bot->name);
    free(bot->real_name);
    free(bot->username);
    free(bot->avatar_72);
    free(bot);
}

/* ============================================================
 * SlackSubteam
 * ============================================================ */

static struct t_slack_subteam *slack_subteam_list = NULL;

struct t_slack_subteam *
slack_subteam_new(const char *id, const char *name)
{
    struct t_slack_subteam *subteam;

    if (!id || !name)
        return NULL;

    subteam = slack_subteam_search(id);
    if (subteam)
        return subteam;

    subteam = calloc(1, sizeof(struct t_slack_subteam));
    if (!subteam)
        return NULL;

    subteam->id = strdup(id);
    subteam->name = strdup(name);

    subteam->next = slack_subteam_list;
    subteam->prev = NULL;
    if (slack_subteam_list)
        slack_subteam_list->prev = subteam;
    slack_subteam_list = subteam;

    return subteam;
}

struct t_slack_subteam *
slack_subteam_search(const char *id)
{
    struct t_slack_subteam *subteam;

    if (!id)
        return NULL;

    for (subteam = slack_subteam_list; subteam; subteam = subteam->next)
    {
        if (strcmp(subteam->id, id) == 0)
            return subteam;
    }
    return NULL;
}

struct t_slack_subteam *
slack_subteam_list_global(void)
{
    return slack_subteam_list;
}

void
slack_subteam_free(struct t_slack_subteam *subteam)
{
    if (!subteam)
        return;

    if (subteam->prev)
        subteam->prev->next = subteam->next;
    else
        slack_subteam_list = subteam->next;

    if (subteam->next)
        subteam->next->prev = subteam->prev;

    free(subteam->id);
    free(subteam->name);
    free(subteam->handle);
    free(subteam->description);
    for (int i = 0; i < subteam->members_count; i++)
        free(subteam->members[i]);
    free(subteam->members);
    free(subteam);
}

void
slack_subteam_update(struct t_slack_subteam *subteam, struct json_object *json)
{
    struct json_object *obj;

    if (!subteam || !json)
        return;

    if (json_object_object_get_ex(json, "handle", &obj))
    {
        free(subteam->handle);
        subteam->handle = strdup(json_object_get_string(obj));
    }

    if (json_object_object_get_ex(json, "members", &obj))
    {
        int count = json_object_array_length(obj);
        char **members = calloc(count, sizeof(char *));
        if (!members)
            return;

        for (int i = 0; i < count; i++)
        {
            struct json_object *member = json_object_array_get_idx(obj, i);
            members[i] = strdup(json_object_get_string(member));
        }

        for (int i = 0; i < subteam->members_count; i++)
            free(subteam->members[i]);
        free(subteam->members);

        subteam->members = members;
        subteam->members_count = count;
    }
}

/* ============================================================
 * SlackReaction
 * ============================================================ */

static void
slack_reaction_free(SlackReaction *r)
{
    if (!r)
        return;
    free(r->name);
    for (int i = 0; i < r->users_count; i++)
        free(r->users[i]);
    free(r->users);
    free(r);
}

static SlackReaction *
slack_reaction_new(const char *name)
{
    SlackReaction *r = calloc(1, sizeof(SlackReaction));
    if (!r)
        return NULL;
    r->name = strdup(name);
    return r;
}

/* ============================================================
 * SlackMessage
 * ============================================================ */

struct t_slack_message *
slack_message_new(SlackTS ts, const char *user_id, const char *text)
{
    struct t_slack_message *msg;

    msg = calloc(1, sizeof(struct t_slack_message));
    if (!msg)
        return NULL;

    msg->ts = ts;
    msg->user_id = user_id ? strdup(user_id) : NULL;
    msg->text = text ? strdup(text) : NULL;

    return msg;
}

struct t_slack_message *
slack_message_search(struct t_slack_message *list, SlackTS ts)
{
    struct t_slack_message *msg;

    for (msg = list; msg; msg = msg->next)
    {
        if (slack_ts_eq(msg->ts, ts))
            return msg;
    }
    return NULL;
}

static void
slack_sha1_hex(const char *s, char out_hex[SHA_DIGEST_LENGTH * 2 + 1])
{
    unsigned char md[SHA_DIGEST_LENGTH];
    int i;

    SHA1((const unsigned char *)(s ? s : ""), s ? strlen(s) : 0, md);
    for (i = 0; i < SHA_DIGEST_LENGTH; i++)
        snprintf(out_hex + i * 2, 3, "%02x", md[i]);
    out_hex[SHA_DIGEST_LENGTH * 2] = '\0';
}

void
slack_message_assign_hash(struct t_slack_message *list,
                          struct t_slack_message *msg)
{
    char *ts_str;
    char full[SHA_DIGEST_LENGTH * 2 + 1];
    char candidate[SHA_DIGEST_LENGTH * 2 + 1];
    int len;
    struct t_slack_message *m;

    if (!msg)
        return;
    if (msg->hash)
        return;

    ts_str = slack_ts_to_string(msg->ts);
    if (!ts_str)
        return;
    slack_sha1_hex(ts_str, full);
    free(ts_str);

    /* wee-slack: start at 3 hex chars, grow until unique among short hashes */
    for (len = 3; len <= SHA_DIGEST_LENGTH * 2; len++)
    {
        int conflict = 0;

        memcpy(candidate, full, (size_t)len);
        candidate[len] = '\0';

        for (m = list; m; m = m->next)
        {
            if (m == msg || !m->hash)
                continue;
            /* collide if either is a prefix of the other at this length */
            if (strncmp(m->hash, candidate, (size_t)len) == 0 ||
                strncmp(candidate, m->hash, strlen(m->hash)) == 0)
            {
                conflict = 1;
                break;
            }
        }
        if (!conflict)
        {
            free(msg->hash);
            msg->hash = strdup(candidate);
            return;
        }
    }
    /* fallback full hash */
    free(msg->hash);
    msg->hash = strdup(full);
}

struct t_slack_message *
slack_message_search_hash(struct t_slack_message *list,
                           const char *hash_or_dollar)
{
    const char *h;
    size_t hlen;
    struct t_slack_message *m, *found = NULL;
    int matches = 0;

    if (!hash_or_dollar || !hash_or_dollar[0])
        return NULL;
    h = hash_or_dollar;
    if (h[0] == '$')
        h++;
    if (!h[0])
        return NULL;
    hlen = strlen(h);

    /* exact or unique prefix among assigned hashes */
    for (m = list; m; m = m->next)
    {
        if (!m->hash)
            continue;
        if (strcmp(m->hash, h) == 0)
            return m;
        if (strncmp(m->hash, h, hlen) == 0)
        {
            found = m;
            matches++;
        }
    }
    return (matches == 1) ? found : NULL;
}

struct t_slack_message *
slack_message_from_ref(struct t_slack_message *list, const char *ref,
                        t_slack_msg_filter filter, void *filter_data)
{
    struct t_slack_message *m;
    int index, n;

    if (!list)
        return NULL;

    if (ref && ref[0])
    {
        /* full timestamp seconds.microseconds */
        if (strchr(ref, '.') && ref[0] >= '0' && ref[0] <= '9')
        {
            SlackTS ts = slack_ts_new(ref);
            m = slack_message_search(list, ts);
            if (m && (!filter || filter(m, filter_data)))
                return m;
        }

        /* $hash or hex hash */
        if (ref[0] == '$' || (isxdigit((unsigned char)ref[0]) &&
                               !strchr(ref, '.') && strlen(ref) >= 3 &&
                               strspn(ref, "0123456789abcdefABCDEF$") == strlen(ref)))
        {
            m = slack_message_search_hash(list, ref);
            if (m && (!filter || filter(m, filter_data)))
                return m;
        }

        /* numeric index (1 = newest matching) */
        if (ref[0] >= '1' && ref[0] <= '9')
        {
            char *end = NULL;
            long v = strtol(ref, &end, 10);
            if (end && *end == '\0' && v > 0 && v < 100000)
            {
                index = (int)v;
                n = 0;
                for (m = list; m; m = m->next)
                {
                    if (filter && !filter(m, filter_data))
                        continue;
                    if (++n == index)
                        return m;
                }
                return NULL;
            }
        }

        return NULL;
    }

    /* no ref → first matching (newest) */
    for (m = list; m; m = m->next)
    {
        if (filter && !filter(m, filter_data))
            continue;
        return m;
    }
    return NULL;
}

void
slack_message_free(struct t_slack_message *msg)
{
    if (!msg)
        return;

    free(msg->user_id);
    free(msg->text);
    free(msg->subtype);
    free(msg->thread_ts);
    free(msg->hash);
    if (msg->json)
        json_object_put(msg->json);

    SlackReaction *r = msg->reactions;
    while (r)
    {
        SlackReaction *next = r->next;
        slack_reaction_free(r);
        r = next;
    }

    free(msg);
}

void
slack_message_update(struct t_slack_message *msg, struct json_object *json)
{
    struct json_object *obj;

    if (!msg || !json)
        return;

    if (msg->json)
        json_object_put(msg->json);
    msg->json = json_object_get(json);

    if (json_object_object_get_ex(json, "text", &obj))
    {
        free(msg->text);
        msg->text = strdup(json_object_get_string(obj));
    }

    if (json_object_object_get_ex(json, "user", &obj))
    {
        free(msg->user_id);
        msg->user_id = strdup(json_object_get_string(obj));
    }

    if (json_object_object_get_ex(json, "subtype", &obj))
    {
        free(msg->subtype);
        msg->subtype = strdup(json_object_get_string(obj));
    }

    if (json_object_object_get_ex(json, "thread_ts", &obj))
    {
        free(msg->thread_ts);
        msg->thread_ts = strdup(json_object_get_string(obj));
    }

    if (json_object_object_get_ex(json, "reply_count", &obj))
        msg->reply_count = json_object_get_int(obj);

    if (json_object_object_get_ex(json, "reply_users_count", &obj))
        msg->reply_users_count = json_object_get_int(obj);

    if (json_object_object_get_ex(json, "subscribed", &obj))
        msg->subscribed = json_object_get_boolean(obj);

    if (json_object_object_get_ex(json, "deleted", &obj))
        msg->is_deleted = json_object_get_boolean(obj);

    /* update reactions */
    SlackReaction *old_reactions = msg->reactions;
    msg->reactions = NULL;

    if (json_object_object_get_ex(json, "reactions", &obj))
    {
        int count = json_object_array_length(obj);
        SlackReaction *tail = NULL;

        for (int i = 0; i < count; i++)
        {
            struct json_object *r_obj = json_object_array_get_idx(obj, i);
            struct json_object *name_obj;
            if (!json_object_object_get_ex(r_obj, "name", &name_obj))
                continue;

            SlackReaction *r = slack_reaction_new(json_object_get_string(name_obj));
            if (!r)
                continue;

            struct json_object *users_obj;
            if (json_object_object_get_ex(r_obj, "users", &users_obj))
            {
                r->users_count = json_object_array_length(users_obj);
                r->users = calloc(r->users_count, sizeof(char *));
                for (int j = 0; j < r->users_count; j++)
                {
                    struct json_object *u = json_object_array_get_idx(users_obj, j);
                    r->users[j] = strdup(json_object_get_string(u));
                }
            }

            r->next = NULL;
            if (tail)
                tail->next = r;
            else
                msg->reactions = r;
            tail = r;
        }
    }

    /* free old reactions */
    while (old_reactions)
    {
        SlackReaction *next = old_reactions->next;
        slack_reaction_free(old_reactions);
        old_reactions = next;
    }
}

struct t_slack_message *
slack_message_prepend(struct t_slack_message *list, struct t_slack_message *msg)
{
    if (!msg)
        return list;

    msg->next = list;
    msg->prev = NULL;
    if (list)
        list->prev = msg;

    /* Short hash after link (msg is list head; uniqueness scans next) */
    free(msg->hash);
    msg->hash = NULL;
    slack_message_assign_hash(msg, msg);

    return msg;
}

void
slack_message_reaction_add(struct t_slack_message *msg,
                           const char *name,
                           const char *user_id)
{
    SlackReaction *r;
    int i;

    if (!msg || !name || !name[0] || !user_id || !user_id[0])
        return;

    for (r = msg->reactions; r; r = r->next)
    {
        if (r->name && strcmp(r->name, name) == 0)
            break;
    }

    if (!r)
    {
        r = slack_reaction_new(name);
        if (!r)
            return;
        r->next = msg->reactions;
        msg->reactions = r;
    }

    for (i = 0; i < r->users_count; i++)
    {
        if (r->users[i] && strcmp(r->users[i], user_id) == 0)
            return; /* already present */
    }

    {
        char **nu = realloc(r->users, (size_t)(r->users_count + 1) * sizeof(char *));
        if (!nu)
            return;
        r->users = nu;
        r->users[r->users_count] = strdup(user_id);
        if (!r->users[r->users_count])
            return;
        r->users_count++;
    }
}

void
slack_message_reaction_remove(struct t_slack_message *msg,
                              const char *name,
                              const char *user_id)
{
    SlackReaction *r, *prev = NULL;
    int i, j;

    if (!msg || !name || !name[0] || !user_id || !user_id[0])
        return;

    for (r = msg->reactions; r; prev = r, r = r->next)
    {
        if (r->name && strcmp(r->name, name) == 0)
            break;
    }
    if (!r)
        return;

    for (i = 0; i < r->users_count; i++)
    {
        if (r->users[i] && strcmp(r->users[i], user_id) == 0)
            break;
    }
    if (i >= r->users_count)
        return;

    free(r->users[i]);
    for (j = i; j < r->users_count - 1; j++)
        r->users[j] = r->users[j + 1];
    r->users_count--;

    if (r->users_count == 0)
    {
        if (prev)
            prev->next = r->next;
        else
            msg->reactions = r->next;
        slack_reaction_free(r);
    }
}

/* ============================================================
 * SlackChannel
 * ============================================================ */

static struct t_slack_channel *slack_channel_list = NULL;

const char *
slack_channel_type_string(enum slack_channel_type type)
{
    switch (type)
    {
        case SLACK_CHANNEL_TYPE_CHANNEL: return "channel";
        case SLACK_CHANNEL_TYPE_GROUP:   return "group";
        case SLACK_CHANNEL_TYPE_DM:      return "dm";
        case SLACK_CHANNEL_TYPE_MPDM:    return "mpdm";
        case SLACK_CHANNEL_TYPE_THREAD:  return "thread";
    }
    return "unknown";
}

struct t_slack_channel *
slack_channel_new(const char *id, const char *name,
                  enum slack_channel_type type)
{
    struct t_slack_channel *channel;

    if (!id || !name)
        return NULL;

    channel = slack_channel_search(id);
    if (channel)
        return channel;

    channel = calloc(1, sizeof(struct t_slack_channel));
    if (!channel)
        return NULL;

    channel->id = strdup(id);
    channel->name = strdup(name);
    channel->type = type;
    channel->last_read = slack_ts_zero();

    channel->next = slack_channel_list;
    channel->prev = NULL;
    if (slack_channel_list)
        slack_channel_list->prev = channel;
    slack_channel_list = channel;

    return channel;
}

struct t_slack_channel *
slack_channel_search(const char *id)
{
    struct t_slack_channel *channel;

    if (!id)
        return NULL;

    for (channel = slack_channel_list; channel; channel = channel->next)
    {
        if (strcmp(channel->id, id) == 0)
            return channel;
    }
    return NULL;
}

struct t_slack_channel *
slack_channel_list_global(void)
{
    return slack_channel_list;
}

void
slack_channel_free(struct t_slack_channel *channel)
{
    if (!channel)
        return;

    if (channel->prev)
        channel->prev->next = channel->next;
    else
        slack_channel_list = channel->next;

    if (channel->next)
        channel->next->prev = channel->prev;

    free(channel->id);
    free(channel->name);
    free(channel->topic);
    free(channel->purpose);
    free(channel->user_id);
    free(channel->last_message_ts);
    free(channel->typing_user);
    if (channel->typing_clear_hook)
        weechat_unhook(channel->typing_clear_hook);

    struct t_slack_message *msg = channel->messages;
    while (msg)
    {
        struct t_slack_message *next = msg->next;
        slack_message_free(msg);
        msg = next;
    }

    free(channel);
}

void
slack_channel_update(struct t_slack_channel *channel, struct json_object *json)
{
    struct json_object *obj;

    if (!channel || !json)
        return;

    if (json_object_object_get_ex(json, "name", &obj))
    {
        const char *n = json_object_get_string(obj);
        /* IMs have no name field; do not wipe a resolved display name */
        if (n && n[0])
        {
            free(channel->name);
            channel->name = strdup(n);
        }
    }

    /* peer for 1:1 DMs */
    if (json_object_object_get_ex(json, "user", &obj))
    {
        const char *uid = json_object_get_string(obj);
        if (uid && uid[0])
        {
            free(channel->user_id);
            channel->user_id = strdup(uid);
        }
    }

    if (json_object_object_get_ex(json, "topic", &obj))
    {
        struct json_object *value;
        if (json_object_object_get_ex(obj, "value", &value))
        {
            free(channel->topic);
            channel->topic = strdup(json_object_get_string(value));
        }
    }

    if (json_object_object_get_ex(json, "purpose", &obj))
    {
        struct json_object *value;
        if (json_object_object_get_ex(obj, "value", &value))
        {
            free(channel->purpose);
            channel->purpose = strdup(json_object_get_string(value));
        }
    }

    if (json_object_object_get_ex(json, "is_member", &obj))
        channel->is_member = json_object_get_boolean(obj);

    if (json_object_object_get_ex(json, "last_read", &obj))
        channel->last_read = slack_ts_new(json_object_get_string(obj));

    if (json_object_object_get_ex(json, "unread_count", &obj))
        channel->unread_count = json_object_get_int(obj);
}

/* ============================================================
 * Thread channels
 * ============================================================ */

struct t_slack_channel *
slack_thread_channel_find(struct t_slack_channel *parent, const char *thread_ts)
{
    if (!parent || !thread_ts)
        return NULL;

    /* thread channels are stored in the global list with IDs like "thread_<parent_id>_<thread_ts>" */
    char thread_id[256];
    snprintf(thread_id, sizeof(thread_id), "thread_%s_%s", parent->id, thread_ts);

    return slack_channel_search(thread_id);
}

struct t_slack_channel *
slack_thread_channel_create(struct t_slack_channel *parent,
                             const char *thread_ts,
                             const char *topic)
{
    if (!parent || !thread_ts)
        return NULL;

    struct t_slack_channel *existing = slack_thread_channel_find(parent, thread_ts);
    if (existing)
        return existing;

    char thread_id[256];
    snprintf(thread_id, sizeof(thread_id), "thread_%s_%s", parent->id, thread_ts);

    char thread_name[512];
    snprintf(thread_name, sizeof(thread_name), "%s.%s", parent->name, thread_ts);

    struct t_slack_channel *thread = slack_channel_new(thread_id, thread_name,
                                                        SLACK_CHANNEL_TYPE_THREAD);
    if (!thread)
        return NULL;

    if (topic && topic[0])
    {
        free(thread->topic);
        thread->topic = strdup(topic);
    }

    return thread;
}
