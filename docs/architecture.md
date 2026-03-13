# mtprotor Architecture (MVP)

## Why Go
- Single binary deployment on Ubuntu 24.04.
- Fast network I/O and simple concurrency model.
- Strong standard library (`net`, `http`, `os/exec`, `slog`) with minimal external deps.

## Core Idea
`mtprotor` is a long-running runtime manager that accepts client connections and validates MTProxy secrets from handshake bytes against an in-memory secret store. Secrets are updated through local API/CLI **without restarting the manager process**.

For protocol compatibility, each enabled secret is served by a dedicated backend worker process (official MTProto proxy command template, configurable). Dispatcher routes each new connection to the worker matched by secret.
Worker ports are local-only (`127.0.0.1`) by default; clients only reach the main public listener.

## Runtime Flow
1. Client connects to public listener (`listen_addr`).
2. Dispatcher reads first 64 bytes (MTProxy obfuscated handshake preamble).
3. Runtime tests preamble against enabled secrets in memory.
4. If no match: reject connection.
5. If matched: connect to corresponding local worker, forward preamble + stream.
6. Active streams continue independently; secret updates only affect routing of **new** connections.

## Hot Reload Semantics
- `add secret`: runtime inserts secret, persists to disk, starts worker immediately.
- `disable secret`: runtime marks secret disabled, stops accepting new connections for it.
- `remove secret`: runtime removes from active map immediately; existing sessions stay until natural close (default behavior).
- No daemon restart is required for these operations.

## Modules
- `cmd/mtprotor`: CLI + daemon entrypoint.
- `internal/config`: JSON config loading/validation.
- `internal/model`: domain types (`SecretRecord`, runtime state).
- `internal/storage`: persistent state file (atomic JSON writes).
- `internal/handshake`: handshake parsing/matching against secrets.
- `internal/worker`: backend worker lifecycle (start/stop/status).
- `internal/runtime`: orchestrates secret store, connection routing, and worker manager.
- `internal/api`: local admin API over Unix socket.
- `internal/client`: CLI client for admin API.
- `internal/app`: command wiring.

## Control Plane
- Local HTTP API on Unix socket (`admin_socket`) for secure host-local management.
- CLI wraps API:
  - `mtprotor status`
  - `mtprotor secret add/remove/enable/disable/list`

## Persistence
- Secrets and metadata stored in local JSON (`state_file`) with atomic replace.
- On startup runtime loads state and restores enabled secrets.

## Deployment
- `systemd` unit runs daemon as dedicated user.
- `scripts/install.sh` installs binary, default config, service, directories.
- `scripts/uninstall.sh` removes service and installed files.

## Security Defaults
- API bound to Unix socket with strict file mode.
- Worker listeners bound to loopback by default.
- Secret values never returned in full by list APIs.
- Optional per-secret expiration and connection limits.
