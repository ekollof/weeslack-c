/*
 * slack_cache — LMDB history + directory seed (rate-limit relief).
 *
 *   <data_dir>/weeslack/<team_key>/cache.lmdb   (MDB_NOSUBDIR)
 *
 * Messages (default DBI): channel_id \0 ts → JSON
 * Users ("user"):         user_id → JSON
 * Emoji ("emoji"):        name → url or :alias:
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <lmdb.h>

#include "weeslack.h"
#include "slack_cache.h"

enum
{
    SLACK_CACHE_MAP_MIB     = 256,
    SLACK_CACHE_KEY_MAX     = 192,
    SLACK_CACHE_DEFAULT_LIM = 200
};

struct t_slack_cache
{
    MDB_env *env;
    MDB_dbi dbi_msg;
    MDB_dbi dbi_user;
    MDB_dbi dbi_emoji;
    char *path;
    int open;
};

/* ---- helpers ---- */

static const char *
slack_cache_team_key(struct t_weeslack_workspace *ws)
{
    if (!ws)
        return "default";
    if (ws->domain && ws->domain[0])
        return ws->domain;
    if (ws->id && ws->id[0])
        return ws->id;
    return "default";
}

static int
slack_cache_enabled(void)
{
    if (!weeslack_config.history_cache)
        return 0;
    return weechat_config_boolean(weeslack_config.history_cache);
}

static int
slack_cache_max_msgs(void)
{
    int n;

    if (!weeslack_config.history_cache_max)
        return SLACK_CACHE_DEFAULT_LIM;
    n = weechat_config_integer(weeslack_config.history_cache_max);
    if (n < 50)
        return 50;
    if (n > 5000)
        return 5000;
    return n;
}

static void
slack_cache_sanitize(char *s)
{
    if (!s)
        return;
    for (; *s; s++)
    {
        if (*s == '/' || *s == '\\' || (unsigned char)*s < 32)
            *s = '_';
    }
}

static int
slack_cache_build_msg_key(char *key, size_t key_size,
                           const char *channel_id, const char *ts,
                           size_t *out_len)
{
    size_t clen, tlen, need;

    if (!key || !channel_id || !ts || !channel_id[0] || !ts[0])
        return 0;
    clen = strlen(channel_id);
    tlen = strlen(ts);
    need = clen + 1 + tlen;
    if (need >= key_size)
        return 0;
    memcpy(key, channel_id, clen);
    key[clen] = '\0';
    memcpy(key + clen + 1, ts, tlen);
    if (out_len)
        *out_len = need;
    return 1;
}

/* ---- open / close ---- */

int
slack_cache_open(struct t_weeslack_workspace *workspace)
{
    struct t_slack_cache *c;
    const char *data_dir;
    char dir[640];
    char path[704];
    char team[128];
    MDB_txn *txn = NULL;
    int rc;
    size_t mapsize;

    if (!workspace)
        return 0;
    if (!slack_cache_enabled())
        return 0;
    if (workspace->cache)
        return 1;

    data_dir = weechat_info_get("weechat_data_dir", "");
    if (!data_dir || !data_dir[0])
        data_dir = weechat_info_get("weechat_config_dir", "");
    if (!data_dir || !data_dir[0])
        return 0;

    snprintf(team, sizeof(team), "%s", slack_cache_team_key(workspace));
    slack_cache_sanitize(team);

    snprintf(dir, sizeof(dir), "%s/weeslack/%s", data_dir, team);
    weechat_mkdir_parents(dir, 0755);
    snprintf(path, sizeof(path), "%s/cache.lmdb", dir);

    c = calloc(1, sizeof(*c));
    if (!c)
        return 0;

    rc = mdb_env_create(&c->env);
    if (rc != 0)
    {
        free(c);
        return 0;
    }

    mapsize = (size_t)SLACK_CACHE_MAP_MIB * 1024ULL * 1024ULL;
    mdb_env_set_mapsize(c->env, mapsize);
    mdb_env_set_maxdbs(c->env, 8);

    rc = mdb_env_open(c->env, path, MDB_NOSUBDIR, 0600);
    if (rc != 0)
    {
        weechat_printf(NULL,
                        "%sweeslack: LMDB open failed (%s): %s",
                        weechat_prefix("error"), path, mdb_strerror(rc));
        mdb_env_close(c->env);
        free(c);
        return 0;
    }

    rc = mdb_txn_begin(c->env, NULL, 0, &txn);
    if (rc != 0)
    {
        mdb_env_close(c->env);
        free(c);
        return 0;
    }

    /* Default DBI keeps phase-1 message keys. */
    rc = mdb_dbi_open(txn, NULL, 0, &c->dbi_msg);
    if (rc != 0)
        goto fail_txn;
    rc = mdb_dbi_open(txn, "user", MDB_CREATE, &c->dbi_user);
    if (rc != 0)
        goto fail_txn;
    rc = mdb_dbi_open(txn, "emoji", MDB_CREATE, &c->dbi_emoji);
    if (rc != 0)
        goto fail_txn;

    mdb_txn_commit(txn);
    c->open = 1;
    c->path = strdup(path);
    workspace->cache = c;
    weeslack_debug_at(3, "cache open %s", path);
    return 1;

fail_txn:
    mdb_txn_abort(txn);
    mdb_env_close(c->env);
    free(c);
    return 0;
}

