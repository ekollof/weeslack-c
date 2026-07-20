# TODO: wee-slack Feature Parity

**Last update:** 2026-07-21 — members_max_pages 0=unlimited; Web API via
libcurl multi (no hook_url)

**Markers:** `[x]` done · `[~]` partial · `[ ]` missing

---

## Phase 1: Core Connectivity

- [x] WebSocket RTM — TLS+SNI, masking, ping/pong, reconnect backoff  
- [x] Reconnect re-issues **`rtm.connect`** (fresh URL; stale WS URLs expire)  
- [x] RTM `goodbye` / `error` → reconnect path  
- [x] HTTP — **libcurl multi** for Web API form POST/GET (Bearer + cookie,
      IPv4, proxy); no `hook_url`  
- [x] **Request queue (wee-slack model)**  
  - Fast queue + **slow queue** (history/members; ≤1 promote/sec)  
  - Max **2** concurrent in-flight  
  - On 429: **global cooldown** from `Retry-After`, re-queue job (no stampede)  
  - Soft failures: quadratic backoff re-queue  
  - `conversations.mark` is **droppable** under cooldown  
- [x] Proxy via WeeChat globals — WS + all libcurl multi transfers  
- [x] Binary **PUT** (upload) + authenticated **GET** (download) via same multi

---

## Phase 2: Data Model

- [x] SlackTS / SlackUser / SlackChannel / SlackSubteam  
- [x] Workspace stable `id` vs display `name`  
- [x] Custom emoji map via **`emoji.list`** (+ **refresh-only** on `emoji_changed`)  
- [x] **Multi-workspace model isolation** — users / bots / subteams / custom emoji
      carry `workspace`; free on `unregister`; name/search prefers current team  
- [x] Bots — `is_bot` / `is_app_user` registered in bot map; **hidden from nicklist**  
- [x] SlackMessage — list model + **in-place line rewrite** via `hdata_update`  
- [x] Reaction list maintained; **rewrites line** on RTM add/remove  
- [x] Message **`$hash` short-ids** (SHA1 of ts, min 3 hex, wee-slack style)  

---

## Phase 3: Buffer Management

- [x] Server + child hierarchy, localvars  
- [x] Lazy history + serial queue; members on focus  
- [x] History pagination — multi-page slow queue (`history_fetch_count`, page cap)  
- [x] Members pagination — configurable pages (default 3; **0 = unlimited**;
      soft max 500 ×200; still slow queue)  
- [x] Unknown members → **`users.info`** (slow, capped); no stub nicks  
- [x] Nicklist **Here / Away** groups + **`presence_sub`** (RTM); bots purged  
- [x] Thread buffers + replies — **open on demand** (`/cslack thread` / subscribe)  
- [x] `/cslack thread` without args → last parent with replies; `$hash`/N/ts  
- [x] Buflist trigger includes `weeslack` (export conf)  
- [x] Buffer close **frees wrapper** (`t_slack_buffer`); channel model kept;
      history/members state reset so reopen can re-fetch  
- [x] Live messages **reopen** closed channel buffers; history does not  
- [x] **`/cslack join` / `show #ch`** reopen member buffers without API join  
- [x] **`/cslack reconnect [all]`** re-issue `rtm.connect`  
- [x] **`look.leave_channel_on_buffer_close`** (default off)  
- [x] **`unread_count_display`** → WeeChat hotlist on buffer create  
- [x] `short_buffer_names` — `#channel` vs `team.#channel`  
- [x] Mute prefs from **`users.prefs.get`** applied after channel list  

---

## Phase 4: Message Handling

- [x] Live + history; tags include `slack_ts_<ts>`; localvars for timestamps  
- [x] Reactions + react/unreact; suffixes; `show_reaction_nicks` + colors  
- [x] Thread suffix **`$hash` + reply count**  
- [x] **`use_full_names`**, **`external_user_suffix`**  
- [x] Typing, mentions, mute tags, read markers  
- [x] Emoji shortcodes + custom map + `emoji_render_mode` + weemoji.json  
- [x] Custom emoji **images** via `/icat` when `look.icat_enabled` **and**
      `/icat` is registered (probe; compact tiles; cache under
      `data_dir/weeslack/emoji/`) — chat lines stay text; graphics use Kitty
