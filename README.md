# MTProxy
Simple MT-Proto proxy

## Fork extension: hot secret reload (runtime)
This fork adds runtime user/secret management without process restart.

Added options:
- `--admin-socket <path>`: enable local Unix-socket admin API.
- `--secrets-state <path>`: persistent state file for secrets.
- `--admin-token <token>`: optional token for admin commands.

Notes:
- Hot-reload mode is currently single-process only (`--admin-socket` is not supported with `--slaves`), because each slave process has its own memory and secret store would desync.
- Existing active client connections are not dropped when secrets are added/removed.
- Secret state is persisted atomically and restored on next start.
- Admin API is available over Unix socket in HTTP/JSON format (plus legacy line protocol).

## One-command install (Ubuntu 24.04)
Interactive installer (asks port/secret/token and removes old stack first):
```bash
curl -fsSL https://raw.githubusercontent.com/billionero1/mtprotor/main/install.sh | sudo bash
```

In non-interactive mode it uses defaults and performs clean reinstall (`CLEAN_OLD=yes`).

Uninstall:
```bash
curl -fsSL https://raw.githubusercontent.com/billionero1/mtprotor/main/uninstall.sh | sudo bash
```

Optional uninstall flags:
- `--purge-data` removes `/etc/mtproxy-fork`, `/var/lib/mtproxy-fork`, `/etc/default/mtproxy-fork`.
- `--purge-source` removes `/opt/mtproxy-fork-src`.

After install:
- service name: `mtproxy-fork.service`
- binary: `/usr/local/bin/mtproto-proxy-fork`
- local CLI: `/usr/local/bin/proxyctl`

## Building
Install dependencies, you would need common set of tools for building from source, and development packages for `openssl` and `zlib`.

On Debian/Ubuntu:
```bash
apt install git curl build-essential libssl-dev zlib1g-dev
```
On CentOS/RHEL:
```bash
yum install openssl-devel zlib-devel
yum groupinstall "Development Tools"
```

Clone the repo:
```bash
git clone https://github.com/TelegramMessenger/MTProxy
cd MTProxy
```

To build, simply run `make`, the binary will be in `objs/bin/mtproto-proxy`:

```bash
make && cd objs/bin
```

If the build has failed, you should run `make clean` before building it again.

## Running
1. Obtain a secret, used to connect to telegram servers.
```bash
curl -s https://core.telegram.org/getProxySecret -o proxy-secret
```
2. Obtain current telegram configuration. It can change (occasionally), so we encourage you to update it once per day.
```bash
curl -s https://core.telegram.org/getProxyConfig -o proxy-multi.conf
```
3. Generate a secret to be used by users to connect to your proxy.
```bash
head -c 16 /dev/urandom | xxd -ps
```
4. Run `mtproto-proxy`:
```bash
./mtproto-proxy -u nobody -p 8888 -H 443 -S <secret> --aes-pwd proxy-secret proxy-multi.conf -M 1
```
... where:
- `nobody` is the username. `mtproto-proxy` calls `setuid()` to drop privileges.
- `443` is the port, used by clients to connect to the proxy.
- `8888` is the local port. You can use it to get statistics from `mtproto-proxy`. Like `wget localhost:8888/stats`. You can only get this stat via loopback.
- `<secret>` is the secret generated at step 3. Also you can set multiple secrets: `-S <secret1> -S <secret2>`.
- `proxy-secret` and `proxy-multi.conf` are obtained at steps 1 and 2.
- `1` is the number of workers. You can increase the number of workers, if you have a powerful server.

Also feel free to check out other options using `mtproto-proxy --help`.

### Running with hot reload
Example:
```bash
./mtproto-proxy \
  -u nobody \
  -p 8888 \
  -H 443 \
  -S <bootstrap-secret> \
  --aes-pwd proxy-secret \
  --admin-socket /run/mtproxy/admin.sock \
  --secrets-state /var/lib/mtproxy/secrets.tsv \
  proxy-multi.conf
```

If you use the installer, runtime values are in `/etc/default/mtproxy-fork` and the service file template is `systemd/mtproxy-fork.service`.

