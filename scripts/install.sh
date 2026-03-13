#!/usr/bin/env bash
set -euo pipefail

REPO=""
REF="main"
SKIP_OFFICIAL=0
SKIP_OFFICIAL_EXPLICIT=0
NON_INTERACTIVE=0
FORCE_CONFIG=0

LISTEN_PORT=""
WORKER_PORT_START=""
WORKER_PORT_END=""
HAS_PORT_ARGS=0

usage() {
  cat <<USAGE
Usage: install.sh [options]

Options:
  --repo <owner/repo>        GitHub repository (required for curl install)
  --ref <branch-or-tag>      Git ref to install (default: main)
  --skip-official            Do not install official Telegram MTProxy binary
  --non-interactive          Disable prompts; use defaults/flags
  --listen-port <port>       Public listen port for mtprotor
  --worker-port-start <port> Worker local port range start (default 29000)
  --worker-port-end <port>   Worker local port range end (default 29999)
  --force-config             Always rewrite /etc/mtprotor/config.json
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo)
      REPO="${2:-}"
      shift 2
      ;;
    --ref)
      REF="${2:-}"
      shift 2
      ;;
    --skip-official)
      SKIP_OFFICIAL=1
      SKIP_OFFICIAL_EXPLICIT=1
      shift
      ;;
    --non-interactive)
      NON_INTERACTIVE=1
      shift
      ;;
    --listen-port)
      LISTEN_PORT="${2:-}"
      HAS_PORT_ARGS=1
      shift 2
      ;;
    --worker-port-start)
      WORKER_PORT_START="${2:-}"
      HAS_PORT_ARGS=1
      shift 2
      ;;
    --worker-port-end)
      WORKER_PORT_END="${2:-}"
      HAS_PORT_ARGS=1
      shift 2
      ;;
    --force-config)
      FORCE_CONFIG=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run as root (use sudo)." >&2
  exit 1
fi

INTERACTIVE=0
if [[ "$NON_INTERACTIVE" -eq 0 && -t 0 && -t 1 ]]; then
  INTERACTIVE=1
fi

need_cmd() {
  command -v "$1" >/dev/null 2>&1
}

install_pkgs() {
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -y
  apt-get install -y ca-certificates curl git build-essential golang-go
}

install_official_deps() {
  export DEBIAN_FRONTEND=noninteractive
  apt-get install -y make gcc libssl-dev zlib1g-dev
}

is_valid_port() {
  local p="$1"
  [[ "$p" =~ ^[0-9]+$ ]] && (( p >= 1 && p <= 65535 ))
}

prompt_with_default() {
  local prompt="$1"
  local default="$2"
  local value
  read -r -p "$prompt [$default]: " value
  if [[ -z "$value" ]]; then
    value="$default"
  fi
  printf '%s' "$value"
}

prompt_yes_no() {
  local prompt="$1"
  local default="$2" # y or n
  local value
  local suffix="[y/N]"
  if [[ "$default" == "y" ]]; then
    suffix="[Y/n]"
  fi
  read -r -p "$prompt $suffix: " value
  value="${value,,}"
  if [[ -z "$value" ]]; then
    value="$default"
  fi
  [[ "$value" == "y" || "$value" == "yes" ]]
}

if ! need_cmd go || ! need_cmd git; then
  install_pkgs
fi

TMP_ROOT="$(mktemp -d)"
cleanup() {
  rm -rf "$TMP_ROOT"
}
trap cleanup EXIT

SRC_DIR=""
if [[ -f "./go.mod" && -d "./cmd/mtprotor" ]]; then
  SRC_DIR="$(pwd)"
else
  if [[ -z "$REPO" ]]; then
    echo "When running via curl, pass --repo <owner/repo>." >&2
    exit 1
  fi
  git clone --depth 1 --branch "$REF" "https://github.com/${REPO}.git" "$TMP_ROOT/src"
  SRC_DIR="$TMP_ROOT/src"
fi