void
slack_cache_close(struct t_weeslack_workspace *workspace)
{
    struct t_slack_cache *c;

    if (!workspace || !workspace->cache)
        return;
    c = workspace->cache;
    workspace->cache = NULL;

    if (c->env)
    {
        if (c->open)
        {
            mdb_dbi_close(c->env, c->dbi_msg);
            mdb_dbi_close(c->env, c->dbi_user);
            mdb_dbi_close(c->env, c->dbi_emoji);
        }
        mdb_env_close(c->env);
    }
    free(c->path);
    free(c);
}

int
slack_cache_ready(struct t_weeslack_workspace *workspace)
{
    if (!slack_cache_enabled())
        return 0;
    if (!workspace)
        return 0;
    if (!workspace->cache)
        slack_cache_open(workspace);
    return (workspace->cache != NULL);
}

/* ---- prune oldest messages in a channel ---- */

static void
slack_cache_prune_channel_txn(MDB_txn *txn, MDB_dbi dbi,
                               const char *channel_id, int max_keep)
{
    MDB_cursor *cur = NULL;
    MDB_val k, v;
    char prefix[SLACK_CACHE_KEY_MAX];
    size_t plen;
    int rc, count = 0, drop;

    if (!txn || !channel_id || max_keep < 1)
        return;

    plen = strlen(channel_id);
    if (plen + 1 >= sizeof(prefix))
        return;
    memcpy(prefix, channel_id, plen);
    prefix[plen] = '\0';

    if (mdb_cursor_open(txn, dbi, &cur) != 0)
        return;

    k.mv_data = prefix;
    k.mv_size = plen + 1;
    rc = mdb_cursor_get(cur, &k, &v, MDB_SET_RANGE);
    while (rc == 0)
    {
        if (k.mv_size < plen + 1 ||
            memcmp(k.mv_data, prefix, plen + 1) != 0)
            break;
        count++;
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
    }

    drop = count - max_keep;
    if (drop <= 0)
    {
        mdb_cursor_close(cur);
        return;
    }

    /* Delete oldest first (SET_RANGE = lowest ts). */
    k.mv_data = prefix;
    k.mv_size = plen + 1;
    rc = mdb_cursor_get(cur, &k, &v, MDB_SET_RANGE);
    while (rc == 0 && drop > 0)
    {
        if (k.mv_size < plen + 1 ||
            memcmp(k.mv_data, prefix, plen + 1) != 0)
            break;
        if (mdb_cursor_del(cur, 0) != 0)
            break;
        drop--;
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
    }
    mdb_cursor_close(cur);
}

/* ---- put message ---- */

int
slack_cache_put_message(struct t_weeslack_workspace *workspace,
                         const char *channel_id,
                         struct json_object *msg_json)
{
    struct t_slack_cache *c;
    struct json_object *ts_obj;
    const char *ts;
    const char *json_str;
    char key[SLACK_CACHE_KEY_MAX];
    size_t klen;
    MDB_txn *txn = NULL;
    MDB_val k, v;
    int rc;

    if (!slack_cache_ready(workspace) || !channel_id || !msg_json)
        return 0;
    c = workspace->cache;

    if (!json_object_object_get_ex(msg_json, "ts", &ts_obj))
        return 0;
    ts = json_object_get_string(ts_obj);
    if (!ts || !ts[0])
        return 0;

