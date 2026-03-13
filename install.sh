#!/usr/bin/env bash
set -euo pipefail

REPO_URL_DEFAULT="https://github.com/billionero1/mtprotor.git"
REPO_URL="${MTPROXY_REPO_URL:-$REPO_URL_DEFAULT}"
REPO_REF="${MTPROXY_REPO_REF:-main}"
INSTALL_DIR="${MTPROXY_INSTALL_DIR:-/opt/mtproxy-fork-src}"
SERVICE_NAME="mtproxy-fork"

CONF_DIR="/etc/mtproxy-fork"
DATA_DIR="/var/lib/mtproxy-fork"
ENV_FILE="/etc/default/mtproxy-fork"
UNIT_FILE="/etc/systemd/system/mtproxy-fork.service"
BIN_PATH="/usr/local/bin/mtproto-proxy-fork"
CTL_PATH="/usr/local/bin/proxyctl"

CLIENT_PORT="443"
STATS_PORT="8888"
ADMIN_SOCKET="$DATA_DIR/admin.sock"
STATE_FILE="$DATA_DIR/secrets.tsv"
CLEAN_OLD="yes"
REFRESH_TG_CONFIG="yes"

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "Run as root: curl -fsSL <repo>/install.sh | sudo bash" >&2
  exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
  echo "This installer supports Ubuntu/Debian (apt-get) only." >&2
  exit 1
fi

is_interactive=0
tty_fd=9
if [[ -t 1 ]]; then
  set +e
  exec {tty_fd}<>/dev/tty 2>/dev/null
  tty_open_rc=$?
  set -e
  if (( tty_open_rc == 0 )); then
    is_interactive=1
  fi
fi

rand_hex16() {
  if command -v openssl >/dev/null 2>&1; then
    openssl rand -hex 16
    return
  fi
  od -An -N16 -tx1 /dev/urandom | tr -d ' \n'
}

require_port() {
  local p="$1"
  [[ "$p" =~ ^[0-9]+$ ]] || return 1
  (( p >= 1 && p <= 65535 )) || return 1
  return 0
}

require_hex32() {
  [[ "$1" =~ ^[0-9a-fA-F]{32}$ ]]
}

prompt_default() {
  local __var="$1"
  local question="$2"
  local def="$3"
  local val=""
  if (( is_interactive )); then
    printf "%s [%s]: " "$question" "$def" >&"$tty_fd"
    IFS= read -r val <&"$tty_fd" || true
  fi
  if [[ -z "$val" ]]; then
    val="$def"
  fi
  printf -v "$__var" '%s' "$val"
}

prompt_yes_no() {
  local __var="$1"
  local question="$2"
  local def="$3"
  local hint="Y/n"
  local val=""
  if [[ "$def" == "no" ]]; then
    hint="y/N"
  fi
  if (( is_interactive )); then
    printf "%s [%s]: " "$question" "$hint" >&"$tty_fd"
    IFS= read -r val <&"$tty_fd" || true
  fi
  val="${val:-}"
  if [[ -z "$val" ]]; then
    val="$def"
  fi
  case "${val,,}" in
    y|yes) printf -v "$__var" '%s' "yes" ;;
    n|no) printf -v "$__var" '%s' "no" ;;
    *)
      printf -v "$__var" '%s' "$def"
      ;;
  esac
}

BOOTSTRAP_SECRET="$(rand_hex16)"
ADMIN_TOKEN="$(rand_hex16)$(rand_hex16)"

prompt_default CLIENT_PORT "Client port" "$CLIENT_PORT"
require_port "$CLIENT_PORT" || { echo "Invalid client port: $CLIENT_PORT" >&2; exit 1; }

prompt_default STATS_PORT "Local stats port" "$STATS_PORT"
require_port "$STATS_PORT" || { echo "Invalid stats port: $STATS_PORT" >&2; exit 1; }

prompt_default BOOTSTRAP_SECRET "Bootstrap secret (32 hex)" "$BOOTSTRAP_SECRET"
require_hex32 "$BOOTSTRAP_SECRET" || { echo "Invalid bootstrap secret (must be 32 hex chars)" >&2; exit 1; }

prompt_default ADMIN_TOKEN "Admin API token (hex/string)" "$ADMIN_TOKEN"
[[ -n "$ADMIN_TOKEN" ]] || { echo "Admin token cannot be empty" >&2; exit 1; }

prompt_yes_no CLEAN_OLD "Delete old mtproxy/mtprotor services and files first" "yes"
prompt_yes_no REFRESH_TG_CONFIG "Refresh Telegram proxy config files (proxy-secret/proxy-multi.conf)" "yes"

echo "[1/8] Installing build dependencies..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y --no-install-recommends \
  ca-certificates curl git make gcc libc6-dev libssl-dev zlib1g-dev

cleanup_old() {
  echo "[2/8] Removing old services/processes..."
  systemctl disable --now mtprotor.service MTProxy.service "$SERVICE_NAME" 2>/dev/null || true
  rm -f "$UNIT_FILE" /etc/systemd/system/mtprotor.service /etc/systemd/system/MTProxy.service
  pkill -f '/usr/local/bin/mtprotor|/usr/local/bin/mtproto-proxy-fork|/usr/local/bin/mtproto-proxy' 2>/dev/null || true
  rm -rf /etc/mtprotor /var/lib/mtprotor /run/mtprotor "$CONF_DIR" "$DATA_DIR"
  rm -f "$ENV_FILE"
  systemctl daemon-reload || true
}

