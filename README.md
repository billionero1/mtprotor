# mtprotor

Production-minded runtime manager for Telegram MTProxy secrets with **real hot reload** (no daemon restart).

`mtprotor` keeps secrets in runtime memory, updates them via local API/CLI, and applies changes immediately for new connections while existing active streams continue.

## What This Project Solves

Official `mtproto-proxy` requires restart/reload workflow to apply secret changes. `mtprotor` adds a control layer that provides:

- hot add/remove/enable/disable of secrets
- immediate effect on new connections
- no restart of the main runtime process
- preservation of active connections by default
- persistent state on disk
- local admin API + CLI
- systemd/install/uninstall tooling

## Language Choice

Implemented in **Go** for:

- high networking throughput
- simple and reliable concurrency model
- single-binary deployment
- low operational complexity on Ubuntu VPS

## Architecture (MVP)

1. Client connects to `mtprotor` listener.
2. Runtime reads first 64 bytes of MTProxy handshake and matches against in-memory enabled secrets.
3. If secret is valid, runtime routes connection to the worker process assigned to that secret.
4. Worker handles protocol compatibility with Telegram.
5. Existing active streams continue even if secret set changes.

Hot updates are done through local Unix-socket API (CLI uses that API).

## Repository Structure

- `cmd/mtprotor` - binary entrypoint
- `internal/app` - CLI/daemon command wiring
- `internal/runtime` - core runtime, hot secret store, connection routing
- `internal/handshake` - secret normalization + MTProxy handshake matching
- `internal/worker` - per-secret worker lifecycle (command/builtin)
- `internal/storage` - persistent state (atomic JSON)
- `internal/api` - local HTTP API over Unix socket
- `internal/client` - API client for CLI
- `examples` - dev/prod config examples
- `systemd` - service unit
- `scripts` - install/uninstall/wrapper scripts
- `docs` - architecture notes

## Current Secret Format Support

- 16-byte hex secret (`32` hex chars)
- `dd`/`ee` prefixed formats (first 16 bytes after prefix are used for match key)

## Build

```bash
go build -o bin/mtprotor ./cmd/mtprotor
```

## Quick Start (Local Dev)

1. Start any TCP echo-like upstream on `127.0.0.1:9000` (for local simulation).
2. Run daemon in builtin mode:

```bash
cp examples/config.dev.json /tmp/mtprotor-config.json
./bin/mtprotor daemon --config /tmp/mtprotor-config.json
```

3. Manage secrets:

```bash
./bin/mtprotor secret add 00112233445566778899aabbccddeeff --label main --config /tmp/mtprotor-config.json
./bin/mtprotor secret list --config /tmp/mtprotor-config.json
./bin/mtprotor status --config /tmp/mtprotor-config.json
```

## Production Install (Ubuntu 24.04)

### One-command install from GitHub

```bash
curl -fsSL https://raw.githubusercontent.com/<org>/<repo>/main/scripts/install.sh | sudo bash -s -- --repo <org>/<repo>
```

Optional flags:

- `--ref <branch-or-tag>`
- `--skip-official` (skip installing official `mtproto-proxy` worker)

Installer performs:

- builds and installs `mtprotor`
- installs systemd unit
- creates `mtprotor` system user
- writes default config `/etc/mtprotor/config.json`
- installs worker wrapper `/usr/local/bin/mtproto-worker-wrapper.sh`
- installs official Telegram `mtproto-proxy` and proxy config files (unless `--skip-official`)

Detailed VPS rollout checklist: `docs/rollout.md`.

## Service Management

```bash
sudo systemctl status mtprotor --no-pager
sudo systemctl restart mtprotor
sudo journalctl -u mtprotor -f
```

## CLI

```bash
mtprotor status
mtprotor secret list
mtprotor secret add <secret_hex> --label main
mtprotor secret remove <secret_id>
mtprotor secret disable <secret_id>
mtprotor secret enable <secret_id>
```

Extra options for add:

- `--expires-at 2026-12-31T00:00:00Z`
- `--max-connections 100`
- `--disabled`

## Local Admin API

Unix socket: `/run/mtprotor/admin.sock`

Endpoints:

- `GET /v1/status`
- `GET /v1/secrets`
- `POST /v1/secrets`
- `DELETE /v1/secrets/{id}`
- `PATCH /v1/secrets/{id}/enable`
- `PATCH /v1/secrets/{id}/disable`

Example:

```bash
curl --unix-socket /run/mtprotor/admin.sock http://localhost/v1/secrets
```

Create secret:

```bash
curl --unix-socket /run/mtprotor/admin.sock \
  -H 'Content-Type: application/json' \
  -d '{"secret":"00112233445566778899aabbccddeeff","label":"main","enabled":true}' \
  http://localhost/v1/secrets
```

## Config

Main file: `/etc/mtprotor/config.json`

Use `examples/config.production.json` as baseline.

Important fields:

- `listen_addr` - public ingress for clients
- `admin_socket` - Unix socket for local management
- `state_file` - persistent secret state
- `worker.mode`:
  - `command` for official worker process
  - `builtin` for testing/dev

`command` mode uses placeholders in args:

- `{{port}}` - assigned local worker port
- `{{secret}}` - normalized secret
- `{{id}}` - secret ID

When using official worker wrapper (`/usr/local/bin/mtproto-worker-wrapper.sh`):

- set `MTPROXY_BASE_ARGS` in `/etc/default/mtprotor` with shared params like `--aes-pwd` and `--aes-key`
- do not include `-p/-H/-S` in `MTPROXY_BASE_ARGS` (wrapper injects them per secret worker)
- optional `MTPROXY_PLAIN_PORT_OFFSET` controls derived plain port (`-p`)
- example env file: `examples/mtprotor.env`

## Uninstall

```bash
sudo /usr/local/bin/mtprotor-uninstall
```

Purge config/state too:

```bash
sudo /usr/local/bin/mtprotor-uninstall --purge-data
```

## Testing

```bash
go test ./...
```

Quick operational smoke check on server:

```bash
sudo /usr/local/bin/mtprotor-smoke
# or from repo checkout:
./scripts/smoke-hot-reload.sh
```

Included tests:

- unit tests for secret normalization and handshake matching
- unit tests for state persistence
- integration tests for:
  - hot remove behavior (active connection survives)
  - restart + state restore

## CI/CD

Included GitHub Actions workflows:

- `.github/workflows/ci.yml` - format check, test, vet, build
- `.github/workflows/release.yml` - build Linux binaries on tag `v*` and publish release assets

## Operational Notes

- Secret remove/disable blocks new connections immediately.
- Existing connections are preserved by default (`drop_on_disable=false`).
- API returns masked secret values (full secret not exposed in list output).

## License

MIT (recommended for this project; add `LICENSE` file before publishing).