    if (!slack_cache_build_msg_key(key, sizeof(key), channel_id, ts, &klen))
        return 0;

    json_str = json_object_to_json_string_ext(msg_json,
                                               JSON_C_TO_STRING_PLAIN);
    if (!json_str)
        return 0;

    rc = mdb_txn_begin(c->env, NULL, 0, &txn);
    if (rc != 0)
        return 0;

    k.mv_data = key;
    k.mv_size = klen;
    v.mv_data = (void *)json_str;
    v.mv_size = strlen(json_str);

    rc = mdb_put(txn, c->dbi_msg, &k, &v, 0);
    if (rc != 0)
    {
        mdb_txn_abort(txn);
        return 0;
    }

    slack_cache_prune_channel_txn(txn, c->dbi_msg, channel_id,
                                   slack_cache_max_msgs());
    mdb_txn_commit(txn);
    return 1;
}

/* ---- load messages ---- */

int
slack_cache_load_channel(struct t_weeslack_workspace *workspace,
                          const char *channel_id,
                          int limit,
                          struct json_object ***out_msgs,
                          char **max_ts_out,
                          char **min_ts_out)
{
    struct t_slack_cache *c;
    MDB_txn *txn = NULL;
    MDB_cursor *cur = NULL;
    MDB_val k, v;
    char prefix[SLACK_CACHE_KEY_MAX];
    char start_key[SLACK_CACHE_KEY_MAX];
    size_t plen;
    int rc, n = 0, cap, i;
    struct json_object **msgs = NULL;
    char *max_ts = NULL;
    char *min_ts = NULL;

    if (out_msgs)
        *out_msgs = NULL;
    if (max_ts_out)
        *max_ts_out = NULL;
    if (min_ts_out)
        *min_ts_out = NULL;

    if (!slack_cache_ready(workspace) || !channel_id || !channel_id[0] ||
        !out_msgs)
        return -1;
    c = workspace->cache;

    if (limit < 1)
        limit = slack_cache_max_msgs();
    if (limit > slack_cache_max_msgs())
        limit = slack_cache_max_msgs();

    plen = strlen(channel_id);
    if (plen + 2 >= sizeof(prefix))
        return -1;
    memcpy(prefix, channel_id, plen);
    prefix[plen] = '\0';

    if (plen + 2 >= sizeof(start_key))
        return -1;
    memcpy(start_key, channel_id, plen);
    start_key[plen] = '\0';
    start_key[plen + 1] = (char)0xff;

    rc = mdb_txn_begin(c->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0)
        return -1;
    rc = mdb_cursor_open(txn, c->dbi_msg, &cur);
    if (rc != 0)
    {
        mdb_txn_abort(txn);
        return -1;
    }

    cap = limit;
    msgs = calloc((size_t)cap, sizeof(*msgs));
    if (!msgs)
    {
        mdb_cursor_close(cur);
        mdb_txn_abort(txn);
        return -1;
    }

    k.mv_data = start_key;
    k.mv_size = plen + 2;
    rc = mdb_cursor_get(cur, &k, &v, MDB_SET_RANGE);
    if (rc == MDB_NOTFOUND)
        rc = mdb_cursor_get(cur, &k, &v, MDB_LAST);
    else if (rc == 0)
    {
        if (k.mv_size < plen + 1 ||
            memcmp(k.mv_data, prefix, plen + 1) != 0)
            rc = mdb_cursor_get(cur, &k, &v, MDB_PREV);
    }

    while (rc == 0 && n < limit)
    {
        const char *kdata = k.mv_data;
        size_t ksize = k.mv_size;
        const char *ts;
        size_t tlen;
        char *json_buf;
        struct json_object *obj;

        if (ksize < plen + 1 + 1 ||
            memcmp(kdata, prefix, plen + 1) != 0)
            break;

        ts = kdata + plen + 1;
        tlen = ksize - (plen + 1);

        /* First hit is newest (walking PREV). */
        if (!max_ts && tlen > 0)
        {
            max_ts = malloc(tlen + 1);
            if (max_ts)
            {
                memcpy(max_ts, ts, tlen);
                max_ts[tlen] = '\0';
            }
        }
        /* Keep updating min as we go older. */
        free(min_ts);
        min_ts = malloc(tlen + 1);
        if (min_ts)
        {
            memcpy(min_ts, ts, tlen);
            min_ts[tlen] = '\0';
        }

        json_buf = malloc(v.mv_size + 1);
        if (!json_buf)
            break;
        memcpy(json_buf, v.mv_data, v.mv_size);
        json_buf[v.mv_size] = '\0';
        obj = json_tokener_parse(json_buf);
        free(json_buf);
        if (!obj)
        {
            rc = mdb_cursor_get(cur, &k, &v, MDB_PREV);
            continue;
        }

        if (n >= cap)
        {
            struct json_object **tmp;
            cap *= 2;
            tmp = realloc(msgs, (size_t)cap * sizeof(*msgs));
            if (!tmp)
            {
                json_object_put(obj);
                break;
            }
            msgs = tmp;
        }
        msgs[n++] = obj;
        rc = mdb_cursor_get(cur, &k, &v, MDB_PREV);
    }

    mdb_cursor_close(cur);
    mdb_txn_abort(txn);

    if (n == 0)
    {
        free(msgs);
        free(max_ts);
        free(min_ts);
        return 0;
    }

    for (i = 0; i < n / 2; i++)
    {
        struct json_object *swap = msgs[i];
        msgs[i] = msgs[n - 1 - i];
        msgs[n - 1 - i] = swap;
    }

    *out_msgs = msgs;
    if (max_ts_out)
        *max_ts_out = max_ts;
    else
        free(max_ts);
    if (min_ts_out)
        *min_ts_out = min_ts;
    else
        free(min_ts);
    return n;
}

