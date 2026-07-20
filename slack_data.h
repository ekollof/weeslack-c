#ifndef SLACK_DATA_H
#define SLACK_DATA_H

#include <weechat/weechat-plugin.h>
#include <json-c/json.h>
#include <time.h>

/* ---- SlackTS: Slack timestamp (seconds.microseconds) ---- */

typedef struct
{
    time_t sec;
    long usec;
} SlackTS;

extern SlackTS slack_ts_new(const char *ts_str);
extern SlackTS slack_ts_from_time(time_t t);
extern int slack_ts_cmp(SlackTS a, SlackTS b);
extern int slack_ts_eq(SlackTS a, SlackTS b);
extern int slack_ts_is_empty(SlackTS ts);
extern char *slack_ts_to_string(SlackTS ts);
extern SlackTS slack_ts_zero(void);

/* ---- SlackUser ---- */

/* Forward (workspace ownership for multi-team). */
struct t_weeslack_workspace;

struct t_slack_user
{
    char *id;
    char *name;
    char *real_name;
    char *display_name;
    char *color;
    char *avatar_72;
    int deleted;
    int is_bot;
    int is_app_user; /* Slack app / integration user (Google Drive, etc.) */
    int is_external;
    char *presence;
    char *status_emoji;
    char *status_text;
    struct json_object *profile_json;
    /* Owning workspace (set from that team's users.list / RTM). */
    struct t_weeslack_workspace *workspace;
    struct t_slack_user *next;
    struct t_slack_user *prev;
};

extern struct t_slack_user *slack_user_new(const char *id, const char *name,
                                           struct t_weeslack_workspace *workspace);
/* Prefer workspace match when ws non-NULL; else first id match. */
extern struct t_slack_user *slack_user_search(const char *id);
extern struct t_slack_user *slack_user_search_ws(
    struct t_weeslack_workspace *workspace, const char *id);
extern struct t_slack_user *slack_user_list_global(void);
extern void slack_user_free(struct t_slack_user *user);
extern void slack_user_free_workspace(struct t_weeslack_workspace *workspace);
extern const char *slack_user_get_color(struct t_slack_user *user);
/* display_name → real_name → @handle → id; never NULL if user non-NULL */
extern const char *slack_user_best_name(struct t_slack_user *user);
extern void slack_user_update(struct t_slack_user *user, struct json_object *json);
/* 1 = hide from channel nicklist (bots, apps, slackbot, deleted). */
extern int slack_user_hide_from_nicklist(struct t_slack_user *user);

/* ---- SlackBot ---- */

struct t_slack_bot
{
    char *id;
    char *name;
    char *real_name;
    char *username;
    char *avatar_72;
    int deleted;
    struct t_weeslack_workspace *workspace;
    struct t_slack_bot *next;
    struct t_slack_bot *prev;
};

extern struct t_slack_bot *slack_bot_new(const char *id, const char *name,
                                         struct t_weeslack_workspace *workspace);
extern struct t_slack_bot *slack_bot_search(const char *id);
extern struct t_slack_bot *slack_bot_search_ws(
    struct t_weeslack_workspace *workspace, const char *id);
extern struct t_slack_bot *slack_bot_list_global(void);
extern void slack_bot_free(struct t_slack_bot *bot);
extern void slack_bot_free_workspace(struct t_weeslack_workspace *workspace);
extern void slack_bot_update(struct t_slack_bot *bot, struct json_object *json);
extern const char *slack_bot_best_name(struct t_slack_bot *bot);

/* resolve user by id, @name, display_name, or real_name (case-insensitive) */
extern struct t_slack_user *slack_user_search_name(const char *name);
/* Prefer matches from workspace when non-NULL (multi-team name collisions). */
extern struct t_slack_user *slack_user_search_name_ws(
    struct t_weeslack_workspace *workspace, const char *name);

/* ---- SlackSubteam (user groups) ---- */

struct t_slack_subteam
{
    char *id;
    char *name;
    char *handle;
    char *description;
    char **members;
    int members_count;
    int is_usergroup;
    /* 1 if our user is in this usergroup (for highlights) */
    int is_member;
    struct t_weeslack_workspace *workspace;
    struct t_slack_subteam *next;
    struct t_slack_subteam *prev;
};

extern struct t_slack_subteam *slack_subteam_new(
    const char *id, const char *name,
    struct t_weeslack_workspace *workspace);
extern struct t_slack_subteam *slack_subteam_search(const char *id);
extern struct t_slack_subteam *slack_subteam_search_ws(
    struct t_weeslack_workspace *workspace, const char *id);
extern struct t_slack_subteam *slack_subteam_list_global(void);
extern void slack_subteam_free(struct t_slack_subteam *subteam);
extern void slack_subteam_free_workspace(struct t_weeslack_workspace *workspace);
extern void slack_subteam_update(struct t_slack_subteam *subteam,
                                 struct json_object *json);
/* Set is_member from members[] vs my_user_id (or explicit flag). */
extern void slack_subteam_set_member_flag(struct t_slack_subteam *subteam,
                                           const char *my_user_id);

/* ---- SlackMessage ---- */

