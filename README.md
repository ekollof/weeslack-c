# weeslack-c

A native C plugin for [WeeChat](https://weechat.org/) providing Slack
protocol support. Designed as a migration path from the Python-based
[wee-slack](https://github.com/wee-slack/wee-slack) plugin.

**Status:** Early development. Core connectivity (HTTP client, WebSocket,
reconnection) is implemented. Full wee-slack feature parity is in progress.

> **Note:** This project was developed with AI assistance.

## Features

- Reads tokens from wee-slack config (`/cslack migrate`)
- Slack Web API via curl (`hook_process_hashtable`)
- RTM WebSocket connection with automatic reconnection
- Exponential backoff on connection loss
- Proxy support via `weechat.network.proxy_curl`

## Building

### Dependencies

- WeeChat development headers (`weechat-dev` on Debian/Ubuntu,
  `weechat-devel` on Fedora/RHEL)
- json-c (JSON parsing)
- OpenSSL (TLS for WebSocket)
- pkg-config
- GCC or compatible C11 compiler

### Compile

```sh
make
```

### Install

Installs to `~/.local/share/weechat/plugins/` as a normal user,
`/usr/local/lib/weechat/` as root.

```sh
make install
```

### Uninstall

```sh
make uninstall
```

## Usage

### Migrating from wee-slack

If you already have wee-slack configured, import your token:

```
/cslack migrate
```

This copies the token from `plugins.var.python.slack.slack_api_token` to
the plugin's own config file (`weeslack.conf`).

### Manual configuration

```
/set weeslack.workspace.token xoxp-your-token-here
```

For session tokens (xoxc) with cookies:

```
/set weeslack.workspace.token xoxc-token:cookie
```

### Connect

```
/cslack connect
```

### Commands

| Command | Description |
|---------|-------------|
| `/cslack connect` | Connect to Slack using configured token |
| `/cslack disconnect` | Disconnect from Slack |
| `/cslack migrate` | Import token from wee-slack (python) config |
| `/cslack list` | List loaded workspaces |

## Project Structure

```
weeslack.c              Plugin entry, config, commands, data model
slack_http.c / .h       HTTP client + paced request queue
slack_ws.c / .h         WebSocket client (RTM)
slack_event.c / .h      Events, history, members, upload helpers
slack_buffer.c / .h     Buffers, nicklist, localvars
slack_data.c / .h       Users, channels, messages, bots
tools/                  Agent automation (weechat-cmd + debug socket)
AGENTS.md               Coding standards and agent rules
TODO.md                 Feature status
```

### Live testing helpers

Vendored under [`tools/`](tools/README.md): `weechat-cmd.sh` and
`weechat_debug_socket.py` so agents can drive a running WeeChat without the
FIFO. See that README for install and safety rules.

## License

BSD 2-Clause. See [LICENSE](LICENSE) for details.
