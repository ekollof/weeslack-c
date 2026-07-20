# weeslack-c

A native C plugin for [WeeChat](https://weechat.org/) providing Slack
protocol support. It is a migration path from the Python
[wee-slack](https://github.com/wee-slack/wee-slack) script (`/cslack`
instead of `/slack`).

**Status:** Feature parity with Python wee-slack is largely complete. Core
connectivity, paced HTTP, RTM, multi-workspace isolation, libcurl binary
upload/download, threads, stars, OAuth register/unregister, and Kitty/icat
previews (files + custom emoji) are in place. See [TODO.md](TODO.md).

Plugin binary: `weeslack.so` · Command: `/cslack` · Config: `weeslack.conf`

> **Note:** This project was developed with AI assistance.

## Features

### Connectivity

- Slack RTM WebSocket (TLS + SNI, RFC6455 masking, ping/pong, reconnect)
- Fresh `rtm.connect` URL after drops / `goodbye` (no stale WS reuse)
- Web API via WeeChat `hook_url` with a **paced request queue** (wee-slack model)
- Rate-limit handling: max 2 concurrent, slow queue for history/members,
  global cooldown on 429 / `Retry-After`
- Proxy via WeeChat globals for HTTP, WebSocket, and libcurl upload/download
- Binary upload + authenticated download via **libcurl multi** (async)
- Auto-connect on load when a valid token is set (respects WeeChat `-a`)
- Multi-team: comma-separated tokens; staggered connect; workspace-scoped
  users/bots/emoji; ids `default`, `ws1`, …

### Chat experience

- Server + channel / DM / MPDM / thread buffers (isolated from Python wee-slack)
- Lazy history + members on focus (optional background history after quiet period)
- Reactions, edits/deletes (in-place line rewrite), `$hash` message ids
- Typing via WeeChat typing plugin (`typing_set_nick`) + optional title/bar
- Mentions, mute prefs, presence (Here/Away nicklist), stars, pins, search
- Threads: open on demand, subscribe API; `auto_open_threads` =
  `off` / `subscribed` / `all`
- Input: `s/old/new/`, `+emoji` / `-emoji`, `/me`, Slack slash `/cmd`, `//` escape
- IRC-style commands on weeslack buffers: `/me` `/join` `/query` `/msg`
  `/part` `/topic` `/invite` `/away` `/whois`
- Auth file download (Xepher-style path) + optional auto-download + Kitty
  `/icat` previews when `icat.py` is loaded

### Team lifecycle (beyond wee-slack)

- `/cslack register` — OAuth (same public wee-slack app) or paste `xox…`
- `/cslack unregister` / `forget` — retire a team: disconnect, close buffers,
  **remove token from config** (for leaving a company)

## Building

### Dependencies

- WeeChat development headers (`weechat-dev` / `weechat-devel`)
- json-c, OpenSSL, pkg-config
- C11 compiler (GCC/Clang)

### Compile / install

```sh
make              # → weeslack.so
make install      # → ~/.local/share/weechat/plugins/weeslack.so
make uninstall
make clean
make asan         # AddressSanitizer build (see Makefile)
```

After `make install`, reload in WeeChat:

```
/plugin unload weeslack
/plugin load weeslack
```

Do not overwrite a live `.so` under a running process without unloading first.

## Usage

### Migrating from wee-slack

```
/cslack migrate
```

Copies `plugins.var.python.slack.slack_api_token` into
`weeslack.workspace.token` (supports comma-separated multi-team tokens).

### Tokens

```
/set weeslack.workspace.token xoxp-your-token
/set weeslack.workspace.token xoxc-token:cookie_value
# multi-team (first = default, then ws1…; extra teams connect +3s apart):
/set weeslack.workspace.token xoxp-team1,xoxc-team2:d=cookie
```

Or OAuth / paste:

```
/cslack register              # print OAuth instructions
/cslack register <code>       # exchange code for token
/cslack register xoxp-...     # paste existing token
```

### Connect / disconnect

With a valid token and `weeslack.workspace.auto_connect` on (default), the
plugin connects shortly after load (honors WeeChat auto-connect / `-a`).

```
/cslack connect
/cslack disconnect            # all teams, or current team buffer only
/set weeslack.workspace.auto_connect off
```

### Retire a team (remove token)

```
/cslack list
/cslack unregister            # dry-run: show what would be removed
/cslack unregister -yes       # current buffer’s workspace
/cslack forget -yes ws1       # by id / name / domain
```

This disconnects RTM, closes that team’s buffers, and drops its entry from
`weeslack.workspace.token` so it will not auto-connect again.

### Commands

| Command | Description |
|---------|-------------|
| `/cslack connect` | Connect using configured token(s) |
| `/cslack reconnect [all]` | Re-issue `rtm.connect` for current team or all |
| `/cslack disconnect` | Disconnect current team or all |
| `/cslack migrate` | Import token(s) from Python wee-slack |
| `/cslack register` | OAuth add team or paste `xox…` token |
| `/cslack unregister` / `forget` | Retire team: `-yes [id\|name\|domain]` |
| `/cslack list` / `teams` | List workspaces |
| `/cslack channels [regex]` | List channels; `-refresh` re-fetches |
| `/cslack users [regex]` | List users |
| `/cslack usergroups [@handle]` | List groups or group members |
| `/cslack loadhistory` / `rehistory` | Fetch history for current (or given) channel |
| `/cslack names` | Refresh nicklist for current channel |
| `/cslack refresh` | Re-fetch users.list + emoji.list |
| `/cslack talk` / `join` / `leave` | DM(s) (`a,b` → MPDM), join (reopens if member), leave |
| `/cslack show [#ch]` | Unhide current or reopen a closed member channel |
| `/cslack topic` / `create` / `invite` | Topic, create channel, invite user |
| `/cslack mute` / `unmute` / `showmuted` | Mute prefs |
| `/cslack status` | Profile emoji/text, or `dnd`/`away`/`active`/`-delete` |
| `/cslack away` / `back` | Presence |
| `/cslack thread` / `reply` / `subscribe` | Threads |
| `/cslack react` / `unreact` | Reactions (`[ts\|$hash] emoji`) |
| `/cslack star` / `unstar` / `stars` | Stars |
| `/cslack pin` / `unpin` | Pins |
| `/cslack search` / `linkarchive` | Search; permalink for message |
| `/cslack download <url>` | Authenticated private file download |
| `/cslack upload <file>` | Upload to current channel |
| `/cslack slash /cmd [args]` | Slack `chat.command` |
| `/cslack whois` | User info + live presence |
| `/cslack hide` / `show` / `label` | Buffer hide / title |
| `/cslack distracting` / `nodistractions` | Distracting channel list |
| `/cslack typing` | Send typing indicator |
| `/cslack open` | Alias for `talk` (DM/MPDM) |
| `/cslack debug [on\|off]` | Open `weeslack.debug`; toggle `look.debug_mode` |
| `/cslack queue` | HTTP queue / cooldown status |
| `/cslack info` / `version` | Plugin version + queue + workspaces |
| `/cslack help` | Command help |

When `/cslack` is run from `core.weechat` (e.g. debug socket), buffer-local
ops use the focused window buffer.

### Input shortcuts (on channel buffers)

| Input | Effect |
|-------|--------|
| `s/old/new/[gi]` | Edit last (or `$hash`/N) message via `chat.update` |
| `s///` | Delete message via `chat.delete` |
| `+emoji` / `-emoji` | Add/remove reaction on last message |
| `/me text` | `chat.meMessage` |
| `/cmd args` | Unknown WeeChat commands → Slack slash (`chat.command`) |
| `//text` or leading space | Post literal text starting with `/` |

### IRC-style commands (weeslack buffers only)

`/me` `/join` `/query` `/msg` `/part` `/leave` `/topic` `/invite` `/away`
`/whois` are handled for Slack and do not affect IRC buffers.

## Configuration highlights

File: `weeslack.conf` (WeeChat options under `weeslack.*`).

### Workspace

| Option | Notes |
|--------|--------|
| `workspace.token` | `xoxp`/`xoxb`/`xoxs` or `xoxc-token:cookie`; comma-separated multi-team |
| `workspace.auto_connect` | Connect on plugin load if token valid (default on) |
| `workspace.server_aliases` | `subdomain:alias` pairs for display name |

### Look (selected)

| Option | Notes |
|--------|--------|
| `look.download_path` | Root for files; empty → `$XDG_DOWNLOAD_DIR` or `~/Downloads` |
| `look.auto_download_files` | Auto-save live attachments (default on) |
| `look.icat_enabled` | Kitty preview via `/icat` if `icat.py` loaded (default on) |
| `look.send_typing_notice` | Outgoing typing ≤1 / 4s |
| `look.channel_name_typing_indicator` | Also show typing in buffer title |
| `look.auto_open_threads` | `off` / `subscribed` / `all` (default off) |
| `look.leave_channel_on_buffer_close` | Remote leave/close on buffer close (default off) |
| `look.background_load_all_history` | Queue history after quiet (default **off**) |
| `look.background_history_max` | Cap channels for background history (default 40) |
| `look.history_fetch_count` | Messages per history page |
| `look.history_max_pages` | Max history pages per channel load (default 5) |
| `look.members_max_pages` | Max members pages (default 3, hard max 50) |
| `look.slack_timeout` | HTTP/libcurl timeout ms (default 30000) |
| `look.debug_level` | With `debug_mode`: 1–5 (RTM lines at level 3) |
| `look.emoji_render_mode` | emoji / shortcodes / text |
| `look.thread_messages_in_channel` | Show thread replies in parent |
| `look.never_away` | Periodic presence `auto` |
| `look.debug_mode` | `weeslack.debug` buffer |
| `look.record_events` | RTM + HTTP JSONL under data dir |
| `look.unfurl_auto_link_display` | `both` / `text` / `url` |
| `look.show_buflist_presence` | DM short_name `+nick` when active |
| `look.shared_name_prefix` | Prefix for Slack Connect channels (default `%`) |

Colors live under `weeslack.color.*` (typing, deleted, edited, thread,
reactions, muted buflist, private nick colors).

### Downloads and images

Layout (Xepher-style):

```text
<root>/weeslack/<team.channel>/<YYYY-MM-DD>/<filename>
```

Collisions get `.1`, `.2`, … suffixes. Manual `/cslack download` and live
auto-download share this path. If `look.icat_enabled` is on **and** the
weechat-icat script is loaded (`/icat` registered), image files trigger:

```text
/icat -print_immediately -quiet -columns 40 -rows N "<path>"
```

If `/icat` is missing, previews are skipped (one-time warning; no spam).
Custom emoji image URLs are cached under `data_dir/weeslack/emoji/` and
previewed the same way on live messages (when `icat_enabled` is on).

### Typing bar

Enable WeeChat’s typing plugin and put the `typing` item in a bar:

```
/set typing.look.enabled_nicks on
```

Legacy bar item `slack_typing_notice` and optional title indicator remain.

## Buffer layout

| Buffer | full_name pattern |
|--------|-------------------|
| Server | `weeslack.server.<team>` |
| Channel / DM | `weeslack.<team>.<name>` |

Localvars include `slack_channel_id`, `slack_workspace_id`, `slack_type`,
`server`, mute/subscribe flags. **Do not** set `script_name=slack` (collides
with Python wee-slack). History lines are tagged `slack_ts_<ts>` and
`$hash` short-ids for cursor/mouse tools.

## Architecture

```
weeslack.c       Plugin entry, config, /cslack, completions, upgrade, workspaces
slack_http.c     Web API queue + form POST + 429 cooldown
slack_ws.c       RTM WebSocket (hook_connect + OpenSSL + masking)
slack_event.c    Events, history, members, upload, stars, download, slash
slack_buffer.c   Buffers, nicklist, typing signals, input (sed/react)
slack_data.c     Users, channels, messages, bots, subteams, timestamps
```

### Connect bootstrap (rate-limit safe)

```
rtm.connect
  → users.list → emoji.list → usergroups.list
  → conversations.list → create buffers (no history/members yet)
  → users.prefs.get → muted channels
  → ~8s quiet (buffer_switch ignores history storms)
  → focus buffer → history (slow) + members
  → optional background_load_all_history (capped)
```

**Do not** `/cslack connect` in a loop or load history for every buffer at
connect. Prefer the plugin unloaded while hacking queue/history code.

## Live testing / automation

Vendored under [`tools/`](tools/README.md):

```sh
./tools/weechat-cmd.sh '${info:version}'
./tools/weechat-cmd.sh '/plugin unload weeslack'
./tools/weechat-cmd.sh '/plugin load weeslack'
./tools/weechat-cmd.sh '/cslack list'
```

Uses a Unix debug socket (not the WeeChat FIFO — FIFO can leak into chat).
Never send bare `/reload` while fragile native plugins (e.g. xmpp) are loaded.

## Project structure

```
weeslack.c / weeslack.h
slack_http.c / .h
slack_ws.c / .h
slack_event.c / .h
slack_buffer.c / .h
slack_data.c / .h
Makefile
tools/                  weechat-cmd + debug socket plugin
AGENTS.md               Coding standards and agent rules
TODO.md                 Feature parity checklist
```

## Related

- [wee-slack](https://github.com/wee-slack/wee-slack) — original Python plugin
- WeeChat typing plugin — bar item `typing`
- [weechat-icat](https://github.com/trygveaa/weechat-icat) — Kitty graphics (`icat.py`)

## License

BSD 2-Clause. See [LICENSE](LICENSE) for details.
