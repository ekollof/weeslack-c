# TODO: wee-slack Feature Parity

**Last update:** 2026-07-20 — gap audit + fixes (emoji bootstrap split, thread
auto-open, linkarchive ts, dead config, proxy, reactions, mentions)

**Markers:** `[x]` done · `[~]` partial · `[ ]` missing

---

## Phase 1: Core Connectivity

- [x] WebSocket RTM — TLS+SNI, masking, ping/pong, reconnect backoff  
- [x] HTTP — `hook_url`, Bearer + cookie, `ipresolve=v4`, POST form bodies  
- [x] **Request queue (wee-slack model)**  
  - Fast queue + **slow queue** (history/members; ≤1 promote/sec)  
  - Max **2** concurrent in-flight  
  - On 429: **global cooldown** from `Retry-After`, re-queue job (no stampede)  
  - Soft failures: quadratic backoff re-queue  
  - `conversations.mark` is **droppable** under cooldown  
- [x] Proxy via WeeChat globals — WS (`proxy_curl`) **and** HTTP (`hook_url` proxy URL)  
- [~] Binary file **PUT** / download still use curl `hook_process` (not `hook_url`)  

---

## Phase 2: Data Model

- [x] SlackTS / SlackUser / SlackChannel / SlackSubteam  
- [x] Workspace stable `id` vs display `name`  
- [x] Custom emoji map via **`emoji.list`** (+ **refresh-only** on `emoji_changed`; no re-bootstrap)  
- [~] Bots — `bots.list` optional (often unavailable on xoxp)  
- [~] SlackMessage — list model; edit not in-place line rewrite  
- [x] Reaction list maintained on RTM add/remove (notice lines; no in-place rewrite)  

---

## Phase 3: Buffer Management

- [x] Server + child hierarchy, localvars  
- [x] Lazy history + serial queue; members on focus  
- [x] Thread buffers + replies — **open on demand** (`/cslack thread` / subscribe), not every live reply  
- [x] Buflist trigger includes `weeslack` (export conf)  
- [~] Buffer close keeps model (by design)  
- [x] `short_buffer_names` — `#channel` vs `team.#channel`  

---

## Phase 4: Message Handling

- [x] Live + history; tags include `slack_ts_<ts>`; `localvar_slack_timestamp` + `last_message_ts`  
- [x] Reactions + react/unreact commands (+ model update)  
- [x] Typing (title indicator), mentions (`<@U>`, `<!here>`, `<!subteam^…>`), mute tags, read markers  
- [x] Emoji shortcodes + custom map + `emoji_render_mode`  
- [x] Bold/italic/strikethrough honor config (`render_*_as`)  
- [x] Thread-in-channel uses `thread_broadcast_prefix` (also when no thread buffer open)  
- [~] Edit/delete — notice lines (WeeChat has no public line-edit API)  
- [~] Files — display + **`/cslack download <url>`** to `look.download_path` (`weechat_mkdir_parents`)  
- [x] Join/leave show resolved names, not raw U… ids  
- [x] `colorize_private_chats` gates nick color on DM/MPDM  

---

## Phase 5: Outgoing

- [x] postMessage / threads / reply  
- [x] Upload pipeline (getUploadURL → PUT → complete)  
- [~] Slash: posted as plain text  
- [x] Input emoji shortcodes  

---

## Phase 6: Commands

- [x] Full prior set + workspace-id fix + focused buffer  
- [x] **`download`**, **`stars`**, **`star`**, **`unstar`**  
- [x] **`linkarchive`** without args uses last printed message ts  
- [~] subscribe = local thread notify only  

---

## Phase 7: Completions

- [x] Hooks wired to `/cslack`  
- [x] Nick completion prefers **buffer nicklist**, else all users  

---

## Phase 8: Configuration

### Used

- [x] token, emoji_render_mode, typing indicator (title), thread_messages_in_channel  
- [x] thread_broadcast_prefix, render_bold_as / render_italic_as / **render_strikethrough_as**  
- [x] **download_path**, **short_buffer_names**, **colorize_private_chats**  
- [x] color options (typing/deleted/edited/thread/muted)  
- [x] `color.thread_suffix` = `multiple` uses nick color for reply count suffix  

### Still light

- [~] History / members single-page only (rate-limit safe; no cursor pagination)  
- [~] Stars / search list polish (raw ids, caps)  

---

## Phase 9: Advanced

- [x] Hdata / infolist  
- [x] **`/upgrade` auto-reconnect** (timer + token after upgrade read)  
- [x] stars.list / stars.add / stars.remove  
- [x] pin / search / permalink / react  

---

## Phase 10: RTM

- [x] Core events  
- [x] emoji_changed → re-fetch emoji.list **without** re-running channel bootstrap  
- [x] Rate-limit + auth messaging  

---

## Connect bootstrap (rate-limit safe)

```
rtm.connect
 → users.list / bots.list / emoji.list / usergroups.list  (via HTTP queue)
 → conversations.list → create buffers (NO history, NO members)
 → bootstrap quiet ~8s (buffer_switch ignores history/members)
 → user focuses a buffer → history (slow lane) + members once
 → /cslack loadhistory forces re-fetch
 → emoji_changed → emoji.list only (no conversations.list)
```

**Do not** reload/connect while Slack is still cooling from earlier storms.

---

## Intentionally remaining (low ROI / platform limits)

1. True in-place buffer line rewrite (no WeeChat plugin API)  
2. Full custom-emoji image rendering in TUI (URLs only)  
3. Real Slack slash-command protocol for user tokens  
4. Multi-workspace UI beyond one `default` id  
5. Auto-open every thread on live reply (rejected — rate-limit / buffer storm)  

---

## Gap-fix notes (2026-07-20)

Prior TODO marked many items complete while code still had:

| Issue | Fix |
|-------|-----|
| `emoji_changed` re-chained full bootstrap | emoji cb: bootstrap only when `user_data` set |
| Every thread reply opened a buffer + `fetch_replies` | open only if subscribed / explicit `/cslack thread` |
| `linkarchive` read never-set localvar | set `localvar_slack_timestamp` + `channel->last_message_ts` on print |
| Dead config: short names, private color, strikethrough | wired |
| HTTP ignored proxy | `slack_http_apply_proxy` from `proxy_curl` |
| Reaction model updated from wrong JSON | `slack_message_reaction_add/remove` |
| `header=1` broke JSON parse | removed earlier (connect fix) |
