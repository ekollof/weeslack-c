#ifndef SLACK_CACHE_H
#define SLACK_CACHE_H

#include <json-c/json.h>
#include <stddef.h>

struct t_weeslack_workspace;

/*
 * LMDB cache (Xepher-style MDB_NOSUBDIR per workspace).
 *
 *   <data_dir>/weeslack/<team_key>/cache.lmdb
 *
 * DBIs:
 *   (default) messages:  channel_id \0 ts  → message JSON
 *   "user"             user_id            → user JSON (from users.list)
 *   "emoji"             name               → url or :alias: value
 */

extern int slack_cache_open(struct t_weeslack_workspace *workspace);
extern void slack_cache_close(struct t_weeslack_workspace *workspace);
extern int slack_cache_ready(struct t_weeslack_workspace *workspace);

/* ---- messages ---- */

extern int slack_cache_put_message(struct t_weeslack_workspace *workspace,
                                    const char *channel_id,
                                    struct json_object *msg_json);

/*
 * Load up to limit messages, oldest-first.
 * max_ts_out / min_ts_out: newest/oldest ts (malloc; caller frees) if count>0.
 */
extern int slack_cache_load_channel(struct t_weeslack_workspace *workspace,
                                     const char *channel_id,
                                     int limit,
                                     struct json_object ***out_msgs,
                                     char **max_ts_out,
                                     char **min_ts_out);

extern void slack_cache_clear_channel(struct t_weeslack_workspace *workspace,
                                       const char *channel_id);

/* ---- directory (users / custom emoji) ---- */

extern int slack_cache_put_user(struct t_weeslack_workspace *workspace,
                                 struct json_object *user_json);
extern int slack_cache_put_emoji(struct t_weeslack_workspace *workspace,
                                  const char *name, const char *value);

/*
 * Load all cached users; callback(user_json, data) for each.
 * Returns count loaded.
 */
extern int slack_cache_foreach_user(
    struct t_weeslack_workspace *workspace,
    void (*cb)(struct json_object *user_json, void *data),
    void *data);

/*
 * Load all cached emoji; callback(name, value, data) for each.
 * Returns count loaded.
 */
extern int slack_cache_foreach_emoji(
    struct t_weeslack_workspace *workspace,
    void (*cb)(const char *name, const char *value, void *data),
    void *data);

/* Clear user/emoji DBIs (before full refresh write-through). */
extern void slack_cache_clear_users(struct t_weeslack_workspace *workspace);
extern void slack_cache_clear_emoji(struct t_weeslack_workspace *workspace);

#endif