if [[ "$CLEAN_OLD" == "yes" ]]; then
  cleanup_old
else
  echo "[2/8] Skipping old stack cleanup"
fi

echo "[3/8] Cloning/updating source from $REPO_URL ($REPO_REF)..."
if [[ -d "$INSTALL_DIR/.git" ]]; then
  git -C "$INSTALL_DIR" fetch --depth 1 origin "$REPO_REF"
  git -C "$INSTALL_DIR" checkout -q "$REPO_REF"
  git -C "$INSTALL_DIR" reset --hard "origin/$REPO_REF"
else
  rm -rf "$INSTALL_DIR"
  git clone --depth 1 --branch "$REPO_REF" "$REPO_URL" "$INSTALL_DIR"
fi

echo "[4/8] Building mtproto-proxy..."
make -C "$INSTALL_DIR" clean
make -C "$INSTALL_DIR" -j"$(nproc)"
install -m 0755 "$INSTALL_DIR/objs/bin/mtproto-proxy" "$BIN_PATH"

echo "[5/8] Preparing runtime user and directories..."
if ! id -u mtproxy >/dev/null 2>&1; then
  useradd --system --home-dir "$DATA_DIR" --create-home --shell /usr/sbin/nologin mtproxy \
    || useradd --system --home-dir "$DATA_DIR" --create-home --shell /sbin/nologin mtproxy
fi
install -d -m 0750 -o root -g mtproxy "$CONF_DIR"
install -d -m 0750 -o mtproxy -g mtproxy "$DATA_DIR"

download_to() {
  local url="$1"
  local dst="$2"
  local tmp
  tmp="$(mktemp)"
  curl -fsSL --retry 3 --retry-all-errors "$url" -o "$tmp"
  install -m 0640 -o root -g mtproxy "$tmp" "$dst"
  rm -f "$tmp"
}

echo "[6/8] Preparing Telegram config files..."
if [[ "$REFRESH_TG_CONFIG" == "yes" || ! -s "$CONF_DIR/proxy-secret" ]]; then
  download_to "https://core.telegram.org/getProxySecret" "$CONF_DIR/proxy-secret"
fi
if [[ "$REFRESH_TG_CONFIG" == "yes" || ! -s "$CONF_DIR/proxy-multi.conf" ]]; then
  download_to "https://core.telegram.org/getProxyConfig" "$CONF_DIR/proxy-multi.conf"
fi

echo "[7/8] Writing local config, state and service files..."
cat > "$ENV_FILE" <<ENV
CLIENT_PORT=$CLIENT_PORT
STATS_PORT=$STATS_PORT
BOOTSTRAP_SECRET=$BOOTSTRAP_SECRET
ADMIN_SOCKET=$ADMIN_SOCKET
STATE_FILE=$STATE_FILE
ADMIN_TOKEN=$ADMIN_TOKEN
ENV
chown root:mtproxy "$ENV_FILE"
chmod 0640 "$ENV_FILE"

if [[ ! -s "$STATE_FILE" ]]; then
  now="$(date +%s)"
  {
    echo "v1"
    printf "%s\t1\t0\t%s\t%s\tbootstrap\n" "$BOOTSTRAP_SECRET" "$now" "$now"
  } > "$STATE_FILE"
fi
chown mtproxy:mtproxy "$STATE_FILE"
chmod 0640 "$STATE_FILE"

install -m 0644 "$INSTALL_DIR/systemd/mtproxy-fork.service" "$UNIT_FILE"
install -m 0755 "$INSTALL_DIR/scripts/proxyctl" "$CTL_PATH"

echo "[8/8] Enabling and starting service..."
systemctl daemon-reload
systemctl enable --now "$SERVICE_NAME"
sleep 1
if ! systemctl is-active --quiet "$SERVICE_NAME"; then
  echo "Service failed to start. Last logs:" >&2
  journalctl -u "$SERVICE_NAME" --no-pager -n 80 >&2 || true
  exit 1
fi

if ! curl --silent --show-error --unix-socket "$ADMIN_SOCKET" -H "X-Admin-Token: $ADMIN_TOKEN" http://localhost/v1/status >/dev/null; then
  echo "Admin API check failed on $ADMIN_SOCKET" >&2
  exit 1
fi

SERVER_IP="$(curl -4 -fsSL --max-time 5 https://api.ipify.org || true)"
if [[ -z "$SERVER_IP" ]]; then
  SERVER_IP="$(hostname -I | awk '{print $1}')"
fi

echo
echo "Install complete"
echo "Service: systemctl status $SERVICE_NAME"
echo "Manage secrets: proxyctl status | proxyctl secret list"
echo "API socket: $ADMIN_SOCKET"
echo "API token: $ADMIN_TOKEN"
if [[ -n "$SERVER_IP" ]]; then
  echo "Bootstrap link (recommended for iOS paste): https://t.me/proxy?server=${SERVER_IP}&port=${CLIENT_PORT}&secret=dd${BOOTSTRAP_SECRET}"
  echo "Bootstrap deep-link: tg://proxy?server=${SERVER_IP}&port=${CLIENT_PORT}&secret=dd${BOOTSTRAP_SECRET}"
fi