void
slack_cache_clear_channel(struct t_weeslack_workspace *workspace,
                           const char *channel_id)
{
    struct t_slack_cache *c;
    MDB_txn *txn = NULL;
    MDB_cursor *cur = NULL;
    MDB_val k, v;
    char prefix[SLACK_CACHE_KEY_MAX];
    size_t plen;
    int rc;

    if (!slack_cache_ready(workspace) || !channel_id)
        return;
    c = workspace->cache;

    plen = strlen(channel_id);
    if (plen + 1 >= sizeof(prefix))
        return;
    memcpy(prefix, channel_id, plen);
    prefix[plen] = '\0';

    if (mdb_txn_begin(c->env, NULL, 0, &txn) != 0)
        return;
    if (mdb_cursor_open(txn, c->dbi_msg, &cur) != 0)
    {
        mdb_txn_abort(txn);
        return;
    }

    k.mv_data = prefix;
    k.mv_size = plen + 1;
    rc = mdb_cursor_get(cur, &k, &v, MDB_SET_RANGE);
    while (rc == 0)
    {
        if (k.mv_size < plen + 1 ||
            memcmp(k.mv_data, prefix, plen + 1) != 0)
            break;
        if (mdb_cursor_del(cur, 0) != 0)
            break;
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
    }
    mdb_cursor_close(cur);
    mdb_txn_commit(txn);
}

/* ---- users / emoji ---- */

int
slack_cache_put_user(struct t_weeslack_workspace *workspace,
                      struct json_object *user_json)
{
    struct t_slack_cache *c;
    struct json_object *id_obj;
    const char *uid, *json_str;
    MDB_txn *txn = NULL;
    MDB_val k, v;
    int rc;

    if (!slack_cache_ready(workspace) || !user_json)
        return 0;
    c = workspace->cache;

    if (!json_object_object_get_ex(user_json, "id", &id_obj))
        return 0;
    uid = json_object_get_string(id_obj);
    if (!uid || !uid[0])
        return 0;

    json_str = json_object_to_json_string_ext(user_json,
                                               JSON_C_TO_STRING_PLAIN);
    if (!json_str)
        return 0;

    if (mdb_txn_begin(c->env, NULL, 0, &txn) != 0)
        return 0;
    k.mv_data = (void *)uid;
    k.mv_size = strlen(uid);
    v.mv_data = (void *)json_str;
    v.mv_size = strlen(json_str);
    rc = mdb_put(txn, c->dbi_user, &k, &v, 0);
    if (rc != 0)
    {
        mdb_txn_abort(txn);
        return 0;
    }
    mdb_txn_commit(txn);
    return 1;
}

