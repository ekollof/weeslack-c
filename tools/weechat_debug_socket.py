"""
weechat_debug_socket.py — two-way debug interface via Unix socket

Listens on a Unix socket. Each connection sends one expression (or /command),
gets one line back, then closes.

Protocol (every line must use one of these prefixes):
  - /command  — run on core.weechat only (chat-sending commands rejected)
  - ${expr} or !eval ${expr} — read-only eval; result returned, never sent to chat
  - !py code  — Python eval/exec with a restricted weechat.command()
  - Anything else (bare text, mistaken FIFO input) is rejected — never sent to chat

Socket path: ${weechat_runtime_dir}/weechat_debug.sock
  (usually /run/user/1000/weechat/weechat_debug.sock)

Usage:
  echo '${weechat.color.chat_bg}' | socat - UNIX-CONNECT:/run/user/1000/weechat/weechat_debug.sock
  echo '/set weechat.color.chat_bg default' | socat - UNIX-CONNECT:/run/user/1000/weechat/weechat_debug.sock

Or use the weechat-cmd wrapper script.

Do NOT use the FIFO pipe for automation: lines without a leading * are sent as
chat text to the focused buffer (can spam IRC channels / XMPP MUCs).

Vendored in weeslack-c/tools/ for agents on this plugin. Sister copy lives in
weechat-export/weechat-python/ — keep behaviour in sync when fixing bugs.
"""

import re
import weechat
import socket
import os
import errno

SCRIPT_NAME = "weechat_debug_socket"
SCRIPT_AUTHOR = "local"
SCRIPT_VERSION = "1.2.3"
SCRIPT_LICENSE = "MIT"
SCRIPT_DESC = "Two-way debug interface via Unix socket (eval + command execution)"

_server_sock = None
_hook_fd = None
_sock_path = None
_shutting_down = False

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _runtime_dir():
    """Return weechat's runtime dir (same as ${weechat_runtime_dir})."""
    return weechat.info_get("weechat_dir", "").replace(
        weechat.info_get("weechat_config_dir", ""),
        weechat.info_get("weechat_runtime_dir", ""),
    ) or os.path.join(
        os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"),
        "weechat",
    )


def _get_sock_path():
    runtime = weechat.info_get("weechat_runtime_dir", "")
    if not runtime:
        runtime = os.path.join(
            os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"),
            "weechat",
        )
    os.makedirs(runtime, exist_ok=True)
    return os.path.join(runtime, "weechat_debug.sock")


# Eval constructs that execute commands or inject buffer input.
_BLOCKED_EVAL_RE = re.compile(
    r"\$\{(?:command|exec|input|run|script):",
    re.IGNORECASE,
)


def _eval(expr):
    """Evaluate a weechat expression and return the string result."""
    return weechat.string_eval_expression(expr, {}, {}, {})


def _eval_expression(line):
    """Return the expression body from a !eval or ${...} request."""
    stripped = line.lstrip()
    if stripped.startswith("!eval "):
        return stripped[6:].lstrip()
    return stripped


def _eval_blocked(expr):
    """Return an error string if expr must not be evaluated, else None."""
    if _BLOCKED_EVAL_RE.search(expr):
        return "blocked: eval must not run commands (${command:}, ${exec:}, …)"
    return None


def _is_eval_request(line):
    """True if line is an explicit eval request (not mistaken chat/FIFO text)."""
    stripped = line.lstrip()
    return stripped.startswith("${") or stripped.startswith("!eval ")


_CORE_BUFFER_NAME = "core.weechat"

# Commands that can post chat or inject input into a focused buffer.
_BLOCKED_COMMAND_RE = re.compile(
    r"^\s*/(?:"
    r"msg\b|m(?:\s|$)|query\b|privmsg\b|me(?:\s|$)|action\b|say\b|"
    r"input\b|quote\b|part\b|join\b"
    r")",
    re.IGNORECASE,
)


def _buffer_full_name(buffer):
    if not buffer:
        return ""
    return (
        weechat.buffer_get_string(buffer, "full_name")
        or weechat.buffer_get_string(buffer, "name")
        or ""
    )


