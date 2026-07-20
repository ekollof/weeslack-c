#!/usr/bin/env bash
# weechat-cmd — send an eval expression or /command to the weechat debug socket
#
# Vendored in weeslack-c/tools/ for agents working on this plugin. Sister copy:
# weechat-export/weechat-cmd.sh — keep behaviour in sync when fixing bugs.
#
# Usage:
#   weechat-cmd '${weechat.color.chat_bg}'        # eval option, prints result
#   weechat-cmd '${info:version}'                 # eval info
#   weechat-cmd '/set weechat.color.chat_bg red'  # execute command (prints "ok")
#   echo '${buflist.format.name}' | weechat-cmd   # read from stdin
#
# Note: eval expressions must use ${...} syntax. Bare text is rejected by the
# socket (never sent to chat — unlike the FIFO pipe, which posts it to the MUC).
#
# Requires: socat, weechat_debug_socket.py loaded (autoloaded after install.sh)
#
# Do NOT use the FIFO pipe for automation. Lines without a leading * are sent as
# chat text to the focused buffer (IRC channel / XMPP MUC). A mistyped /command
# without * will be posted publicly. Use this script (debug socket) instead.
#
# NEVER use /reload while xmpp is loaded — it crashes xmpp.so (SIGABRT on config
# reload). For config changes: use /set, /trigger set, or /fset via weechat-cmd,
# or restart weechat. For Python scripts: /python reload <name> only.

SOCK_PATH="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/weechat/weechat_debug.sock"

reject_unsafe_cmd() {
    local cmd="$1"
    if [[ "$cmd" =~ ^[[:space:]]*/reload([[:space:]]|$) ]]; then
        echo "error: /reload is unsafe with xmpp loaded (crashes xmpp.so)" >&2
        echo "  use /set or /fset for config, /python reload <script> for scripts" >&2
        echo "  or restart weechat for a full config refresh" >&2
        exit 1
    fi
}

if [[ ! -S "$SOCK_PATH" ]]; then
    echo "error: socket not found at $SOCK_PATH" >&2
    echo "  Is weechat running with weechat_debug_socket.py loaded?" >&2
    exit 1
fi

if [[ $# -gt 0 ]]; then
    reject_unsafe_cmd "$*"
    printf '%s\n' "$*" | socat - "UNIX-CONNECT:${SOCK_PATH}"
else
    # read from stdin (allows piping)
    if [[ -t 0 ]]; then
        socat - "UNIX-CONNECT:${SOCK_PATH}"
    else
        cmd="$(cat)"
        reject_unsafe_cmd "$cmd"
        printf '%s' "$cmd" | socat - "UNIX-CONNECT:${SOCK_PATH}"
    fi
fi
