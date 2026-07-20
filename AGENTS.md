# weeslack-c

Native C WeeChat plugin for Slack (`weeslack.so`, command `/cslack`).
Migration path from Python [wee-slack](https://github.com/wee-slack/wee-slack).

**Do not hammer the Slack API.** History and bulk work go through a paced
request queue (see below). Prefer leaving the plugin unloaded while iterating
on rate-limit sensitive code.

## Build

```sh
make            # compile
make install    # → ~/.local/share/weechat/plugins/weeslack.so
make clean
```

Dependencies: WeeChat headers, json-c, OpenSSL, libcurl.

## Architecture

```
weeslack.c       Plugin entry, config, /cslack, completions, upgrade, workspace
slack_http.c     Web API queue + libcurl multi (form + binary) + rate-limit
slack_ws.c       RTM WebSocket (hook_connect + OpenSSL TLS + RFC6455 masking)
slack_event.c    Events, history, members, upload, stars, download, API helpers
slack_buffer.c   Buffer create/layout, nicklist, typing title, localvars
slack_data.c     Users, channels, messages, bots, subteams, timestamps
```

### HTTP request model (wee-slack-style)

All `slack_http_request_new*` calls **enqueue**; nothing stampedes the API.

| Mechanism | Behavior |
|-----------|----------|
| Fast queue | Normal Web API calls |
| Slow queue (`SLACK_HTTP_SLOW`) | History + members; ≤1 promote/sec into fast |
| Max concurrent | **4** libcurl multi handles total (API + binary + OAuth body) |
| 429 / `ratelimited` | Global cooldown from `Retry-After` (else ~8s); re-queue job |
| Soft failure | Quadratic backoff re-queue (max 3 tries) |
| `SLACK_HTTP_MARK` | `conversations.mark` — dropped under cooldown |

Call `slack_http_queue_init()` in plugin init and `slack_http_queue_shutdown()` on end.

All Slack HTTP uses **libcurl multi** (timer-driven): Web API form bodies,
binary **PUT**/authenticated **GET**, OAuth code exchange, custom-emoji fetch.
Bearer + cookie + WeeChat proxy where applicable. No `hook_url` / `hook_process`.

### Connect bootstrap (rate-limit safe)

```
rtm.connect
  → users.list → bots.list (optional) → emoji.list → usergroups.list
  → conversations.list (paginated) → create buffers
       NO history, NO members at create
  → bootstrap quiet ~8s (buffer_switch ignores history/members)
  → user focuses buffer → history (slow) + members once
  → /cslack loadhistory [channel_id] forces re-fetch
```

**Never** load history for every buffer at connect. Creating many buffers fires
`buffer_switch` for each; quiet period + lazy load prevent storms.

### Workspace identity

- `workspace->id` — stable key (e.g. `"default"`), never overwritten  
- `workspace->name` — Slack team display name after `rtm.connect`  

`weeslack_workspace_search("default")` matches **id** or name, or the sole workspace.

### Buffer layout

| Buffer | full_name pattern |
|--------|-------------------|
| Server | `weeslack.server.<team>` |
| Channel / DM | `weeslack.<team>.<name>` |

Localvars: `type`, `server`, `channel`, `slack_channel_id`, `slack_type`,
`slack_dm_user` (DMs), mute/subscribe flags. Do **not** set `script_name=slack`
(collides with Python wee-slack).

History tags include `slack_ts_<ts>` for tooling.

### Key commands

| Command | Notes |
|---------|--------|
| `/cslack connect` / `reconnect [all]` / `disconnect` | Connect / re-issue rtm.connect / drop RTM |
| `/cslack migrate` / `register` | Python token import; OAuth or paste `xox…` |
| `/cslack unregister` / `forget -yes [id]` | Retire workspace: drop token + close buffers |
| `/cslack loadhistory` / `rehistory` / `names` | History + nicklist (focused buffer if core) |
| `/cslack talk` / `open` | DM or MPDM (`a,b`); open = talk alias |
| `/cslack join` / `show [#ch]` | Join API, or reopen closed member buffer |
| `/cslack download` / `upload` | Auth file I/O via libcurl multi |
| `/cslack stars` / `star` / `unstar` / `react` / `unreact` | Stars + reactions |
| `/cslack info` / `queue` / `debug` / `version` | Status, HTTP queue, debug buffer |

When `/cslack` is run from `core.weechat` (debug socket), buffer-local ops use
`weechat_current_buffer()`.

### Config highlights (`weeslack.conf`)

- `workspace.token` — xoxp/xoxb or `xoxc-token:cookie`; **comma-separated multi-team**  
- `workspace.auto_connect` — connect on plugin load if token valid (default on; respects WeeChat auto_connect/`-a`)  
- `workspace.server_aliases` — `subdomain:alias` pairs for team display name  
- Workspace ids: first token → `default`, then `ws1`, `ws2`… (staggered connect)  
- Buffers set `localvar_slack_workspace_id`; commands resolve via `weeslack_workspace_from_buffer`  
- `look.debug_mode` / `look.record_events` — debug buffer + RTM JSONL log  
- `look.background_load_all_history` — opt-in post-quiet history queue (default off)  
- `look.slack_timeout` — HTTP/libcurl timeout ms (default 30000)  
- `look.auto_open_threads` — `off` / `subscribed` / `all` (default off)  
- `look.leave_channel_on_buffer_close` — remote leave when closing buffer (default off)  
- `look.show_buflist_presence` — DM short_name `+nick` when peer active  
- `look.unfurl_auto_link_display` (`both`/`text`/`url`), `unfurl_ignore_alt_text`  
- `look.notify_usergroup_handle_updated`  
- `look.emoji_render_mode`, typing indicator, thread_messages_in_channel  
- IRC cmds on weeslack buffers: `/me` `/join` `/query` `/msg` `/part` `/topic` `/invite` `/away` `/whois`  
- Input `/cmd args` → Slack slash (`chat.command`); `//text` posts literally  
- `/cslack talk a,b` / `/query a,b` → MPDM  
- `/cslack info` — version, queue, options, workspaces  

- `look.download_path` — root; files go to `<root>/weeslack/<origin>/<YYYY-MM-DD>/<file>`  
- `look.auto_download_files` — live attachments auto-saved under that layout (default on)  
- `look.icat_enabled` — Kitty `/icat` for attachment images **and** custom
  emoji (requires `icat.py` / `/icat` registered; probe before run)
- `render_bold_as` / `render_italic_as`  
- Color options for typing / deleted / edited / thread / muted  

## Live testing / automation

With WeeChat running and `weechat_debug_socket.py` loaded (see **tools/** below):

```sh
# from this repo root
./tools/weechat-cmd.sh '/plugin load weeslack'
./tools/weechat-cmd.sh '/cslack connect'
./tools/weechat-cmd.sh '/cslack loadhistory'   # after focusing a channel in the UI
```

Socket: `$XDG_RUNTIME_DIR/weechat/weechat_debug.sock`.

**Do not** use the WeeChat FIFO for automation (chat leakage).  
**Never** `/reload` with `xmpp.so` loaded (crashes). Use `/set` or process restart.  
**Avoid** `/python reload` on Python 3.14 if possible (unload fatals).  
Prefer process restart for full config refresh.

## Safe Programming Standards

These rules are **mandatory**.

### Buffer Safety

- **Never** use `strcpy`, `strcat`, `sprintf`, or `strncat`. Use `snprintf`
  with `sizeof(buf)` / remaining capacity (`buf + pos`, `sizeof - pos`).
- Track remaining capacity when building strings incrementally.
- Prefer `calloc` over `malloc`.

### Input Validation

- Validate all external input (JSON, API responses, user commands).
- Check return values of allocations and WeeChat API calls.
- Never trust network length fields without bounds checks.

### Memory Management

- Every `malloc`/`calloc`/`strdup` must have a matching `free` on all paths.
- `json_object_put` for every json-c object you own.
- Free queue/request state via `slack_http_request_free` / cancel on disconnect.

### Network / Protocol Safety

- Encode user content for API form fields (urlencode path in `slack_http`).
- Validate WebSocket opcodes and lengths; handle partial reads.
- **Rate-limit everything** through `slack_http` queue; respect `Retry-After`.
- Treat all network responses as untrusted.
- Do not load bulk history in a loop or on every buffer create.

### Error Handling

- Clean up on failure; log with `weechat_prefix("error")`.
- Prefer server buffer for workspace-scoped messages (`SLACK_WS_PRINTF`).

### Concurrency / Reentrancy

- WeeChat callbacks are main-thread; do not block.
- HTTP is async via **libcurl multi** + timer (API form bodies and binary
  upload/download share the multi).
- Queue pump is timer-driven (`slack_http_queue_init`).

### Code Style

- C11: `-std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE`
- `-Wall -Wextra -Werror -pedantic`
- `static` for file-local symbols; `(void)param` for unused.
- One module per `.c`/`.h` pair; header guards.
- Prefer **C11-friendly constants**: `enum { … = N }` for integer constant
  expressions (array sizes, caps), not sprawling magic numbers.
- Use **`static_assert` / `_Static_assert`** for invariants between those
  constants (include `<assert.h>`). Do not require C23 `constexpr`.

### Git commits

Commit **regularly** between logical batches (not one giant dump at the end of
a session). Prefer small, reviewable commits.

**Message style:**

- **Subject (first line):** short and terse — imperative, no trailing period,
  roughly ≤72 chars. Focus on *what* changed, not a full essay.
- **Body (after a blank line):** explain *why* / *how* when it is not obvious:
  tradeoffs, rate-limit rationale, user-visible behavior, follow-ups.
- The project already discloses AI assistance (see README). Do not pad every
  commit with tool/agent footers or “Co-authored-by” noise unless the user
  asks for that on a given change.

Examples:

```text
Good subject:  Reopen closed channels on live message
Bad subject:   This commit updates the buffer handling so that when a user
               closes a channel we can still recreate it later when needed

Good body:     Keep channel model after free; reset history_state so focus
               re-fetches. History loads must not recreate buffers.
```

## Live testing (vendored weechat-cmd)

This tree vendors **agent automation** under `tools/`:

| Path | Purpose |
|------|---------|
| `tools/weechat_debug_socket.py` | WeeChat Python plugin (Unix socket) |
| `tools/weechat-cmd.sh` | CLI client (`socat`) |
| `tools/README.md` | Install + safety rules |

**Do not use the WeeChat FIFO** for automation (public chat risk). Use:

```sh
./tools/weechat-cmd.sh '${info:version}'
./tools/weechat-cmd.sh '/plugin list'
./tools/weechat-cmd.sh '/cslack list'
./tools/weechat-cmd.sh '/plugin unload weeslack'   # after make install
./tools/weechat-cmd.sh '/plugin load weeslack'
```

Install the socket plugin once (autoload):

```sh
mkdir -p "${XDG_DATA_HOME:-$HOME/.local/share}/weechat/python/autoload"
cp tools/weechat_debug_socket.py \
  "${XDG_DATA_HOME:-$HOME/.local/share}/weechat/python/autoload/"
# then restart WeeChat, or /python load weechat_debug_socket.py
```

**Never** send `/reload` via automation while fragile native plugins (e.g.
xmpp) are loaded — `weechat-cmd.sh` blocks bare `/reload`. For this C plugin
prefer `/plugin unload|load weeslack` after `make install`.

**Rate limits:** prefer `weeslack` unloaded while editing queue/history code;
do not `/cslack connect` in a loop.

## Related trees

- **weechat-export** — full XDG config export/install; often the source of
  newer `weechat-cmd` / debug-socket fixes (keep `tools/` in sync)  
- Live WeeChat: XDG (`~/.config/weechat`, `~/.local/share/weechat`)  

Buflist parent nesting for `weeslack` requires trigger `search_server_buffer_ptr`
to include plugin `weeslack` (see weechat-export `weechat-conf/trigger.conf`).