- [x] Bold/italic/strikethrough honor config  
- [x] Thread-in-channel + `thread_broadcast_prefix`  
- [x] Edit/delete in-place; input `s///`, `+emoji`/`-emoji`, `//` escape  
- [x] Input `/cmd` → `chat.command`; files + download  
- [x] Blockquotes, Block Kit fallback, HTML unescape, attachments colors  

---

## Phase 5: Outgoing

- [x] postMessage / threads / reply / upload (libcurl)  
- [x] **`/cslack slash`**, `/me` → meMessage, underline map  

---

## Phase 6: Commands

- [x] Full set: connect/disconnect/migrate/register/unregister, channels, users,
      usergroups, history, talk/open, mute, status, create/invite, threads,
      react, stars, pin, search, download, whois, join/leave, IRC command_run  
- [x] **`debug`**, **`queue`**, **`version`**, **`info`** (status summary)  
- [x] Cursor/mouse `$hash` actions  

---

## Phase 7: Completions

- [x] Nicks (buffer nicklist / workspace-scoped users + `@nick`)  
- [x] Channels, threads `$hash`, topic, emoji, usergroups  

---

## Phase 8: Configuration

- [x] Multi-token, auto_connect, server_aliases, debug/record, background history  
- [x] **`auto_open_threads`**: `off` | `subscribed` | `all`  
- [x] download/icat/Xepher layout, shared_name_prefix, leave_on_buffer_close  
- [x] All major wee-slack look/color options wired  

---

## Phase 9: Advanced

- [x] Hdata / infolist  
- [x] `/upgrade` auto-reconnect  
- [x] ASAN build targets  
- [x] OAuth register + unregister (token retire beyond wee-slack)  

---

## Phase 10: RTM

- [x] Core message/lifecycle/presence/pin/emoji/thread subscribe events  
- [x] Reconnect with fresh rtm.connect; reconnect_url stored  

---

## Connect bootstrap (rate-limit safe)

```
rtm.connect
 → users.list / bots.list / emoji.list / usergroups.list
 → conversations.list → create buffers (NO history, NO members)
 → users.prefs.get → muted_channels
 → bootstrap quiet ~8s
 → focus → history (slow) + members
 → /cslack loadhistory | names | refresh as needed
 → emoji_changed → emoji.list only
 → WS drop → rtm.connect (fresh URL)
```

**Do not** reload/connect while Slack is still cooling.  
**Do not** `make install` over a loaded plugin — unload first.  
Prefer process restart over unload/load when hunting crashes.

---

## Intentionally remaining

None for feature parity. Optional future polish only (e.g. packaging,
CHANGELOG, live soak tests).

**Custom emoji graphics:** `look.icat_enabled` + registered `/icat` (Kitty
tiles under the message). Chat lines stay text.

**Members:** `look.members_max_pages` **0** = paginate until Slack has no
cursor (still slow queue). Non-zero caps pages (soft max 500).

---

## Recent fixes (2026-07-21)

| Issue | Fix |
|-------|-----|
| Upload/download used curl CLI | libcurl multi async |
| Custom emoji images | `/icat` + cache under `weeslack/emoji/` |
| Auto-open only subscribed | `auto_open_threads` off/subscribed/all |
| Multi-ws users mixed | workspace ownership + free on unregister |
| Buffer close leaked `t_slack_buffer` | free wrapper on close_cb |
| members hard max 15 | raised to 50 (config) |
| No status overview | `/cslack info` |
| Dead curl CLI proxy helper | removed (`hook_process` gone) |
| Closed buffer stayed gone | live reopen; join/show local reopen; history_state reset |
| No reconnect command | `/cslack reconnect [all]` |
| Version | **0.2.0** |
| members hard cap | **0 = unlimited**; soft max 500 |
| Web API used hook_url | libcurl multi form POST/GET |

Earlier gap-fix history lives in git; the checklist above is the source of
truth for parity status.