def _core_buffer():
    """Return the core.weechat buffer (never the focused IRC/XMPP buffer)."""
    for plugin, name in (("core", "weechat"), ("", _CORE_BUFFER_NAME)):
        buf = weechat.buffer_search(plugin, name)
        if buf:
            return buf
    return weechat.buffer_search_main()


def _is_core_buffer(buffer):
    if not buffer:
        return True
    full_name = _buffer_full_name(buffer)
    return full_name == _CORE_BUFFER_NAME or full_name.startswith("core.")


def _command_blocked(cmd):
    """Return an error string if cmd must not run, else None."""
    stripped = cmd.strip()
    if not stripped.startswith("/"):
        return "blocked: not a weechat command"

    if _BLOCKED_COMMAND_RE.match(stripped):
        return "blocked: command can send chat or change channel membership"

    if re.match(r"^\s*/reload\b", stripped, re.IGNORECASE):
        return "blocked: /reload crashes xmpp.so — use /set, /trigger set, or restart"

    if re.match(r"^\s*/eval\b", stripped, re.IGNORECASE):
        return "blocked: /eval can run arbitrary expressions"

    buffer_match = re.search(r"-buffer\s+(\S+)", stripped, re.IGNORECASE)
    if buffer_match:
        target = buffer_match.group(1)
        if not (target == _CORE_BUFFER_NAME or target.startswith("core.")):
            return f"blocked: -buffer {target} (only core.* buffers allowed)"

    return None


def _exec_command(cmd):
    """Execute a weechat command on core.weechat only."""
    reason = _command_blocked(cmd)
    if reason:
        raise PermissionError(reason)

    buf = _core_buffer()
    # /command -buffer core.weechat * <cmd> forces core context even if the
    # nested command would otherwise use the focused MUC/PM buffer.
    weechat.command(buf, f"/command -buffer {_CORE_BUFFER_NAME} * {cmd}")


def _deferred_cmd_cb(data, remaining_calls):
    """
    Run a queued /command outside hook_fd.

    Executing WeeChat commands (especially /python unload|reload) directly
    from hook_fd re-enters the Python plugin and aborts on Python 3.14
    (SIGABRT in weechat_python_exec / PyThreadState_Swap).
    """
    try:
        if data and not _shutting_down:
            _exec_command(data)
    except Exception:
        pass
    return weechat.WEECHAT_RC_OK


def _schedule_command(cmd):
    """Queue cmd for the next main-loop tick; never nest from hook_fd."""
    # 1 ms, once
    weechat.hook_timer(1, 0, 1, "_deferred_cmd_cb", cmd)


class _SafeWeechat:
    """Minimal weechat proxy for !py — command() is core-only."""

    def __init__(self, real):
        self._real = real

    def command(self, buffer, command):
        if not _is_core_buffer(buffer):
            name = self._real.buffer_get_string(buffer, "name") or buffer
            raise PermissionError(f"refusing command on buffer {name}")
        # Defer: same reentrancy rule as top-level /commands from hook_fd.
        reason = _command_blocked(command)
        if reason:
            raise PermissionError(reason)
        _schedule_command(command)
        return weechat.WEECHAT_RC_OK

    def __getattr__(self, name):
        return getattr(self._real, name)


# ---------------------------------------------------------------------------
# Socket handling
# ---------------------------------------------------------------------------


