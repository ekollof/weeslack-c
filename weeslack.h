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
    char *name; /* display name from Slack team (may be server_aliases) */
    char *domain; /* team subdomain from rtm.connect (e.g. "acme") */
    char *token;
    char *cookie;
    char *ws_url;
    char *my_user_id;
    /* Comma-separated keywords from users.prefs highlight_words */
    char *highlight_words;
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
    struct t_config_option *auto_connect;
    struct t_config_option *server_aliases;

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
    struct t_config_option *auto_download_files;
    struct t_config_option *icat_enabled;
    struct t_config_option *never_away;
    struct t_config_option *send_typing_notice;
    struct t_config_option *use_full_names;
    struct t_config_option *external_user_suffix;
    struct t_config_option *show_reaction_nicks;
    struct t_config_option *distracting_channels;
    struct t_config_option *history_fetch_count;
    struct t_config_option *group_name_prefix;
    struct t_config_option *switch_buffer_on_join;
    struct t_config_option *unhide_buffers_with_activity;
    struct t_config_option *muted_channels_activity;
    struct t_config_option *map_underline_to;
    struct t_config_option *link_previews;
    struct t_config_option *notify_subscribed_threads;
    struct t_config_option *colorize_attachments;
    struct t_config_option *shared_name_prefix;
    /* 0=off 1=subscribed/@mention 2=all live replies (rate-limit sensitive) */
    struct t_config_option *auto_open_threads;
    struct t_config_option *show_buflist_presence;
    struct t_config_option *notify_usergroup_handle_updated;
    struct t_config_option *unfurl_ignore_alt_text;
    struct t_config_option *unfurl_auto_link_display;
    struct t_config_option *debug_mode;
    struct t_config_option *record_events;
    struct t_config_option *background_load_all_history;
    struct t_config_option *background_history_max;
    struct t_config_option *history_max_pages;
    struct t_config_option *members_max_pages;
    struct t_config_option *slack_timeout;
    struct t_config_option *debug_level;
    struct t_config_option *leave_channel_on_buffer_close;

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

/* Set during plugin_end so buffer close does not leave Slack channels. */
extern int weeslack_plugin_unloading;

extern struct t_weeslack_workspace *weeslack_workspace_search(const char *name);
extern struct t_weeslack_workspace *weeslack_workspace_new(const char *name, const char *token, const char *cookie);
extern int weeslack_workspace_free(struct t_weeslack_workspace *workspace);
/* Resolve workspace from buffer localvars, sole workspace, or "default". */
extern struct t_weeslack_workspace *weeslack_workspace_from_buffer(
    struct t_gui_buffer *buffer);

/* Debug buffer (look.debug_mode) and RTM JSON log (look.record_events).
 * level: 1=important … 5=noisy; printed only if level <= look.debug_level. */
extern void weeslack_debug(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
extern void weeslack_debug_at(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
extern void weeslack_debug_open_buffer(void);
extern void weeslack_record_json(const char *json_text);
/* Prefer workspace domain for log path when available. */
extern void weeslack_record_json_ws(struct t_weeslack_workspace *workspace,
                                    const char *json_text);

extern struct t_weeslack_channel *weeslack_channel_search(struct t_weeslack_workspace *workspace, const char *name);
extern struct t_weeslack_channel *weeslack_channel_new(struct t_weeslack_workspace *workspace, const char *id, const char *name);
extern int weeslack_channel_free(struct t_weeslack_channel *channel);

#endif
