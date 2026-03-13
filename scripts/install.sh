#!/usr/bin/env bash
set -euo pipefail

REPO=""
REF="main"
SKIP_OFFICIAL=0

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
      shift
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run as root (use sudo)." >&2
  exit 1
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

if [[ ! -f /etc/mtprotor/config.json ]]; then
  install -m 0640 -o mtprotor -g mtprotor "$SRC_DIR/examples/config.production.json" /etc/mtprotor/config.json
fi

if [[ ! -f /etc/default/mtprotor ]]; then
  cat >/etc/default/mtprotor <<'ENVEOF'
# Base args passed to official mtproto-proxy for each secret worker.
# Do not include -p/-H/-S here: wrapper sets them per secret worker.
MTPROXY_BIN=/usr/local/bin/mtproto-proxy
MTPROXY_BASE_ARGS="--aes-pwd /etc/mtprotor/proxy-multi.conf --aes-key /etc/mtprotor/proxy-secret"
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
echo "Check status: systemctl status mtprotor --no-pager"
echo "Manage secrets: mtprotor secret add <secret_hex> --label main"
