/*
 * slack_cache — LMDB-backed message history seed (rate-limit relief).
 *
 * Layout (MDB_NOSUBDIR file):
 *   <data_dir>/weeslack/<team_key>/cache.lmdb
 *
 * Key:   channel_id + '\0' + ts   (lexicographic → sorted by channel, then ts)
 * Value: JSON text of the Slack message object
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
    SLACK_CACHE_MAP_MIB     = 256,  /* map size 256 MiB per workspace */
    SLACK_CACHE_KEY_MAX     = 192,
    SLACK_CACHE_DEFAULT_LIM = 200
};

struct t_slack_cache
{
    MDB_env *env;
    MDB_dbi dbi;
    char *path;
    int dbi_open;
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
slack_cache_build_key(char *key, size_t key_size,
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
    /* no trailing NUL required in LMDB key; length is need */
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
    mdb_env_set_maxdbs(c->env, 4);

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

    rc = mdb_dbi_open(txn, NULL, 0, &c->dbi);
    if (rc != 0)
    {
        mdb_txn_abort(txn);
        mdb_env_close(c->env);
        free(c);
        return 0;
    }
    mdb_txn_commit(txn);
    c->dbi_open = 1;
    c->path = strdup(path);
    workspace->cache = c;

    weeslack_debug_at(3, "cache open %s", path);
    return 1;
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
        if (c->dbi_open)
            mdb_dbi_close(c->env, c->dbi);
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

/* ---- put ---- */

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

    if (!slack_cache_build_key(key, sizeof(key), channel_id, ts, &klen))
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

    rc = mdb_put(txn, c->dbi, &k, &v, 0);
    if (rc != 0)
    {
        mdb_txn_abort(txn);
        return 0;
    }
    mdb_txn_commit(txn);
    return 1;
}

/* ---- load (oldest-first, up to limit newest messages) ---- */

int
slack_cache_load_channel(struct t_weeslack_workspace *workspace,
                          const char *channel_id,
                          int limit,
                          struct json_object ***out_msgs,
                          char **max_ts_out)
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
    struct json_object **tmp;
    char *max_ts = NULL;

    if (out_msgs)
        *out_msgs = NULL;
    if (max_ts_out)
        *max_ts_out = NULL;

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

    /* Seek just after this channel's keyspace: channel\0\xff... */
    if (plen + 2 >= sizeof(start_key))
        return -1;
    memcpy(start_key, channel_id, plen);
    start_key[plen] = '\0';
    start_key[plen + 1] = (char)0xff;

    rc = mdb_txn_begin(c->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0)
        return -1;
    rc = mdb_cursor_open(txn, c->dbi, &cur);
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
    /* SET_RANGE may land past our prefix or on first key of next channel.
     * Walk backward with PREV from that position (or LAST if not found). */
    if (rc == MDB_NOTFOUND)
        rc = mdb_cursor_get(cur, &k, &v, MDB_LAST);
    else if (rc == 0)
    {
        /* If landed on or after next channel, step back once */
        if (k.mv_size < plen + 1 ||
            memcmp(k.mv_data, prefix, plen + 1) != 0)
            rc = mdb_cursor_get(cur, &k, &v, MDB_PREV);
    }

    while (rc == 0 && n < limit)
    {
        const char *kdata = k.mv_data;
        size_t ksize = k.mv_size;
        const char *ts;
        char *json_buf;
        struct json_object *obj;

        if (ksize < plen + 1 + 1 ||
            memcmp(kdata, prefix, plen + 1) != 0)
            break; /* left this channel's keyspace */

        ts = kdata + plen + 1;
        if (!max_ts && ts[0])
        {
            size_t tlen = ksize - (plen + 1);
            max_ts = malloc(tlen + 1);
            if (max_ts)
            {
                memcpy(max_ts, ts, tlen);
                max_ts[tlen] = '\0';
            }
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

        /* Collect newest-first; reverse later for paint order. */
        if (n >= cap)
        {
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
        return 0;
    }

    /* Reverse to oldest-first for history paint */
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

    rc = mdb_txn_begin(c->env, NULL, 0, &txn);
    if (rc != 0)
        return;
    rc = mdb_cursor_open(txn, c->dbi, &cur);
    if (rc != 0)
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
        rc = mdb_cursor_del(cur, 0);
        if (rc != 0)
            break;
        rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT);
    }

    mdb_cursor_close(cur);
    mdb_txn_commit(txn);
}
