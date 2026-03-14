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
- Optional Bot HTTP Bridge (`host + base_path + login/password + api_key`) for panel-like bot integration.
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
6. bootstrap secret + admin token setup (stored in `/etc/mtproxy-fork/admin.token`)
7. auto-generate bot SSH login/password (x3-ui style) + forced-command profile with required `allow-from`
8. auto-generate bot HTTP API profile (base_path + login/password + api_key + allow-from)
9. systemd install/start + expire-sync timer (+ optional bot api service)
10. health checks + ready links

Installer validation notes:
- Bot SSH username must match Linux login pattern: `^[a-z_][a-z0-9_-]{1,30}$` (lowercase).
- If invalid username is entered, installer now shows reason and suggests normalized value.

After install:
- service: `mtproxy-fork.service`
- binary: `/usr/local/bin/mtproto-proxy-fork`
- runtime launcher: `/usr/local/bin/mtproxy-fork-run`
- CLI: `/usr/local/bin/dogctl` (compat: `/usr/local/bin/proxyctl`)
- menu: `/usr/local/bin/dogmenu` (compat: `/usr/local/bin/mtproxymenu`)
- bot SSH dispatcher: `/usr/local/bin/proxybot-dispatch`
- bot SSH password setup: `/usr/local/bin/mtproxybot-setup`
- bot HTTP bridge daemon: `/usr/local/bin/proxybot-httpd`
- bot HTTP profile setup: `/usr/local/bin/mtproxybot-http-setup`
- env/config: `/etc/default/mtproxy-fork`
- admin token file: `/etc/mtproxy-fork/admin.token`
- secrets state: `/var/lib/mtproxy-fork/secrets.tsv`
- bot credentials cache: `/etc/mtproxy-fork/bot-access.env`
- bot http credentials cache: `/etc/mtproxy-fork/bot-http.env`
- expire timer: `mtproxy-fork-expire-sync.timer` (disables expired users)
- bot api service: `mtproxy-fork-bot-api.service`

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
dogctl runtime status
dogctl runtime restart
dogctl runtime logs
dogctl runtime logs-live
dogctl runtime update
proxyctl runtime status
proxyctl runtime restart
proxyctl runtime logs
proxyctl runtime logs-live
proxyctl runtime update
proxyctl service status
proxyctl service status --full
proxyctl service status --json
proxyctl service restart
proxyctl service logs
proxyctl service update
proxyctl health
proxyctl bot api show
proxyctl bot api status
proxyctl bot api restart
```

`proxyctl service status` is compact by default (active status, PID, memory, CPU, listener, secret counters).
`proxyctl runtime ...` is a branding alias for the same operations.
`dogctl ...` is a brand wrapper over `proxyctl ...`.

### Links
```bash
proxyctl link
proxyctl link --plain
proxyctl link --secret <hex32>
```

### Secrets (users)
```bash
proxyctl secret list
proxyctl secret list --table
proxyctl secret add <secret_hex_or_dd_or_ee> --label user123
proxyctl secret disable <secret_hex>
proxyctl secret enable <secret_hex>
proxyctl secret remove <secret_hex>
proxyctl secret expire-disable
```

### Auto issue user secret
```bash
proxyctl secret issue user123
proxyctl secret issue user123 --days 30
```

This prints JSON with generated secret and ready `https://t.me/proxy?...` link.

`active_until` is enforced by proxy runtime: after expiry, new handshakes are blocked automatically.
Additionally, expire-sync timer runs every minute and marks expired secrets as disabled.

## Interactive Terminal Menu
```bash
dogmenu
# or
mtproxymenu
# or
dogctl console
```

