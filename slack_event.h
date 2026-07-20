#ifndef SLACK_EVENT_H
#define SLACK_EVENT_H

#include <weechat/weechat-plugin.h>
#include <json-c/json.h>
#include "slack_data.h"

struct t_weeslack_workspace;
struct t_slack_channel;

extern void slack_event_handle(struct t_weeslack_workspace *workspace,
                                struct json_object *json);
extern void slack_event_handle_message(struct t_weeslack_workspace *workspace,
                                        struct t_slack_channel *channel,
                                        struct json_object *json,
                                        int history);
extern void slack_event_handle_reaction(struct t_weeslack_workspace *workspace,
                                         struct json_object *json);
/* Enqueue history load (serialised to avoid Slack rate limits).
 * force=1 clears history_state and re-fetches (e.g. /cslack loadhistory). */
/* Suppress auto history/members for a few seconds after mass buffer create */
extern void slack_event_bootstrap_quiet(int seconds);
extern int slack_event_in_bootstrap_quiet(void);

extern void slack_event_fetch_history(struct t_weeslack_workspace *workspace,
                                       struct t_slack_channel *channel);
extern void slack_event_fetch_history_force(struct t_weeslack_workspace *workspace,
                                             struct t_slack_channel *channel);
extern void slack_event_fetch_replies(struct t_weeslack_workspace *workspace,
                                       struct t_slack_channel *thread);
extern void slack_event_fetch_users(struct t_weeslack_workspace *workspace);
extern void slack_event_fetch_bots(struct t_weeslack_workspace *workspace);
extern void slack_event_fetch_emoji(struct t_weeslack_workspace *workspace);
extern void slack_event_fetch_usergroups(struct t_weeslack_workspace *workspace);
extern void slack_event_fetch_channels(struct t_weeslack_workspace *workspace);
extern void slack_event_download_file(struct t_weeslack_workspace *workspace,
                                       const char *url,
                                       struct t_gui_buffer *buffer);
extern void slack_event_stars_list(struct t_weeslack_workspace *workspace,
                                    struct t_gui_buffer *buffer);
extern void slack_event_star_message(struct t_weeslack_workspace *workspace,
                                      const char *channel_id,
                                      const char *timestamp,
                                      int add);
extern void slack_event_react(struct t_weeslack_workspace *workspace,
                               const char *channel_id, const char *timestamp,
                               const char *emoji, int add);
extern void slack_event_fetch_members(struct t_weeslack_workspace *workspace,
                                       struct t_slack_channel *channel);
extern void slack_event_mark_read(struct t_weeslack_workspace *workspace,
                                   struct t_slack_channel *channel);
extern void slack_event_set_subscribe(struct t_weeslack_workspace *workspace,
                                       struct t_slack_channel *channel,
                                       int subscribe);
extern void slack_event_send_typing(struct t_weeslack_workspace *workspace,
                                     const char *channel_id);
extern void slack_event_send_message(struct t_weeslack_workspace *workspace,
                                      const char *channel_id,
                                      const char *text,
                                      const char *thread_ts);
extern int slack_api_check_error(struct t_weeslack_workspace *workspace,
                                  struct json_object *json,
                                  const char *context);
extern char *slack_event_replace_emoji(const char *text);
extern void slack_event_upload_file(struct t_weeslack_workspace *workspace,
                                     const char *channel_id,
                                     const char *file_path,
                                     const char *thread_ts);
extern char *slack_event_format_mentions(struct t_weeslack_workspace *workspace,
                                          const char *text,
                                          struct t_slack_channel *channel);
extern void slack_event_set_topic(struct t_weeslack_workspace *workspace,
                                   const char *channel_id,
                                   const char *topic);
extern void slack_event_open_dm(struct t_weeslack_workspace *workspace,
                                 const char *user_id);
extern void slack_event_set_dnd(struct t_weeslack_workspace *workspace,
                                 int enable);
extern void slack_event_set_presence(struct t_weeslack_workspace *workspace,
                                      const char *presence);
extern void slack_event_set_mute(struct t_weeslack_workspace *workspace,
                                  const char *channel_id, int mute);
extern void slack_event_get_permalink(struct t_weeslack_workspace *workspace,
                                       const char *channel_id,
                                       const char *timestamp,
                                       struct t_gui_buffer *buffer);
extern void slack_event_pin_message(struct t_weeslack_workspace *workspace,
                                     const char *channel_id,
                                     const char *timestamp,
                                     int pin);
extern void slack_event_search_messages(struct t_weeslack_workspace *workspace,
                                         const char *query,
                                         struct t_gui_buffer *buffer);
extern void slack_event_join_channel(struct t_weeslack_workspace *workspace,
                                     const char *name_or_id);
extern void slack_event_leave_channel(struct t_weeslack_workspace *workspace,
                                      const char *channel_id);
extern void slack_event_whois(struct t_weeslack_workspace *workspace,
                              const char *name_or_id,
                              struct t_gui_buffer *buffer);
/* Re-issue rtm.connect for a fresh WebSocket URL (after drop / goodbye). */
extern void slack_event_rtm_reconnect(struct t_weeslack_workspace *workspace);
/* Refresh users.list + emoji.list without full connect bootstrap. */
extern void slack_event_refresh_directory(struct t_weeslack_workspace *workspace);
/* Force re-fetch of channel members into nicklist (clears members_loaded). */
extern void slack_event_refresh_members(struct t_weeslack_workspace *workspace,
                                        struct t_slack_channel *channel);

#endif
