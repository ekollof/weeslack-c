# WeeChat automation tools (vendored)

Agent-safe control of a **running** WeeChat process without FIFO (which posts
mistyped lines as chat).

| File | Role |
|------|------|
| `weechat_debug_socket.py` | WeeChat Python plugin: Unix socket eval / `/command` |
| `weechat-cmd.sh` | CLI client (`socat`) for that socket |

**Upstream / sister tree:** often maintained alongside
[weechat-export](https://github.com/ekollof/my-weechat) (`weechat-cmd.sh`,
`weechat-python/weechat_debug_socket.py`). When fixing bugs, prefer keeping both
copies in sync.

## Install (once per WeeChat home)

```sh
# From this repo root (or any checkout of weeslack-c)
mkdir -p "${XDG_DATA_HOME:-$HOME/.local/share}/weechat/python/autoload"
cp tools/weechat_debug_socket.py \
  "${XDG_DATA_HOME:-$HOME/.local/share}/weechat/python/autoload/"

# Load without full WeeChat restart (if Python already running):
#   /python load weechat_debug_socket.py
# Or restart WeeChat so autoload picks it up.
```

Requires: **socat**, WeeChat with Python, script loaded.

## Use

```sh
# From anywhere (script resolves socket under XDG_RUNTIME_DIR)
./tools/weechat-cmd.sh '${info:version}'
./tools/weechat-cmd.sh '/plugin list'
./tools/weechat-cmd.sh '/cslack list'

# After make install of weeslack.so
./tools/weechat-cmd.sh '/plugin load weeslack'
./tools/weechat-cmd.sh '/plugin unload weeslack'
```

Socket path (default):

```text
${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/weechat/weechat_debug.sock
```

## Safety rules (mandatory for agents)

1. **Use this socket + `weechat-cmd.sh`**, never the WeeChat FIFO pipe for
   automation. FIFO without a leading `*` posts text to the focused buffer
   (public chat / MUC risk).
2. **Never send `/reload`** while `xmpp` (or other fragile native plugins) is
   loaded — `weechat-cmd.sh` rejects bare `/reload`. Prefer `/set`, `/fset`,
   `/plugin load|unload|reload weeslack`, or restart WeeChat.
3. Prefer **`/plugin unload weeslack`** / **`/plugin load weeslack`** over
   `/python reload` when testing **this** C plugin.
4. Prefer **`/python unload`** + **`/python load`** over `/python reload` on
   systems with Python 3.14+ (reload can SIGSEGV).
5. Socket **rejects bare text** that is not `${...}` or `/command` — that is
   intentional so agents cannot accidentally chat.

## Protocol (one-shot)

Client writes one line, then shuts down write side; server replies with one
line and closes. Expression forms: `${...}` eval, or `/command`.

See script headers in `weechat_debug_socket.py` for details.
