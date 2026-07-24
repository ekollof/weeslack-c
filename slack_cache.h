#ifndef SLACK_CACHE_H
#define SLACK_CACHE_H

#include <json-c/json.h>
#include <stddef.h>

struct t_weeslack_workspace;

/*
 * LMDB history cache (Xepher-style MDB_NOSUBDIR file per workspace).
 *
 * Path: <weechat_data_dir>/weeslack/<team_key>/cache.lmdb
 * Keys:  channel_id \0 ts  → compact JSON message object
 *
 * Write-through from history API + live RTM. Read paints seed on focus /
 * loadhistory so we avoid re-fetching full conversations.history pages.
 */

/* Open (or create) cache for workspace. Safe to call multiple times. */
extern int slack_cache_open(struct t_weeslack_workspace *workspace);

/* Close cache handle (plugin unload / workspace free). */
extern void slack_cache_close(struct t_weeslack_workspace *workspace);

/* 1 if caching enabled in config and env is open. */
extern int slack_cache_ready(struct t_weeslack_workspace *workspace);

/*
 * Store one message JSON for channel_id. Uses "ts" field as key.
 * Idempotent (same channel+ts overwrites).
 */
extern int slack_cache_put_message(struct t_weeslack_workspace *workspace,
                                    const char *channel_id,
                                    struct json_object *msg_json);

/*
 * Load up to limit messages for channel, oldest-first into *out_msgs
 * (array of json objects; caller json_object_put each + free array).
 * Returns count (>=0), or -1 on error.
 * If max_ts_out non-NULL, set to newest ts string (malloc; caller frees)
 * when count > 0.
 */
extern int slack_cache_load_channel(struct t_weeslack_workspace *workspace,
                                     const char *channel_id,
                                     int limit,
                                     struct json_object ***out_msgs,
                                     char **max_ts_out);

/* Drop all messages for a channel (optional; unused in phase 1). */
extern void slack_cache_clear_channel(struct t_weeslack_workspace *workspace,
                                       const char *channel_id);

#endif
