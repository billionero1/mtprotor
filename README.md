# Bulldog MTProxy Fork
Production-focused MTProxy fork with **runtime hot reload of secrets/users** (no process restart required for add/remove/enable/disable).

## What This Fork Adds
- Runtime secret store in proxy process memory.
- Hot operations without service restart:
  - add secret
  - remove secret
  - enable/disable secret
- Existing active connections are not dropped during secret updates.
- Persistent state on disk (`secrets.tsv`) with restore on service start.
- Local admin API over Unix socket (HTTP/JSON + legacy line protocol).
- `proxyctl` CLI and interactive terminal menu.
- One-command installer for Ubuntu 24.04.

## Important Operational Notes
- Hot reload is currently **single-process mode only**.
- `--admin-socket` is intentionally blocked together with `--slaves` to avoid secret desync between worker processes.
- This does **not** mean one secret only. You can manage many secrets/users in one process.

## Quick Install (Ubuntu 24.04)
```bash
curl -fsSL https://raw.githubusercontent.com/billionero1/mtprotor/main/install.sh | sudo bash
```

Installer does:
1. optional full cleanup of old stacks
2. dependency install
3. clone + build fork
4. runtime user/dirs setup
5. Telegram config download (`proxy-secret`, `proxy-multi.conf`)
6. bootstrap secret + admin token setup
7. systemd install and start
8. health checks + ready links

After install:
- service: `mtproxy-fork.service`
- binary: `/usr/local/bin/mtproto-proxy-fork`
- CLI: `/usr/local/bin/proxyctl`
- menu: `/usr/local/bin/mtproxymenu`
- bot SSH dispatcher: `/usr/local/bin/proxybot-dispatch`
- bot SSH password setup: `/usr/local/bin/mtproxybot-setup`
- env/config: `/etc/default/mtproxy-fork`
- secrets state: `/var/lib/mtproxy-fork/secrets.tsv`

## Telegram Link Format (iOS)
For iOS clipboard import, use:
```text
https://t.me/proxy?server=<IP_OR_HOST>&port=<PORT>&secret=dd<SECRET32>
```
`tg://proxy?...` is also printed, but `https://t.me/proxy?...` is more reliable for paste flow.

## Uninstall
```bash
curl -fsSL https://raw.githubusercontent.com/billionero1/mtprotor/main/uninstall.sh | sudo bash
```

Optional flags:
- `--purge-data`
- `--purge-source`

## Day-1 Operations

### Service
```bash
proxyctl service status
proxyctl service status --full
proxyctl service status --json
proxyctl service restart
proxyctl service logs
proxyctl health
```

`proxyctl service status` is compact by default (active status, PID, memory, CPU, listener, secret counters).

### Links
```bash
proxyctl link
proxyctl link --plain
proxyctl link --secret <hex32>
```

### Secrets (users)
```bash
proxyctl secret list
proxyctl secret add <secret_hex_or_dd_or_ee> --label user123
proxyctl secret disable <secret_hex>
proxyctl secret enable <secret_hex>
proxyctl secret remove <secret_hex>
```

### Auto issue user secret
```bash
proxyctl secret issue user123
proxyctl secret issue user123 --days 30
```

This prints JSON with generated secret and ready `https://t.me/proxy?...` link.

## Interactive Terminal Menu
```bash
mtproxymenu
# or
proxyctl menu
```

Menu includes:
- service status/health/logs
- default links
- list users
- issue/disable/enable/remove user
- change client port
- change bootstrap secret
- change public host
- restart service

When link-related params change, default links are reprinted.

## Config Management (`/etc/default/mtproxy-fork`)
Show current runtime config:
```bash
proxyctl config show
```

Update config:
```bash
proxyctl config set client-port 443
proxyctl config set stats-port 8888
proxyctl config set public-host 1.2.3.4
proxyctl config set bootstrap-secret <hex32>
proxyctl config set admin-token <token>
proxyctl config set secret-prefix dd
```

Notes:
- Port/token changes restart service when required.
- Bootstrap secret change is applied via runtime API and persisted.

## External Control for Bot (from another server)
Recommended production method: **SSH to proxy server and run `proxyctl bot ...`**.

If your third-party bot requires **IP + login + password**, use:
```bash
sudo mtproxybot-setup --user mtproxybot --password '<STRONG_PASSWORD>' --allow-from <BOT_SERVER_IP>
```
This returns JSON with connection parameters and enables forced command mode for safe command-only access.

Examples:
```bash
ssh bulldogtg1 "proxyctl bot health"
ssh bulldogtg1 "proxyctl bot issue user_987 --days 30"
ssh bulldogtg1 "proxyctl bot disable <hex32>"
ssh bulldogtg1 "proxyctl bot revoke <hex32>"
```

This gives full lifecycle control for subscriptions:
1. issue on payment
2. disable on grace/failure
3. remove on final expiry

### Security recommendations for bot integration
- Create dedicated SSH key for bot.
- Restrict source IPs in firewall.
- Prefer dedicated low-privilege account with limited sudo rule for `proxyctl` if needed.
- Keep admin API on Unix socket local-only (default). Do not expose to internet.
- Use forced command mode with `proxybot-dispatch` (no shell, no port forwarding).

Full bot template:
- `docs/BOT_INTEGRATION_TEMPLATE.md`

## Local Admin API
Socket path (default): `/var/lib/mtproxy-fork/admin.sock`

Auth:
- `X-Admin-Token: <token>`

Endpoints:
- `GET /v1/status`
- `GET /v1/secrets`
- `POST /v1/secrets`
- `DELETE /v1/secrets/{secret}`
- `PATCH /v1/secrets/{secret}/enable`
- `PATCH /v1/secrets/{secret}/disable`

Accepted secret formats:
- plain: `<32 hex>`
- dd: `dd<32 hex>`
- ee/tls-like: `ee<32 hex>[optional hex suffix]`

## Hot Reload Behavior
- Secret updates affect **new handshakes immediately**.
- Existing active sessions continue naturally.
- State file is atomically persisted and restored after restart.

## Build From Source
```bash
apt-get update
apt-get install -y build-essential git curl libssl-dev zlib1g-dev
make -j"$(nproc)"
```

Binary path:
- `objs/bin/mtproto-proxy`

## Systemd Unit Template
- `systemd/mtproxy-fork.service`

Installed unit:
- `/etc/systemd/system/mtproxy-fork.service`

## Current Limitation (Planned v2)
`--slaves` + hot reload is not enabled yet. Planned next stage:
- inter-process sync/shared state for multi-process hot reload.