def _accept_cb(data, fd):
    """Called by weechat when the listening socket is readable (new connection)."""
    global _server_sock
    if _shutting_down or _server_sock is None:
        return weechat.WEECHAT_RC_OK

    try:
        conn, _ = _server_sock.accept()
    except OSError:
        return weechat.WEECHAT_RC_OK

    try:
        conn.settimeout(2.0)
        raw = b""
        while not raw.endswith(b"\n"):
            chunk = conn.recv(4096)
            if not chunk:
                break
            raw += chunk

        line = raw.decode("utf-8", errors="replace").rstrip("\n").rstrip("\r")

        if not line:
            conn.sendall(b"(empty input)\n")
        elif line.startswith("/"):
            # Never run /commands inline from hook_fd — nesting Python
            # (e.g. /python unload) SIGABRTs on Python 3.14.
            reason = _command_blocked(line)
            if reason:
                conn.sendall(f"ERROR: {reason}\n".encode("utf-8"))
            else:
                _schedule_command(line)
                conn.sendall(b"ok\n")
        elif line.startswith("!py "):
            # Read-only-ish eval/exec. Still runs inline, but _SafeWeechat
            # defers weechat.command() the same way as top-level /commands.
            code = line[4:]
            try:
                import io
                import sys
                import weechat as _wc  # noqa: F401 — expose to eval'd code

                ns = {"weechat": _SafeWeechat(_wc)}
                buf = io.StringIO()
                old_stdout = sys.stdout
                sys.stdout = buf
                try:
                    try:
                        result = eval(code, ns)  # noqa: S307
                        output = buf.getvalue()
                        if output:
                            response = output.rstrip("\n")
                        elif result is not None:
                            response = str(result)
                        else:
                            response = "None"
                    except SyntaxError:
                        exec(code, ns)  # noqa: S102
                        output = buf.getvalue()
                        response = output.rstrip("\n") if output else "ok"
                finally:
                    sys.stdout = old_stdout
                conn.sendall((response + "\n").encode("utf-8"))
            except Exception as e:
                conn.sendall(f"ERROR: {e}\n".encode("utf-8"))
        elif _is_eval_request(line):
            # Pure eval — no nested weechat.command; safe inline.
            expr = _eval_expression(line)
            reason = _eval_blocked(expr)
            if reason:
                conn.sendall(f"ERROR: {reason}\n".encode("utf-8"))
            else:
                result = _eval(expr)
                conn.sendall((result + "\n").encode("utf-8"))
        else:
            conn.sendall(
                b"ERROR: rejected bare text (never sent to chat); "
                b"use ${...} or !eval, /command, or !py\n"
            )
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass

    return weechat.WEECHAT_RC_OK


# ---------------------------------------------------------------------------
# Startup / shutdown
# ---------------------------------------------------------------------------


def _start_server():
    global _server_sock, _hook_fd, _sock_path, _shutting_down

    _shutting_down = False
    _sock_path = _get_sock_path()

    # Remove stale socket file if it exists
    try:
        os.unlink(_sock_path)
    except FileNotFoundError:
        pass

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setblocking(False)
    try:
        sock.bind(_sock_path)
    except OSError as e:
        weechat.prnt("", f"{SCRIPT_NAME}: failed to bind {_sock_path}: {e}")
        sock.close()
        return False

    sock.listen(8)
    _server_sock = sock

    _hook_fd = weechat.hook_fd(
        sock.fileno(),
        1,  # flag_read
        0,  # flag_write
        0,  # flag_exception
        "_accept_cb",
        "",
    )

    weechat.prnt("", f"{SCRIPT_NAME}: listening on {_sock_path}")
    return True


def _stop_server():
    """Unhook first, then close the socket — reverse order races WeeChat's fd hooks."""
    global _server_sock, _hook_fd, _sock_path, _shutting_down

    _shutting_down = True

    hook = _hook_fd
    _hook_fd = None
    if hook:
        try:
            weechat.unhook(hook)
        except Exception:
            pass

    sock = _server_sock
    _server_sock = None
    if sock is not None:
        try:
            sock.close()
        except OSError:
            pass

    path = _sock_path
    _sock_path = None
    if path:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Script entry / exit
# ---------------------------------------------------------------------------


def weechat_debug_socket_unload_cb():
    try:
        _stop_server()
    except Exception:
        # Never raise during unload (can SIGABRT the process on Python 3.14).
        pass
    return weechat.WEECHAT_RC_OK


if weechat.register(
    SCRIPT_NAME,
    SCRIPT_AUTHOR,
    SCRIPT_VERSION,
    SCRIPT_LICENSE,
    SCRIPT_DESC,
    "weechat_debug_socket_unload_cb",
    "UTF-8",
):
    _start_server()