typedef struct t_slack_reaction
{
    char *name;
    char **users;
    int users_count;
    struct t_slack_reaction *next;
} SlackReaction;

struct t_slack_message
{
    SlackTS ts;
    SlackTS last_read;
    char *user_id;
    char *text;
    char *subtype;
    char *thread_ts;
    /* Short id (wee-slack hashed_messages): "a1b" without leading $ */
    char *hash;
    int reply_count;
    int reply_users_count;
    int subscribed;
    int is_deleted;
    int is_edited;
    struct json_object *json;
    SlackReaction *reactions;
    struct t_slack_message *thread_parent;
    struct t_slack_message *next;
    struct t_slack_message *prev;
};

extern struct t_slack_message *slack_message_new(SlackTS ts, const char *user_id,
                                                  const char *text);
extern struct t_slack_message *slack_message_search(struct t_slack_message *list,
                                                     SlackTS ts);
/* Resolve $abc / abc short-hash (unique prefix match) within channel messages. */
extern struct t_slack_message *slack_message_search_hash(
    struct t_slack_message *list, const char *hash_or_dollar);
/*
 * Resolve message ref: full ts, $hash / hash, or 1-based index into list
 * (newest first). message_filter: if non-NULL, only count msgs with filter(msg)!=0.
 */
typedef int (*t_slack_msg_filter)(struct t_slack_message *msg, void *data);
extern struct t_slack_message *slack_message_from_ref(
    struct t_slack_message *list, const char *ref,
    t_slack_msg_filter filter, void *filter_data);
/* Assign unique short hash from SHA1(ts) like wee-slack (min 3 hex chars). */
extern void slack_message_assign_hash(struct t_slack_message *list,
                                      struct t_slack_message *msg);
extern void slack_message_free(struct t_slack_message *msg);
extern void slack_message_update(struct t_slack_message *msg,
                                 struct json_object *json);
extern struct t_slack_message *slack_message_prepend(struct t_slack_message *list,
                                                      struct t_slack_message *msg);
/* RTM reaction_added / reaction_removed — update in-memory reaction list */
extern void slack_message_reaction_add(struct t_slack_message *msg,
                                       const char *name,
                                       const char *user_id);
extern void slack_message_reaction_remove(struct t_slack_message *msg,
                                          const char *name,
                                          const char *user_id);

/* ---- SlackChannel ---- */

enum slack_channel_type
{
    SLACK_CHANNEL_TYPE_CHANNEL,
    SLACK_CHANNEL_TYPE_GROUP,
    SLACK_CHANNEL_TYPE_DM,
    SLACK_CHANNEL_TYPE_MPDM,
    SLACK_CHANNEL_TYPE_THREAD,
};

struct t_slack_channel
{
    char *id;
    char *name;
    char *topic;
    char *purpose;
    /* peer user id for 1:1 DMs (from conversations.list "user") */
    char *user_id;
    enum slack_channel_type type;
    int is_member;
    int is_muted;
    int is_subscribed;
    /* Shared / Slack Connect channel — uses look.shared_name_prefix */
    int is_shared;
    int unread_count;
    /* 0=not loaded, 1=queued, 2=in flight, 3=done (success or empty) */
    int history_state;
    int history_retries;
    int members_loaded;
    /* conversations.info fetched (topic/purpose fill-in) */
    int info_fetched;
    SlackTS last_read;
    /* last displayed message ts (string form) for /cslack linkarchive */
    char *last_message_ts;
    struct t_gui_buffer *buffer;
    struct t_slack_message *messages;
    /* typing indicator: clear hook + name currently typing */
    struct t_hook *typing_clear_hook;
    char *typing_user;
    /* multi-team: which workspace owns this channel model (may be NULL) */
    struct t_weeslack_workspace *workspace;
    struct t_slack_channel *next;
    struct t_slack_channel *prev;
};

extern struct t_slack_channel *slack_channel_new(const char *id, const char *name,
                                                   enum slack_channel_type type);
/* Create or update ownership pointer for multi-team cleanup. */
extern void slack_channel_set_workspace(struct t_slack_channel *channel,
                                         struct t_weeslack_workspace *workspace);
/* Free all channel models owned by workspace (after buffers closed). */
extern void slack_channel_free_workspace(struct t_weeslack_workspace *workspace);
extern struct t_slack_channel *slack_channel_search(const char *id);
extern struct t_slack_channel *slack_channel_list_global(void);
extern void slack_channel_free(struct t_slack_channel *channel);
extern void slack_channel_update(struct t_slack_channel *channel,
                                 struct json_object *json);
/* Topic if set, else purpose (wee-slack buffer title). Never empty string. */
extern const char *slack_channel_display_topic(const struct t_slack_channel *channel);
extern const char *slack_channel_type_string(enum slack_channel_type type);
extern struct t_slack_channel *slack_thread_channel_find(struct t_slack_channel *parent,
                                                          const char *thread_ts);
extern struct t_slack_channel *slack_thread_channel_create(struct t_slack_channel *parent,
                                                            const char *thread_ts,
                                                            const char *topic);

#endif
