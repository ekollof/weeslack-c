# TODO: wee-slack Feature Parity

**Last update:** 2026-07-20 — profile status, create/invite/showmuted,
never_away, /me → chat.meMessage; weemoji.json

**Markers:** `[x]` done · `[~]` partial · `[ ]` missing

---

## Phase 1: Core Connectivity

- [x] WebSocket RTM — TLS+SNI, masking, ping/pong, reconnect backoff  
- [x] Reconnect re-issues **`rtm.connect`** (fresh URL; stale WS URLs expire)  
- [x] RTM `goodbye` / `error` → reconnect path  
- [x] HTTP — `hook_url`, Bearer + cookie, `ipresolve=v4`, POST form bodies  
- [x] **Request queue (wee-slack model)**  
  - Fast queue + **slow queue** (history/members; ≤1 promote/sec)  
  - Max **2** concurrent in-flight  
  - On 429: **global cooldown** from `Retry-After`, re-queue job (no stampede)  
  - Soft failures: quadratic backoff re-queue  
  - `conversations.mark` is **droppable** under cooldown  
- [x] Proxy via WeeChat globals — WS, HTTP `hook_url`, **and** curl upload/download (`-x`)  
- [~] Binary file **PUT** / download still use curl `hook_process` (not `hook_url`); proxy wired  

---

## Phase 2: Data Model

- [x] SlackTS / SlackUser / SlackChannel / SlackSubteam  
- [x] Workspace stable `id` vs display `name`  
- [x] Custom emoji map via **`emoji.list`** (+ **refresh-only** on `emoji_changed`; no re-bootstrap)  
- [x] Bots — `is_bot` / `is_app_user` registered in bot map; **hidden from nicklist** (wee-slack style)  
- [~] SlackMessage — list model; edit not in-place line rewrite  
- [x] Reaction list maintained on RTM add/remove (notice lines; no in-place rewrite)  

---

## Phase 3: Buffer Management

- [x] Server + child hierarchy, localvars  
- [x] Lazy history + serial queue; members on focus  
- [x] History pagination — up to **5×100** on slow queue; accumulate → chronological flush  
- [x] Members pagination — up to **3×200** on slow queue  
- [x] Unknown members → **`users.info`** (slow, capped); no stub nicks  
- [x] Nicklist **Here / Away** groups + **`presence_sub`** (RTM); bots purged  
- [x] Thread buffers + replies — **open on demand** (`/cslack thread` / subscribe)  
- [x] Buflist trigger includes `weeslack` (export conf)  
- [~] Buffer close keeps model (by design)  
- [x] `short_buffer_names` — `#channel` vs `team.#channel`  
- [x] Mute prefs from **`users.prefs.get`** applied after channel list  

---

## Phase 4: Message Handling

- [x] Live + history; tags include `slack_ts_<ts>`; `localvar_slack_timestamp` + `last_message_ts`  
- [x] Reactions + react/unreact commands (+ model update)  
- [x] Reaction **suffixes** on history/live print (`[:name:count]`, own = lightgreen)  
- [x] Typing (title indicator), mentions (`<@U>`, `<!here>`, `<!subteam^…>`), mute tags, read markers  
- [x] Emoji shortcodes + custom map + `emoji_render_mode`  
- [x] **weemoji.json** from WeeChat data/sharedir (standard unicode map + completion)  
- [x] Bold/italic/strikethrough honor config (`render_*_as`)  
- [x] Thread-in-channel uses `thread_broadcast_prefix`  
- [~] Edit/delete — notice lines (WeeChat has no public line-edit API)  
- [x] Files — display (size/mime/download URL) + **`/cslack download <url>`**  
- [x] Message markdown render uses capacity-tracked `snprintf` (no `sprintf`)  
- [x] Join/leave show resolved names  
- [x] `colorize_private_chats` gates nick color on DM/MPDM  
- [x] Blockquotes, Block Kit text fallback, HTML entity unescape  
- [x] Self-mention → `notify_highlight`  

---

## Phase 5: Outgoing

- [x] postMessage / threads / reply  
- [x] Upload pipeline (getUploadURL → PUT → complete); PUT uses proxy when configured  
- [~] Slash: posted as plain text (real slash API is app-specific / low ROI)  
- [x] Input emoji shortcodes  
- [x] `/me text` → **`chat.meMessage`**  

---

## Phase 6: Commands

