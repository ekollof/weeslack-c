#ifndef SLACK_BUFFER_H
#define SLACK_BUFFER_H

#include <weechat/weechat-plugin.h>
#include "slack_data.h"

struct t_weeslack_workspace;

struct t_slack_buffer
{
    struct t_slack_channel *channel;
    struct t_weeslack_workspace *workspace;
    struct t_gui_buffer *buffer;
    int is_thread;
    struct t_slack_buffer *next;
    struct t_slack_buffer *prev;
};

extern struct t_slack_buffer *slack_buffer_new_server(struct t_weeslack_workspace *workspace);
extern struct t_slack_buffer *slack_buffer_new(struct t_weeslack_workspace *workspace,
                                                struct t_slack_channel *channel);
extern struct t_slack_buffer *slack_buffer_search(struct t_gui_buffer *buffer);
extern struct t_slack_buffer *slack_buffer_search_by_channel(const char *channel_id);
extern void slack_buffer_free(struct t_slack_buffer *sbuf);
extern void slack_buffer_set_topic(struct t_slack_buffer *sbuf, const char *topic);
extern void slack_buffer_set_title(struct t_slack_buffer *sbuf, const char *title);
extern void slack_buffer_print_message(struct t_slack_buffer *sbuf,
                                        struct t_slack_message *msg,
                                        const char *nick,
                                        const char *message);
extern void slack_buffer_add_nick(struct t_slack_buffer *sbuf,
                                   struct t_slack_user *user);
extern void slack_buffer_remove_nick(struct t_slack_buffer *sbuf,
                                      struct t_slack_user *user);
extern void slack_buffer_refresh_nicks(struct t_slack_buffer *sbuf);
extern void slack_buffer_clear_nicks(struct t_slack_buffer *sbuf);
/* Drop bots/apps/slackbot from every channel nicklist (after users.list). */
extern void slack_buffer_purge_hidden_nicks(void);
extern void slack_buffer_set_muted(struct t_slack_buffer *sbuf, int muted);
extern void slack_buffer_clear_hotlist(struct t_gui_buffer *buffer);
extern void slack_buffer_set_typing(struct t_slack_channel *channel,
                                     const char *user_name);
extern void slack_buffer_update_user_presence(struct t_slack_user *user);

#endif