int
slack_cache_put_emoji(struct t_weeslack_workspace *workspace,
                       const char *name, const char *value)
{
    struct t_slack_cache *c;
    MDB_txn *txn = NULL;
    MDB_val k, v;
    int rc;

    if (!slack_cache_ready(workspace) || !name || !name[0] || !value)
        return 0;
    c = workspace->cache;

    if (mdb_txn_begin(c->env, NULL, 0, &txn) != 0)
        return 0;
    k.mv_data = (void *)name;
    k.mv_size = strlen(name);
    v.mv_data = (void *)value;
    v.mv_size = strlen(value);
    rc = mdb_put(txn, c->dbi_emoji, &k, &v, 0);
    if (rc != 0)
    {
        mdb_txn_abort(txn);
        return 0;
    }
    mdb_txn_commit(txn);
    return 1;
}

int
slack_cache_foreach_user(struct t_weeslack_workspace *workspace,
                          void (*cb)(struct json_object *user_json, void *data),
                          void *data)
{
    struct t_slack_cache *c;
    MDB_txn *txn = NULL;
    MDB_cursor *cur = NULL;
    MDB_val k, v;
    int rc, n = 0;

    if (!slack_cache_ready(workspace) || !cb)
        return 0;
    c = workspace->cache;

    if (mdb_txn_begin(c->env, NULL, MDB_RDONLY, &txn) != 0)
        return 0;
    if (mdb_cursor_open(txn, c->dbi_user, &cur) != 0)
    {
        mdb_txn_abort(txn);
        return 0;
    }

    rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
    while (rc == 0)
    {
        char *json_buf;
        struct json_object *obj;

        json_buf = malloc(v.mv_size + 1);
        if (!json_buf)
            break;
        memcpy(json_buf, v.mv_data, v.mv_size);
        json_buf[v.mv_size] = '\0';
        obj = json_tokener_parse(json_buf);
        free(json_buf);
        if (obj)
        {
            cb(obj, data);
            json_object_put(obj);
            n++;
        }
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
    }

    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    return n;
}

int
slack_cache_foreach_emoji(struct t_weeslack_workspace *workspace,
                           void (*cb)(const char *name, const char *value,
                                      void *data),
                           void *data)
{
    struct t_slack_cache *c;
    MDB_txn *txn = NULL;
    MDB_cursor *cur = NULL;
    MDB_val k, v;
    int rc, n = 0;

    if (!slack_cache_ready(workspace) || !cb)
        return 0;
    c = workspace->cache;

    if (mdb_txn_begin(c->env, NULL, MDB_RDONLY, &txn) != 0)
        return 0;
    if (mdb_cursor_open(txn, c->dbi_emoji, &cur) != 0)
    {
        mdb_txn_abort(txn);
        return 0;
    }

    rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
    while (rc == 0)
    {
        char *name, *value;

        name = malloc(k.mv_size + 1);
        value = malloc(v.mv_size + 1);
        if (name && value)
        {
            memcpy(name, k.mv_data, k.mv_size);
            name[k.mv_size] = '\0';
            memcpy(value, v.mv_data, v.mv_size);
            value[v.mv_size] = '\0';
            cb(name, value, data);
            n++;
        }
        free(name);
        free(value);
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
    }

    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    return n;
}

static void
slack_cache_clear_dbi(struct t_slack_cache *c, MDB_dbi dbi)
{
    MDB_txn *txn = NULL;
    MDB_cursor *cur = NULL;
    MDB_val k, v;
    int rc;

    if (!c)
        return;
    if (mdb_txn_begin(c->env, NULL, 0, &txn) != 0)
        return;
    if (mdb_cursor_open(txn, dbi, &cur) != 0)
    {
        mdb_txn_abort(txn);
        return;
    }
    rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST);
    while (rc == 0)
    {
        if (mdb_cursor_del(cur, 0) != 0)
            break;
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
    }
    mdb_cursor_close(cur);
    mdb_txn_commit(txn);
}

void
slack_cache_clear_users(struct t_weeslack_workspace *workspace)
{
    if (!slack_cache_ready(workspace))
        return;
    slack_cache_clear_dbi(workspace->cache, workspace->cache->dbi_user);
}

void
slack_cache_clear_emoji(struct t_weeslack_workspace *workspace)
{
    if (!slack_cache_ready(workspace))
        return;
    slack_cache_clear_dbi(workspace->cache, workspace->cache->dbi_emoji);
}