Menu includes:
- runtime status/health/logs
- runtime logs in real time (`journalctl -f` style)
- runtime self-update from git repo (`fetch + rebuild + reinstall + restart`)
- default links
- list users (table: active/expired/disabled + active_until)
- issue/disable/enable/remove user
- change client port
- change bootstrap secret
- change public host
- stealth presets/mode (`standard`, `max-camouflage`, `compatibility`, or custom `plain/dd/ee`)
- set/disable TLS cover domain (for domain camouflage)
- restart service
- bot SSH settings (change login/password/allow-from)
- bot HTTP API settings (change base_path/login/password/api_key/allow-from)
- force run expired-user disable

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
proxyctl config set secret-prefix ee
# alias:
proxyctl config set stealth-mode plain
proxyctl config set stealth-preset standard
proxyctl config set tls-domain www.google.com
proxyctl config set tls-domain off
```

Notes:
- Port/token changes restart service when required.
- Bootstrap secret change is applied via runtime API and persisted.
- `tls-domain` change restarts runtime and enables TLS domain camouflage mode.
- When `tls-domain` is enabled, stealth mode is forced to `ee`.
- `runtime update` uses `SOURCE_DIR` and `SOURCE_REF` from `/etc/default/mtproxy-fork` (defaults: `/opt/mtproxy-fork-src`, `main`).

Stealth modes (simple):
- `plain` = no prefix, max compatibility, minimum camouflage.
- `dd` = default balanced mode (good compatibility + practical camouflage).
- `ee` = more TLS-like behavior, maximum camouflage from available modes.

TLS cover domain (optional):
- Set with `proxyctl config set tls-domain <domain>` (example: `www.google.com`, `vk.com`, `yandex.ru`).
- Generated links include `ee<secret><domain_hex_suffix>`.
- Disable with `proxyctl config set tls-domain off`.

## External Control for Bot (from another server)
Recommended production method for panel-like bot integration: **Bot HTTP Bridge**.

Show/rotate credentials:
```bash
proxyctl bot api show
proxyctl bot api rotate-password
proxyctl bot api rotate-key
proxyctl bot api set --user botapi --port 9443 --base-path /api-xxxx --allow-from <BOT_SERVER_IP>
```

Service control:
```bash
proxyctl bot api status
proxyctl bot api restart
```

HTTP contract (all JSON):
- `GET <base_path>/health`
- `GET <base_path>/secrets`
- `POST <base_path>/issue` body: `{"label":"user_1001","days":30}`
- `POST <base_path>/enable` body: `{"secret":"<hex32>"}`
- `POST <base_path>/disable` body: `{"secret":"<hex32>"}`
- `POST <base_path>/revoke` body: `{"secret":"<hex32>"}`

Required auth headers:
- `Authorization: Basic base64(login:password)`
- `X-API-Key: <api_key>`

Example:
```bash
curl -sS -u 'botapi:<PASSWORD>' -H 'X-API-Key: <API_KEY>' \
  "http://<HOST>:9443/<BASE_PATH>/health"
```

Security defaults:
- source IP/CIDR allowlist (`allow-from`)
- per-IP rate limit
- temporary block on repeated auth failures
- local proxy runtime API remains unix-socket only

SSH forced-command remains available as fallback:
```bash
proxyctl bot ssh show
```

If your third-party bot requires **IP + login + password**, use:
```bash
sudo mtproxybot-setup --show
sudo mtproxybot-setup --regen-password --user mtproxybot --allow-from <BOT_SERVER_IP>
```
This returns JSON with connection parameters and enables forced command mode for safe command-only access.

Installer already auto-generates these credentials and prints them once.

Manage SSH bot credentials later:
```bash
proxyctl bot ssh show
proxyctl bot ssh rotate-password --user mtproxybot --allow-from <BOT_SERVER_IP>
proxyctl bot ssh set --user mtproxybot --password '<STRONG_PASSWORD>' --allow-from <BOT_SERVER_IP>
```

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
- Keep runtime admin API on Unix socket local-only (default).
- Restrict bot API source via allow-from CIDR and firewall.
- Prefer private network path (WireGuard/Tailscale) between bot server and proxy node.
- Rotate bot api password and api key periodically.
- Keep SSH forced-command channel as emergency fallback.

Full bot template:
- `docs/BOT_INTEGRATION_TEMPLATE.md`

## Local Admin API
Socket path (default): `/var/lib/mtproxy-fork/admin.sock`

Auth:
- `X-Admin-Token: <token>` (runtime reads token from `/etc/mtproxy-fork/admin.token`, not from process argv)

Endpoints:
- `GET /v1/status`
- `GET /v1/secrets`
- `POST /v1/secrets`
- `POST /v1/secrets/expire_disable`
- `DELETE /v1/secrets/{secret}`
- `PATCH /v1/secrets/{secret}/enable`
- `PATCH /v1/secrets/{secret}/disable`

Legacy line-protocol includes: `PING`, `STATUS`, `LIST`, `ADD`, `REMOVE`, `ENABLE`, `DISABLE`, `EXPIRE_DISABLE`.

Accepted secret formats:
- plain: `<32 hex>`
- dd: `dd<32 hex>[optional hex suffix]`
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
- `systemd/mtproxy-fork-expire-sync.service`
- `systemd/mtproxy-fork-expire-sync.timer`

Installed unit:
- `/etc/systemd/system/mtproxy-fork.service`
- `/etc/systemd/system/mtproxy-fork-expire-sync.service`
- `/etc/systemd/system/mtproxy-fork-expire-sync.timer`

## Current Limitation (Planned v2)
`--slaves` + hot reload is not enabled yet. Planned next stage:
- inter-process sync/shared state for multi-process hot reload.