HTTP/JSON endpoints over Unix socket:
- `GET /v1/status`
- `GET /v1/secrets`
- `POST /v1/secrets`
- `DELETE /v1/secrets/{secret_hex}`
- `PATCH /v1/secrets/{secret_hex}/enable`
- `PATCH /v1/secrets/{secret_hex}/disable`

Accepted secret formats in API:
- plain hex: `<32 hex>`
- random-padding form: `dd<32 hex>`
- TLS-like form: `ee<32 hex>[optional hex suffix]`

The runtime stores canonical 16-byte secret internally; `dd`/`ee` are accepted on input for link compatibility.

`POST /v1/secrets` body example:
```json
{"secret":"11111111111111111111111111111111","label":"user42","enabled":true,"expires":0}
```

Example via Python Unix socket:
```bash
python3 - <<'PY'
import socket
sock="/run/mtproxy/admin.sock"
req = (
  "GET /v1/status HTTP/1.1\\r\\n"
  "Host: local\\r\\n"
  "Content-Length: 0\\r\\n\\r\\n"
).encode()
s=socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock)
s.sendall(req)
print(s.recv(65535).decode())
s.close()
PY
```

Legacy line protocol is also supported:
- `STATUS`
- `LIST`
- `ADD secret=<hex32> label=<label> expires=<unix_ts_or_0> enabled=<0|1>`
- `REMOVE secret=<hex32>`
- `ENABLE secret=<hex32>`
- `DISABLE secret=<hex32>`

If `--admin-token` is configured:
- HTTP: send header `X-Admin-Token: <token>`
- Line protocol: include `token=<token>` in command arguments.

Examples:
```bash
python3 - <<'PY'
import socket
sock="/run/mtproxy/admin.sock"
for cmd in [
  "STATUS",
  "ADD secret=11111111111111111111111111111111 label=user42 enabled=1 expires=0",
  "LIST",
  "DISABLE secret=11111111111111111111111111111111",
]:
  s=socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
  s.connect(sock)
  s.sendall((cmd+"\\n").encode())
  print(s.recv(65535).decode(), end="")
  s.close()
PY
```

CLI helper (installed as `proxyctl`):
```bash
proxyctl status
proxyctl secret list
proxyctl secret add 11111111111111111111111111111111 --label user42
proxyctl secret disable 11111111111111111111111111111111
proxyctl secret remove 11111111111111111111111111111111
```

5. Generate the link with following schema: `tg://proxy?server=SERVER_NAME&port=PORT&secret=SECRET` (or let the official bot generate it for you).
6. Register your proxy with [@MTProxybot](https://t.me/MTProxybot) on Telegram.
7. Set received tag with arguments: `-P <proxy tag>`
8. Enjoy.

## Random padding
Due to some ISPs detecting MTProxy by packet sizes, random padding is
added to packets if such mode is enabled.

It's only enabled for clients which request it.

Add `dd` prefix to secret (`cafe...babe` => `ddcafe...babe`) to enable
this mode on client side.

## Systemd example configuration
1. Create systemd service file (it's standard path for the most Linux distros, but you should check it before):
```bash
nano /etc/systemd/system/MTProxy.service
```
2. Edit this basic service (especially paths and params):
```bash
[Unit]
Description=MTProxy
After=network.target

[Service]
Type=simple
WorkingDirectory=/opt/MTProxy
ExecStart=/opt/MTProxy/mtproto-proxy -u nobody -p 8888 -H 443 -S <secret> -P <proxy tag> <other params>
Restart=on-failure

[Install]
WantedBy=multi-user.target
```
3. Reload daemons:
```bash
systemctl daemon-reload
```
4. Test fresh MTProxy service:
```bash
systemctl restart MTProxy.service
# Check status, it should be active
systemctl status MTProxy.service
```
5. Enable it, to autostart service after reboot:
```bash
systemctl enable MTProxy.service
```

## Docker image
Telegram is also providing [official Docker image](https://hub.docker.com/r/telegrammessenger/proxy/).
Note: the image is outdated.