echo "Building mtprotor from $SRC_DIR"
(
  cd "$SRC_DIR"
  GOCACHE="$TMP_ROOT/gocache" go build -o "$TMP_ROOT/mtprotor" ./cmd/mtprotor
)

install -d -m 0755 /usr/local/bin
install -m 0755 "$TMP_ROOT/mtprotor" /usr/local/bin/mtprotor
install -m 0755 "$SRC_DIR/scripts/official-worker-wrapper.sh" /usr/local/bin/mtproto-worker-wrapper.sh
install -m 0755 "$SRC_DIR/scripts/uninstall.sh" /usr/local/bin/mtprotor-uninstall
install -m 0755 "$SRC_DIR/scripts/smoke-hot-reload.sh" /usr/local/bin/mtprotor-smoke

if ! id -u mtprotor >/dev/null 2>&1; then
  useradd --system --home /var/lib/mtprotor --shell /usr/sbin/nologin --create-home mtprotor
fi

install -d -m 0750 -o mtprotor -g mtprotor /etc/mtprotor
install -d -m 0750 -o mtprotor -g mtprotor /var/lib/mtprotor
install -d -m 0750 -o mtprotor -g mtprotor /run/mtprotor

# Config values (defaults + optional interactive override)
DEFAULT_LISTEN_PORT="443"
DEFAULT_WORKER_PORT_START="29000"
DEFAULT_WORKER_PORT_END="29999"

