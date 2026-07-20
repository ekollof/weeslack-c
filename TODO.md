# TODO: wee-slack Feature Parity

**Last update:** 2026-07-20 — remaining-gap pass (POST bodies, emoji.list, download, stars, upgrade reconnect, config wiring)

**Markers:** `[x]` done · `[~]` partial · `[ ]` missing

---

## Phase 1: Core Connectivity

- [x] WebSocket RTM — TLS+SNI, masking, ping/pong, reconnect backoff  
- [x] HTTP — `hook_url`, Bearer + cookie, `ipresolve=V4`, POST form bodies  
- [x] **Request queue (wee-slack model)**  
  - Fast queue + **slow queue** (history/members; ≤1 promote/sec)  
  - Max **2** concurrent in-flight  
  - On 429: **global cooldown** from `Retry-After`, re-queue job (no stampede)  
  - Soft failures: quadratic backoff re-queue  
  - `conversations.mark` is **droppable** under cooldown  
- [x] Proxy via WeeChat globals  
- [~] Binary file **PUT** still uses curl

---

## Phase 2: Data Model

- [x] SlackTS / SlackUser / SlackChannel / SlackSubteam  
- [x] Workspace stable `id` vs display `name`  
- [x] Custom emoji map via **`emoji.list`** (+ refresh on `emoji_changed`)  
- [~] Bots — `bots.list` optional (often unavailable on xoxp)  
- [~] SlackMessage — list model; edit not in-place line rewrite  

---

## Phase 3: Buffer Management

- [x] Server + child hierarchy, localvars  
- [x] Lazy history + serial queue; members on focus  
- [x] Thread buffers + replies  
- [x] Buflist trigger includes `weeslack` (export conf)  
- [~] Buffer close keeps model (by design)  

---

## Phase 4: Message Handling

- [x] Live + history; tags include `slack_ts_<ts>`  
- [x] Reactions + react/unreact commands  
- [x] Typing, mentions, mute tags, read markers  
- [x] Emoji shortcodes + custom map + `emoji_render_mode`  
- [x] Bold/italic honor `render_bold_as` / `render_italic_as`; safer `_italic_` boundaries  
- [x] Thread-in-channel uses `thread_broadcast_prefix`  
- [~] Edit/delete — notice lines (WeeChat has no public line-edit API)  
- [~] Files — display + **`/cslack download <url>`** to `look.download_path`  

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
- [~] subscribe = local thread notify only  

---

## Phase 7: Completions

- [x] Hooks wired to `/cslack`  
- [x] Nick completion prefers **buffer nicklist**, else all users  

---

## Phase 8: Configuration

### Used

- [x] token, emoji_render_mode, typing indicator, thread_messages_in_channel  
- [x] thread_broadcast_prefix, render_bold_as, render_italic_as  
- [x] **download_path**  
- [x] color options (typing/deleted/edited/thread/muted)  

### Still light / unused

- [~] `short_buffer_names` — short names always set today  
- [~] `colorize_private_chats` — nicks already colored; option not a hard gate  
- [ ] `render_strikethrough_as` — still hardcoded red strike styling  

---

## Phase 9: Advanced

- [x] Hdata / infolist  
- [x] **`/upgrade` auto-reconnect** (timer + token after upgrade read)  
- [x] stars.list / stars.add / stars.remove  
- [x] pin / search / permalink / react  

---

## Phase 10: RTM

- [x] Core events  
- [x] emoji_changed → re-fetch emoji.list  
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
```

**Do not** reload/connect while Slack is still cooling from earlier storms.

---

## Intentionally remaining (low ROI / platform limits)

1. True in-place buffer line rewrite (no WeeChat plugin API)  
2. Full custom-emoji image rendering in TUI (URLs only)  
3. Real Slack slash-command protocol for user tokens  
4. Multi-workspace UI beyond one `default` id  

---

## New commands (this pass)

| Command | Action |
|---------|--------|
| `/cslack download <url>` | Auth download to `weeslack.look.download_path` |
| `/cslack stars` | List stars |
| `/cslack star <ts>` | Star message |
| `/cslack unstar <ts>` | Unstar message |
