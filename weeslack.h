#ifndef WEESLACK_H
#define WEESLACK_H

#include <weechat/weechat-plugin.h>

#define SLACK_WS_PRINTF(_ws, _fmt, ...) \
    weechat_printf((_ws && (_ws)->server_buffer) ? (_ws)->server_buffer : NULL, \
                    _fmt, ##__VA_ARGS__)

struct t_slack_ws;

struct t_weeslack_workspace
{
    char *id;   /* stable key (e.g. "default") — never overwritten by team name */
    char *name; /* display name from Slack team */
    char *token;
    char *cookie;
    char *ws_url;
    char *my_user_id;
    /* "active" / "away" from presence events; manual away from /cslack away */
    char *my_presence;
    int my_manual_away;
    int connected;
    int reconnect_delay;
    int max_reconnect_delay;
    struct t_slack_ws *ws;
    struct t_gui_buffer *server_buffer;
    struct t_weeslack_workspace *next_workspace;
    struct t_weeslack_workspace *prev_workspace;
};

struct t_weeslack_channel
{
    char *id;
    char *name;
    struct t_weeslack_workspace *workspace;
    struct t_weeslack_channel *next_channel;
    struct t_weeslack_channel *prev_channel;
};

struct t_weeslack_config
{
    struct t_config_file *file;
    struct t_config_section *section_workspace;
    struct t_config_section *section_look;
    struct t_config_section *section_color;

    /* workspace */
    struct t_config_option *token;

    /* look */
    struct t_config_option *render_bold_as;
    struct t_config_option *render_italic_as;
    struct t_config_option *render_strikethrough_as;
    struct t_config_option *thread_messages_in_channel;
    struct t_config_option *thread_broadcast_prefix;
    struct t_config_option *short_buffer_names;
    struct t_config_option *channel_name_typing_indicator;
    struct t_config_option *emoji_render_mode;
    struct t_config_option *download_path;
    struct t_config_option *never_away;
    struct t_config_option *send_typing_notice;
    struct t_config_option *use_full_names;
    struct t_config_option *external_user_suffix;
    struct t_config_option *show_reaction_nicks;
    struct t_config_option *distracting_channels;

    /* color */
    struct t_config_option *color_typing_notice;
    struct t_config_option *color_deleted;
    struct t_config_option *color_edited_suffix;
    struct t_config_option *color_thread_suffix;
    struct t_config_option *color_reaction_suffix;
    struct t_config_option *color_reaction_suffix_added_by_you;
    struct t_config_option *colorize_private_chats;
    struct t_config_option *color_buflist_muted_channels;
};

extern struct t_weeslack_config weeslack_config;

extern struct t_weechat_plugin *weechat_plugin;

extern struct t_weeslack_workspace *weeslack_workspace_search(const char *name);
extern struct t_weeslack_workspace *weeslack_workspace_new(const char *name, const char *token, const char *cookie);
extern int weeslack_workspace_free(struct t_weeslack_workspace *workspace);

extern struct t_weeslack_channel *weeslack_channel_search(struct t_weeslack_workspace *workspace, const char *name);
extern struct t_weeslack_channel *weeslack_channel_new(struct t_weeslack_workspace *workspace, const char *id, const char *name);
extern int weeslack_channel_free(struct t_weeslack_channel *channel);

#endif