- [x] Full prior set + workspace-id fix + focused buffer  
- [x] **`download`**, **`stars`**, **`star`**, **`unstar`**  
- [x] **`linkarchive` / pin / star / react / reply** default to last printed message ts  
- [x] **`whois`** (+ live presence), **`join`**, **`leave`/`part`**, **`refresh`**, **`names`**  
- [x] **`status`** profile emoji/text (`users.profile.set`) + `-delete`; legacy dnd/away/active  
- [x] **`create`** (`conversations.create` [-private]), **`invite`**, **`showmuted`**  
- [x] Stars / search list polish  
- [~] subscribe = local thread notify only  

---

## Phase 7: Completions

- [x] Hooks wired to `/cslack`  
- [x] Nick completion prefers **buffer nicklist**, else all users  
- [x] Join completion via `%(slack_channels)`  

---

## Phase 8: Configuration

### Used

- [x] token, emoji_render_mode, typing indicator (title), thread_messages_in_channel  
- [x] thread_broadcast_prefix, render_bold_as / render_italic_as / **render_strikethrough_as**  
- [x] **download_path**, **short_buffer_names**, **colorize_private_chats**  
- [x] color options (typing/deleted/edited/thread/muted)  
- [x] `color.thread_suffix` = `multiple` uses nick color for reply count suffix  
- [x] **`look.never_away`** — 5‑min timer sets presence `auto` when enabled  

---

## Phase 9: Advanced

- [x] Hdata / infolist  
- [x] **`/upgrade` auto-reconnect** (timer + token after upgrade read)  
- [x] stars.list / stars.add / stars.remove  
- [x] pin / search / permalink / react  

---

## Phase 10: RTM

- [x] Core events + lifecycle (create/rename/archive/leave/delete/join)  
- [x] `channel_joined` / `im_open` / `im_created` (member vs created semantics)  
- [x] pin / emoji_changed / dnd / presence / team_join  
- [x] `me_message`, pin subtypes, channel_name, purpose  
- [x] RTM `reconnect_url` stored for next WS session  
- [x] Rate-limit + auth messaging  
- [x] Reconnect with fresh rtm.connect  
- [x] `user_change` → DM buffer title from status emoji/text  

---

## Connect bootstrap (rate-limit safe)

```
rtm.connect
 → users.list (is_bot → bot map + hide from nicklist) / bots.list / emoji.list / usergroups.list
 → conversations.list → create buffers (NO history, NO members)
 → users.prefs.get → apply muted_channels
 → bootstrap quiet ~8s (buffer_switch ignores history/members)
 → user focuses a buffer → history (slow, ≤5×100) + members (≤3×200)
      unknown members → users.info (slow, capped); bots never nicklisted
 → /cslack loadhistory | names | refresh as needed
 → emoji_changed → emoji.list only
 → WS drop / goodbye → rtm.connect (fresh URL), no full bootstrap
```

**Do not** reload/connect while Slack is still cooling from earlier storms.

---

## Intentionally remaining (low ROI / platform limits)

1. True in-place buffer line rewrite (no WeeChat plugin API)  
2. Full custom-emoji image rendering in TUI (URLs only)  
3. Real Slack slash-command protocol for user tokens  
4. Multi-workspace UI beyond one `default` id  
5. Auto-open every thread on live reply (rejected — rate-limit / buffer storm)  
6. Unlimited history/members pagination (hard caps keep the queue healthy)  
7. Binary upload via `hook_url` (raw PUT still needs curl/process)  

---

## Gap-fix notes (2026-07-20)

| Issue | Fix |
|-------|-----|
| `emoji_changed` re-chained full bootstrap | emoji cb: bootstrap only when `user_data` set |
| Every thread reply opened a buffer + `fetch_replies` | open only if subscribed / explicit `/cslack thread` |
| Bots in nicklist (Drive, USLACKBOT) | `is_bot`/`is_app_user` + purge; no member stubs; users.info for unknowns |
| Stale WS reconnect URL | re-issue `rtm.connect` |
| Dead config options | wired short names / private color / strikethrough |
| HTTP / curl ignored proxy | `slack_http_*_proxy` helpers |
| History/members single page | capped multi-page slow queue |
| No mute from Slack prefs | `users.prefs.get` after channel list |
| Flat nicklist | Here/Away groups + `nicklist_display_groups=1` |
| Here/Away stuck on Away | RTM `presence_sub` after hello/users.list |
| Mute/highlight prefs only on connect | also `pref_change` RTM |
| Reactions only as notices | also suffix `[:name:n]` on message print |
| No `reconnect_url` handling | store URL on workspace |
| `channel_created` opened + forced member | created = model only; join/open separate |
| DM title stale on status change | `user_change` updates peer DM title |
| `/cslack status` only dnd/away | also profile emoji/text via `users.profile.set` |
| No create/invite/showmuted | conversations.create / invite + muted list |
| Appear away while idle | optional `look.never_away` timer |
| `/me` posted as plain text | `chat.meMessage` |