if [[ -f /etc/mtprotor/config.json ]]; then
  mapfile -t existing_vals < <(python3 - <<'PY'
import json
p='/etc/mtprotor/config.json'
try:
    with open(p) as f:
        c=json.load(f)
except Exception:
    print('443')
    print('29000')
    print('29999')
    raise SystemExit(0)
listen=str(c.get('listen_addr', ':443'))
port=listen.rsplit(':',1)[-1] if ':' in listen else '443'
ws=str(c.get('worker',{}).get('port_range_start',29000))
we=str(c.get('worker',{}).get('port_range_end',29999))
print(port)
print(ws)
print(we)
PY
)
  if [[ ${#existing_vals[@]} -ge 3 ]]; then
    DEFAULT_LISTEN_PORT="${existing_vals[0]}"
    DEFAULT_WORKER_PORT_START="${existing_vals[1]}"
    DEFAULT_WORKER_PORT_END="${existing_vals[2]}"
  fi
fi

if [[ -z "$LISTEN_PORT" ]]; then
  LISTEN_PORT="$DEFAULT_LISTEN_PORT"
fi
if [[ -z "$WORKER_PORT_START" ]]; then
  WORKER_PORT_START="$DEFAULT_WORKER_PORT_START"
fi
if [[ -z "$WORKER_PORT_END" ]]; then
  WORKER_PORT_END="$DEFAULT_WORKER_PORT_END"
fi

if [[ "$INTERACTIVE" -eq 1 ]]; then
  echo
  echo "mtprotor configuration"
  LISTEN_PORT="$(prompt_with_default 'Public listen port' "$LISTEN_PORT")"
  WORKER_PORT_START="$(prompt_with_default 'Worker local port range start' "$WORKER_PORT_START")"
  WORKER_PORT_END="$(prompt_with_default 'Worker local port range end' "$WORKER_PORT_END")"

  if [[ "$SKIP_OFFICIAL_EXPLICIT" -eq 0 ]]; then
    if prompt_yes_no 'Install official Telegram MTProxy binary' 'y'; then
      SKIP_OFFICIAL=0
    else
      SKIP_OFFICIAL=1
    fi
  fi
fi

if ! is_valid_port "$LISTEN_PORT"; then
  echo "Invalid --listen-port: $LISTEN_PORT" >&2
  exit 1
fi
if ! is_valid_port "$WORKER_PORT_START"; then
  echo "Invalid --worker-port-start: $WORKER_PORT_START" >&2
  exit 1
fi
if ! is_valid_port "$WORKER_PORT_END"; then
  echo "Invalid --worker-port-end: $WORKER_PORT_END" >&2
  exit 1
fi
if (( WORKER_PORT_END < WORKER_PORT_START )); then
  echo "worker port range end must be >= start" >&2
  exit 1
fi

WRITE_CONFIG=0
if [[ ! -f /etc/mtprotor/config.json ]]; then
  WRITE_CONFIG=1
elif [[ "$FORCE_CONFIG" -eq 1 ]]; then
  WRITE_CONFIG=1
elif [[ "$HAS_PORT_ARGS" -eq 1 ]]; then
  WRITE_CONFIG=1
elif [[ "$INTERACTIVE" -eq 1 ]]; then
  if prompt_yes_no 'Rewrite /etc/mtprotor/config.json with selected values' 'n'; then
    WRITE_CONFIG=1
  fi
fi

if [[ "$WRITE_CONFIG" -eq 1 ]]; then
  python3 - "$SRC_DIR/examples/config.production.json" /etc/mtprotor/config.json "$LISTEN_PORT" "$WORKER_PORT_START" "$WORKER_PORT_END" <<'PY'
import json
import sys
src, dst, listen_port, worker_start, worker_end = sys.argv[1:6]
with open(src) as f:
    cfg = json.load(f)
cfg['listen_addr'] = f':{int(listen_port)}'
worker = cfg.setdefault('worker', {})
worker['port_range_start'] = int(worker_start)
worker['port_range_end'] = int(worker_end)
with open(dst, 'w') as f:
    json.dump(cfg, f, indent=2)
    f.write('\n')
PY
  chmod 0640 /etc/mtprotor/config.json
  chown mtprotor:mtprotor /etc/mtprotor/config.json
fi

if [[ ! -f /etc/default/mtprotor ]]; then
  cat >/etc/default/mtprotor <<'ENVEOF'
# Base args passed to official mtproto-proxy for each secret worker.
# Do not include -p/-H/-S here: wrapper sets them per secret worker.
MTPROXY_BIN=/usr/local/bin/mtproto-proxy
MTPROXY_BASE_ARGS="--aes-pwd /etc/mtprotor/proxy-secret"
MTPROXY_CONFIG_FILE=/etc/mtprotor/proxy-multi.conf
MTPROXY_PLAIN_PORT_OFFSET=10000
ENVEOF
  chmod 0640 /etc/default/mtprotor
  chown root:mtprotor /etc/default/mtprotor
fi

if [[ "$SKIP_OFFICIAL" -eq 0 ]]; then
  echo "Installing official Telegram MTProxy binary"
  install_official_deps
  git clone --depth 1 https://github.com/TelegramMessenger/MTProxy.git "$TMP_ROOT/MTProxy"
  make -C "$TMP_ROOT/MTProxy"
  install -m 0755 "$TMP_ROOT/MTProxy/objs/bin/mtproto-proxy" /usr/local/bin/mtproto-proxy

  if [[ ! -f /etc/mtprotor/proxy-secret ]]; then
    curl -fsSL https://core.telegram.org/getProxySecret -o /etc/mtprotor/proxy-secret
    chmod 0640 /etc/mtprotor/proxy-secret
    chown root:mtprotor /etc/mtprotor/proxy-secret
  fi

  if [[ ! -f /etc/mtprotor/proxy-multi.conf ]]; then
    curl -fsSL https://core.telegram.org/getProxyConfig -o /etc/mtprotor/proxy-multi.conf
    chmod 0640 /etc/mtprotor/proxy-multi.conf
    chown root:mtprotor /etc/mtprotor/proxy-multi.conf
  fi
fi

install -d -m 0755 /etc/systemd/system
install -m 0644 "$SRC_DIR/systemd/mtprotor.service" /etc/systemd/system/mtprotor.service

systemctl daemon-reload
systemctl enable --now mtprotor.service

echo "Installed successfully."
echo "Listen port: $LISTEN_PORT"
echo "Worker range: $WORKER_PORT_START-$WORKER_PORT_END"
echo "Check status: systemctl status mtprotor --no-pager"
echo "Manage secrets: mtprotor secret add <secret_hex> --label main"
