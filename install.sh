#!/usr/bin/env bash
set -euo pipefail

REPO_URL_DEFAULT="https://github.com/billionero1/mtprotor.git"
REPO_URL="${MTPROXY_REPO_URL:-$REPO_URL_DEFAULT}"
REPO_REF="${MTPROXY_REPO_REF:-main}"
INSTALL_DIR="${MTPROXY_INSTALL_DIR:-/opt/mtproxy-fork-src}"
SERVICE_NAME="mtproxy-fork"

CONF_DIR="/etc/mtproxy-fork"
INBOUNDS_DIR="$CONF_DIR/inbounds.d"
DATA_DIR="/var/lib/mtproxy-fork"
ENV_FILE="/etc/default/mtproxy-fork"
UNIT_FILE="/etc/systemd/system/mtproxy-fork.service"
UNIT_TEMPLATE_FILE="/etc/systemd/system/mtproxy-fork@.service"
EXPIRE_SYNC_SERVICE_FILE="/etc/systemd/system/mtproxy-fork-expire-sync.service"
EXPIRE_SYNC_TIMER_FILE="/etc/systemd/system/mtproxy-fork-expire-sync.timer"
EXPIRE_SYNC_TEMPLATE_SERVICE_FILE="/etc/systemd/system/mtproxy-fork-expire-sync@.service"
EXPIRE_SYNC_TEMPLATE_TIMER_FILE="/etc/systemd/system/mtproxy-fork-expire-sync@.timer"
BIN_PATH="/usr/local/bin/mtproto-proxy-fork"
RUNNER_PATH="/usr/local/bin/mtproxy-fork-run"
CTL_PATH="/usr/local/bin/proxyctl"
MENU_PATH="/usr/local/bin/mtproxymenu"
DOG_MENU_PATH="/usr/local/bin/dogmenu"
DOG_CTL_PATH="/usr/local/bin/dogctl"
DISPATCH_PATH="/usr/local/bin/proxybot-dispatch"
BOT_SETUP_PATH="/usr/local/bin/mtproxybot-setup"
BOT_HTTPD_PATH="/usr/local/bin/proxybot-httpd"
BOT_HTTP_SETUP_PATH="/usr/local/bin/mtproxybot-http-setup"
BOT_HTTP_UNIT_FILE="/etc/systemd/system/mtproxy-fork-bot-api.service"
ADMIN_TOKEN_FILE="$CONF_DIR/admin.token"

CLIENT_PORT="443"
STATS_PORT="8888"
PUBLIC_HOST=""
SECRET_PREFIX="dd"
TLS_DOMAIN=""
ADMIN_SOCKET="$DATA_DIR/admin.sock"
STATE_FILE="$DATA_DIR/secrets.tsv"
CLEAN_OLD="yes"
REFRESH_TG_CONFIG="yes"
BOT_SSH_SETUP="yes"
BOT_SSH_USER="mtproxybot"
BOT_SSH_ALLOW_FROM=""
BOT_SSH_PASSWORD=""
BOT_HTTP_SETUP="yes"
BOT_HTTP_LISTEN="0.0.0.0"
BOT_HTTP_PORT="9443"
BOT_HTTP_USER="botapi"
BOT_HTTP_ALLOW_FROM=""
BOT_HTTP_PASSWORD=""
BOT_HTTP_API_KEY=""
BOT_HTTP_BASE_PATH=""
INBOUND_UID=""
INBOUND_SEQ_FILE="$DATA_DIR/inbound-id.seq"

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "Run as root: curl -fsSL <repo>/install.sh | sudo bash" >&2
  exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
  echo "This installer supports Ubuntu/Debian (apt-get) only." >&2
  exit 1
fi

print_logo() {
  cat <<'LOGO'

██████╗ ██╗   ██╗██╗     ██╗     ██████╗  ██████╗  ██████╗
██╔══██╗██║   ██║██║     ██║     ██╔══██╗██╔═══██╗██╔════╝
██████╔╝██║   ██║██║     ██║     ██║  ██║██║   ██║██║  ███╗
██╔══██╗██║   ██║██║     ██║     ██║  ██║██║   ██║██║   ██║
██████╔╝╚██████╔╝███████╗███████╗██████╔╝╚██████╔╝╚██████╔╝
╚═════╝  ╚═════╝ ╚══════╝╚══════╝╚═════╝  ╚═════╝  ╚═════╝

MTProxy Fork Installer (Hot Secret Reload)
LOGO
}

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

say() {
  echo "$*"
}

warn() {
  echo "[WARN] $*" >&2
}

die() {
  echo "[ERROR] $*" >&2
  exit 1
}

rand_hex16() {
  if command -v openssl >/dev/null 2>&1; then
    openssl rand -hex 16
    return
  fi
  od -An -N16 -tx1 /dev/urandom | tr -d ' \n'
}

rand_password() {
  if command -v openssl >/dev/null 2>&1; then
    openssl rand -base64 24 | tr -dc 'A-Za-z0-9' | head -c 24
    return
  fi
  od -An -N16 -tx1 /dev/urandom | tr -d ' \n'
}

rand_bootstrap_secret() {
  local s
  while true; do
    s="$(rand_hex16)"
    case "${s:0:2}" in
      dd|ee) continue ;;
      *) echo "$s"; return 0 ;;
    esac
  done
}

rand_base_path() {
  local r
  if command -v openssl >/dev/null 2>&1; then
    r="$(openssl rand -hex 8)"
  else
    r="$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')"
  fi
  echo "/api-$r"
}

detect_public_host() {
  local ip
  ip="$(curl -4 -fsSL --max-time 5 https://api.ipify.org || true)"
  if [[ -z "$ip" ]]; then
    ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
  fi
  echo "$ip"
}

detect_ssh_client_ip() {
  local ip=""
  if [[ -n "${SSH_CONNECTION:-}" ]]; then
    ip="$(awk '{print $1}' <<<"$SSH_CONNECTION")"
  elif [[ -n "${SSH_CLIENT:-}" ]]; then
    ip="$(awk '{print $1}' <<<"$SSH_CLIENT")"
  fi
  echo "$ip"
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

is_valid_domain_name() {
  local d="${1,,}"
  [[ -n "$d" ]] || return 1
  [[ "$d" == *.* ]] || return 1
  [[ "$d" != .* && "$d" != *. && "$d" != *..* ]] || return 1
  [[ "$d" =~ ^[a-z0-9.-]+$ ]] || return 1
  local p
  IFS='.' read -r -a parts <<<"$d"
  for p in "${parts[@]}"; do
    [[ -n "$p" && ${#p} -le 63 ]] || return 1
    [[ "$p" =~ ^[a-z0-9]([a-z0-9-]*[a-z0-9])?$ ]] || return 1
  done
  return 0
}

domain_to_hex() {
  local d="${1,,}"
  printf '%s' "$d" | od -An -tx1 -v | tr -d ' \n'
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
    *) printf -v "$__var" '%s' "$def" ;;
  esac
}

prompt_port() {
  local __var="$1"
  local question="$2"
  local def="$3"
  local input=""
  while true; do
    prompt_default input "$question" "$def"
    if require_port "$input"; then
      printf -v "$__var" '%s' "$input"
      return 0
    fi
    if (( ! is_interactive )); then
      die "Invalid non-interactive value for $question: $input"
    fi
    warn "Invalid port: $input"
  done
}

prompt_hex32() {
  local __var="$1"
  local question="$2"
  local def="$3"
  local input=""
  while true; do
    prompt_default input "$question" "$def"
    if require_hex32 "$input"; then
      printf -v "$__var" '%s' "${input,,}"
      return 0
    fi
    if (( ! is_interactive )); then
      die "Invalid non-interactive value for $question"
    fi
    warn "Value must be exactly 32 hex chars"
  done
}

prompt_nonempty() {
  local __var="$1"
  local question="$2"
  local def="$3"
  local input=""
  while true; do
    prompt_default input "$question" "$def"
    if [[ -n "$input" ]]; then
      printf -v "$__var" '%s' "$input"
      return 0
    fi
    if (( ! is_interactive )); then
      die "Invalid non-interactive empty value for $question"
    fi
    warn "Value cannot be empty"
  done
}

print_logo

BOOTSTRAP_SECRET="$(rand_bootstrap_secret)"
ADMIN_TOKEN="$(rand_hex16)$(rand_hex16)"
INBOUND_UID="$(rand_hex16)"
PUBLIC_HOST="$(detect_public_host)"
BOT_SSH_ALLOW_FROM="$(detect_ssh_client_ip)"
if [[ -z "$BOT_SSH_ALLOW_FROM" ]]; then
  BOT_SSH_ALLOW_FROM="127.0.0.1"
fi
BOT_HTTP_ALLOW_FROM="$BOT_SSH_ALLOW_FROM"
BOT_HTTP_PASSWORD="$(rand_password)"
BOT_HTTP_API_KEY="$(rand_hex16)$(rand_hex16)"
BOT_HTTP_BASE_PATH="$(rand_base_path)"

say "Installer mode: $([[ $is_interactive -eq 1 ]] && echo interactive || echo non-interactive)"

prompt_port CLIENT_PORT "Client port (Telegram users connect here)" "$CLIENT_PORT"
prompt_port STATS_PORT "Local stats port" "$STATS_PORT"
prompt_default PUBLIC_HOST "Public host/IP for generated links" "${PUBLIC_HOST:-0.0.0.0}"
prompt_hex32 BOOTSTRAP_SECRET "Bootstrap secret (32 hex)" "$BOOTSTRAP_SECRET"
prompt_nonempty ADMIN_TOKEN "Admin API token" "$ADMIN_TOKEN"
prompt_default SECRET_PREFIX "Link secret prefix (plain/dd/ee)" "$SECRET_PREFIX"
case "${SECRET_PREFIX,,}" in
  plain|"")
    SECRET_PREFIX="plain"
    ;;
  dd)
    SECRET_PREFIX="dd"
    ;;
  ee)
    SECRET_PREFIX="ee"
    ;;
  *)
    die "secret prefix must be plain, dd or ee"
    ;;
esac
prompt_default TLS_DOMAIN "Optional TLS cover domain (empty=off, example: www.google.com)" "$TLS_DOMAIN"
TLS_DOMAIN="${TLS_DOMAIN,,}"
if [[ -n "$TLS_DOMAIN" ]]; then
  is_valid_domain_name "$TLS_DOMAIN" || die "invalid TLS cover domain: $TLS_DOMAIN"
  if [[ "$SECRET_PREFIX" != "ee" ]]; then
    warn "TLS cover domain requires ee mode; switching secret prefix to ee"
    SECRET_PREFIX="ee"
  fi
fi
prompt_yes_no CLEAN_OLD "Delete old mtproxy/mtprotor installations first" "yes"
prompt_yes_no REFRESH_TG_CONFIG "Refresh Telegram proxy-secret/proxy-multi.conf" "yes"
prompt_yes_no BOT_SSH_SETUP "Configure SSH bot credentials (login/password) now" "yes"
if [[ "$BOT_SSH_SETUP" == "yes" ]]; then
  while true; do
    prompt_nonempty BOT_SSH_USER "Bot SSH username" "$BOT_SSH_USER"
    if [[ "$BOT_SSH_USER" =~ ^[a-z_][a-z0-9_-]{1,30}$ ]]; then
      break
    fi
    suggestion="$(printf '%s' "$BOT_SSH_USER" \
      | tr '[:upper:]' '[:lower:]' \
      | sed -E 's/[^a-z0-9_-]+/-/g; s/^-+//; s/-+$//; s/-+/-/g')"
    if [[ -z "$suggestion" || ! "$suggestion" =~ ^[a-z_] ]]; then
      suggestion="mtproxybot"
    fi
    suggestion="${suggestion:0:31}"
    if (( is_interactive )); then
      warn "Invalid bot SSH username: '$BOT_SSH_USER'"
      warn "Allowed pattern: ^[a-z_][a-z0-9_-]{1,30}$ (lowercase only)"
      prompt_yes_no use_suggest "Use normalized username '$suggestion'" "yes"
      if [[ "$use_suggest" == "yes" ]]; then
        BOT_SSH_USER="$suggestion"
        break
      fi
      warn "Please enter username again"
      continue
    fi
    die "Invalid bot SSH username '$BOT_SSH_USER'. Allowed pattern: ^[a-z_][a-z0-9_-]{1,30}$. Suggested: $suggestion"
  done
  prompt_nonempty BOT_SSH_ALLOW_FROM "Allowed bot source IP/CIDR (required)" "$BOT_SSH_ALLOW_FROM"
  if [[ "${BOT_SSH_ALLOW_FROM,,}" == "any" ]]; then
    die "allow-from cannot be 'any' in installer secure mode"
  fi
  BOT_SSH_PASSWORD="$(rand_password)"
fi
prompt_yes_no BOT_HTTP_SETUP "Configure Bot HTTP API (host/base_path/login/password) now" "yes"
if [[ "$BOT_HTTP_SETUP" == "yes" ]]; then
  prompt_port BOT_HTTP_PORT "Bot HTTP API port" "$BOT_HTTP_PORT"
  prompt_nonempty BOT_HTTP_USER "Bot HTTP API username" "$BOT_HTTP_USER"
  if [[ ! "$BOT_HTTP_USER" =~ ^[a-z_][a-z0-9_-]{1,30}$ ]]; then
    die "Invalid Bot HTTP username '$BOT_HTTP_USER'. Allowed pattern: ^[a-z_][a-z0-9_-]{1,30}$"
  fi
  prompt_nonempty BOT_HTTP_ALLOW_FROM "Allowed bot source IP/CIDR for HTTP API (required)" "$BOT_HTTP_ALLOW_FROM"
  if [[ "${BOT_HTTP_ALLOW_FROM,,}" == "any" ]]; then
    die "Bot HTTP allow-from cannot be 'any' in installer secure mode"
  fi
  prompt_default BOT_HTTP_BASE_PATH "Bot HTTP base path (example: /api-xxxx)" "$BOT_HTTP_BASE_PATH"
  if [[ "$BOT_HTTP_BASE_PATH" != /* ]] || [[ "$BOT_HTTP_BASE_PATH" == */ ]]; then
    die "Bot HTTP base path must start with '/' and must not end with '/'"
  fi
fi

say "[1/9] Installing build dependencies..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y --no-install-recommends \
  ca-certificates curl git make gcc libc6-dev libssl-dev zlib1g-dev python3

cleanup_old() {
  say "[2/9] Removing old services/processes/files..."
  systemctl disable --now mtprotor.service MTProxy.service "$SERVICE_NAME" mtproxy-fork-expire-sync.timer mtproxy-fork-expire-sync.service 2>/dev/null || true
  systemctl disable --now 'mtproxy-fork@*.service' 'mtproxy-fork-expire-sync@*.timer' 'mtproxy-fork-expire-sync@*.service' 2>/dev/null || true
  rm -f "$UNIT_FILE" "$UNIT_TEMPLATE_FILE" "$EXPIRE_SYNC_SERVICE_FILE" "$EXPIRE_SYNC_TIMER_FILE" "$EXPIRE_SYNC_TEMPLATE_SERVICE_FILE" "$EXPIRE_SYNC_TEMPLATE_TIMER_FILE" /etc/systemd/system/mtprotor.service /etc/systemd/system/MTProxy.service
  pkill -f '/usr/local/bin/mtprotor|/usr/local/bin/mtproto-proxy-fork|/usr/local/bin/mtproto-proxy' 2>/dev/null || true
  rm -rf /etc/mtprotor /var/lib/mtprotor /run/mtprotor "$CONF_DIR" "$DATA_DIR"
  systemctl disable --now mtproxy-fork-bot-api.service 2>/dev/null || true
  rm -f "$ENV_FILE" "$BIN_PATH" "$RUNNER_PATH" "$CTL_PATH" "$MENU_PATH" "$DOG_MENU_PATH" "$DOG_CTL_PATH" "$DISPATCH_PATH" "$BOT_SETUP_PATH" "$BOT_HTTPD_PATH" "$BOT_HTTP_SETUP_PATH" "$BOT_HTTP_UNIT_FILE"
  systemctl daemon-reload || true
}

if [[ "$CLEAN_OLD" == "yes" ]]; then
  cleanup_old
else
  say "[2/9] Skipping cleanup by user choice"
fi

say "[3/9] Cloning/updating source from $REPO_URL ($REPO_REF)..."
if [[ -d "$INSTALL_DIR/.git" ]]; then
  git -C "$INSTALL_DIR" fetch --depth 1 origin "$REPO_REF"
  git -C "$INSTALL_DIR" checkout -q "$REPO_REF"
  git -C "$INSTALL_DIR" reset --hard "origin/$REPO_REF"
else
  rm -rf "$INSTALL_DIR"
  git clone --depth 1 --branch "$REPO_REF" "$REPO_URL" "$INSTALL_DIR"
fi

say "[4/9] Building mtproto-proxy..."
make -C "$INSTALL_DIR" clean
make -C "$INSTALL_DIR" -j"$(nproc)"
install -m 0755 "$INSTALL_DIR/objs/bin/mtproto-proxy" "$BIN_PATH"

say "[5/9] Preparing runtime user and directories..."
if ! id -u mtproxy >/dev/null 2>&1; then
  useradd --system --home-dir "$DATA_DIR" --create-home --shell /usr/sbin/nologin mtproxy \
    || useradd --system --home-dir "$DATA_DIR" --create-home --shell /sbin/nologin mtproxy
fi
install -d -m 0750 -o root -g mtproxy "$CONF_DIR"
install -d -m 0750 -o root -g mtproxy "$INBOUNDS_DIR"
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

say "[6/9] Preparing Telegram config files..."
if [[ "$REFRESH_TG_CONFIG" == "yes" || ! -s "$CONF_DIR/proxy-secret" ]]; then
  download_to "https://core.telegram.org/getProxySecret" "$CONF_DIR/proxy-secret"
fi
if [[ "$REFRESH_TG_CONFIG" == "yes" || ! -s "$CONF_DIR/proxy-multi.conf" ]]; then
  download_to "https://core.telegram.org/getProxyConfig" "$CONF_DIR/proxy-multi.conf"
fi

say "[7/9] Writing config/state/service files..."
cat > "$ENV_FILE" <<ENV
CLIENT_PORT=$CLIENT_PORT
STATS_PORT=$STATS_PORT
PUBLIC_HOST=$PUBLIC_HOST
BOOTSTRAP_SECRET=${BOOTSTRAP_SECRET,,}
SECRET_PREFIX=$SECRET_PREFIX
TLS_DOMAIN=$TLS_DOMAIN
ADMIN_SOCKET=$ADMIN_SOCKET
STATE_FILE=$STATE_FILE
ADMIN_TOKEN_FILE=$ADMIN_TOKEN_FILE
ADMIN_TOKEN=$ADMIN_TOKEN
SOURCE_DIR=$INSTALL_DIR
SOURCE_REF=$REPO_REF
INBOUND_UID=$INBOUND_UID
ENV
chown root:mtproxy "$ENV_FILE"
chmod 0640 "$ENV_FILE"

tmp_admin_token="$(mktemp)"
printf '%s\n' "$ADMIN_TOKEN" > "$tmp_admin_token"
install -m 0640 -o root -g mtproxy "$tmp_admin_token" "$ADMIN_TOKEN_FILE"
rm -f "$tmp_admin_token"

now="$(date +%s)"
{
  echo "v1"
  printf "%s\t1\t0\t%s\t%s\tbootstrap\n" "${BOOTSTRAP_SECRET,,}" "$now" "$now"
} > "$STATE_FILE"
chown mtproxy:mtproxy "$STATE_FILE"
chmod 0640 "$STATE_FILE"

printf '1\n' > "$INBOUND_SEQ_FILE"
chown root:mtproxy "$INBOUND_SEQ_FILE"
chmod 0640 "$INBOUND_SEQ_FILE"

install -m 0644 "$INSTALL_DIR/systemd/mtproxy-fork.service" "$UNIT_FILE"
install -m 0644 "$INSTALL_DIR/systemd/mtproxy-fork@.service" "$UNIT_TEMPLATE_FILE"
install -m 0644 "$INSTALL_DIR/systemd/mtproxy-fork-expire-sync.service" "$EXPIRE_SYNC_SERVICE_FILE"
install -m 0644 "$INSTALL_DIR/systemd/mtproxy-fork-expire-sync.timer" "$EXPIRE_SYNC_TIMER_FILE"
install -m 0644 "$INSTALL_DIR/systemd/mtproxy-fork-expire-sync@.service" "$EXPIRE_SYNC_TEMPLATE_SERVICE_FILE"
install -m 0644 "$INSTALL_DIR/systemd/mtproxy-fork-expire-sync@.timer" "$EXPIRE_SYNC_TEMPLATE_TIMER_FILE"
install -m 0755 "$INSTALL_DIR/scripts/mtproxy-fork-run" "$RUNNER_PATH"
install -m 0755 "$INSTALL_DIR/scripts/proxyctl" "$CTL_PATH"
install -m 0755 "$INSTALL_DIR/scripts/dogctl" "$DOG_CTL_PATH"
install -m 0755 "$INSTALL_DIR/scripts/dogmenu" "$DOG_MENU_PATH"
install -m 0755 "$INSTALL_DIR/scripts/proxybot-dispatch" "$DISPATCH_PATH"
install -m 0755 "$INSTALL_DIR/scripts/mtproxybot-setup" "$BOT_SETUP_PATH"
install -m 0755 "$INSTALL_DIR/scripts/proxybot-httpd" "$BOT_HTTPD_PATH"
install -m 0755 "$INSTALL_DIR/scripts/mtproxybot-http-setup" "$BOT_HTTP_SETUP_PATH"
install -m 0644 "$INSTALL_DIR/systemd/mtproxy-fork-bot-api.service" "$BOT_HTTP_UNIT_FILE"
cat > "$MENU_PATH" <<'MENU'
#!/usr/bin/env bash
exec /usr/local/bin/dogmenu "$@"
MENU
chmod 0755 "$MENU_PATH"

BOT_SETUP_OUTPUT=""
if [[ "$BOT_SSH_SETUP" == "yes" ]]; then
  say "[7.5/9] Configuring bot SSH profile..."
  bot_cmd=("$BOT_SETUP_PATH" --user "$BOT_SSH_USER" --password "$BOT_SSH_PASSWORD")
  bot_cmd+=(--allow-from "$BOT_SSH_ALLOW_FROM")
  if ! BOT_SETUP_OUTPUT="$("${bot_cmd[@]}")"; then
    die "Failed to configure bot SSH profile"
  fi
  if [[ "$BOT_SETUP_OUTPUT" != *'"ok":true'* ]]; then
    die "Bot SSH profile setup returned error: $BOT_SETUP_OUTPUT"
  fi
fi

say "[8/9] Enabling and starting service..."
systemctl daemon-reload
systemctl enable --now "$SERVICE_NAME"
systemctl enable --now mtproxy-fork-expire-sync.timer
BOT_HTTP_SETUP_OUTPUT=""
if [[ "$BOT_HTTP_SETUP" == "yes" ]]; then
  say "[8.1/9] Configuring bot HTTP API profile..."
  http_cmd=("$BOT_HTTP_SETUP_PATH" --listen "$BOT_HTTP_LISTEN" --port "$BOT_HTTP_PORT" --user "$BOT_HTTP_USER" --password "$BOT_HTTP_PASSWORD" --api-key "$BOT_HTTP_API_KEY" --base-path "$BOT_HTTP_BASE_PATH" --allow-from "$BOT_HTTP_ALLOW_FROM")
  if ! BOT_HTTP_SETUP_OUTPUT="$("${http_cmd[@]}")"; then
    die "Failed to configure bot HTTP API profile"
  fi
  if [[ "$BOT_HTTP_SETUP_OUTPUT" != *'"ok":true'* ]]; then
    die "Bot HTTP API setup returned error: $BOT_HTTP_SETUP_OUTPUT"
  fi
fi
sleep 1

say "[9/9] Post-checks..."
if ! systemctl is-active --quiet "$SERVICE_NAME"; then
  die "Service failed to start. Logs: journalctl -u $SERVICE_NAME -n 80"
fi

if [[ ! -S "$ADMIN_SOCKET" ]]; then
  die "Admin socket not found: $ADMIN_SOCKET"
fi

if ! curl --silent --show-error --unix-socket "$ADMIN_SOCKET" -H "X-Admin-Token: $ADMIN_TOKEN" http://localhost/v1/status >/dev/null; then
  die "Admin API check failed on $ADMIN_SOCKET"
fi

if ! ss -ltn | awk '{print $4}' | grep -qE "(^|:)${CLIENT_PORT}$"; then
  die "Client port $CLIENT_PORT is not listening"
fi

if [[ -n "$SECRET_PREFIX" ]]; then
  case "${SECRET_PREFIX,,}" in
    plain)
      LINK_SECRET="${BOOTSTRAP_SECRET,,}"
      ;;
    dd)
      LINK_SECRET="${SECRET_PREFIX,,}${BOOTSTRAP_SECRET,,}"
      ;;
    ee)
      if [[ -n "$TLS_DOMAIN" ]]; then
        LINK_SECRET="ee${BOOTSTRAP_SECRET,,}$(domain_to_hex "$TLS_DOMAIN")"
      else
        LINK_SECRET="ee${BOOTSTRAP_SECRET,,}"
      fi
      ;;
    *)
      LINK_SECRET="${BOOTSTRAP_SECRET,,}"
      ;;
  esac
else
  LINK_SECRET="${BOOTSTRAP_SECRET,,}"
fi

say
say "Install complete"
say "Runtime: $SERVICE_NAME (systemd unit)"
say "Console: dogmenu (compat: mtproxymenu)"
say "CLI: dogctl runtime status (compat: proxyctl runtime status)"
say "Bot setup: mtproxybot-setup --show | mtproxybot-setup --regen-password --user mtproxybot"
say "API socket: $ADMIN_SOCKET"
say "API token file: $ADMIN_TOKEN_FILE"
say "API token: $ADMIN_TOKEN"
if [[ -n "$TLS_DOMAIN" ]]; then
  say "TLS cover domain: $TLS_DOMAIN"
fi
if [[ "$BOT_SSH_SETUP" == "yes" ]]; then
  say "Bot SSH credentials:"
  say "  host=${PUBLIC_HOST:-$(detect_public_host)}"
  say "  port=22"
  say "  username=$BOT_SSH_USER"
  say "  password=$BOT_SSH_PASSWORD"
  say "  allow_from=$BOT_SSH_ALLOW_FROM"
  say "  check/rotate: proxyctl bot ssh show | proxyctl bot ssh rotate-password --user $BOT_SSH_USER"
fi
if [[ "$BOT_HTTP_SETUP" == "yes" ]]; then
  say "Bot HTTP API credentials:"
  say "  scheme=http"
  say "  host=${PUBLIC_HOST:-$(detect_public_host)}"
  say "  port=$BOT_HTTP_PORT"
  say "  base_path=$BOT_HTTP_BASE_PATH"
  say "  username=$BOT_HTTP_USER"
  say "  password=$BOT_HTTP_PASSWORD"
  say "  api_key=${BOT_HTTP_API_KEY,,}"
  say "  allow_from=$BOT_HTTP_ALLOW_FROM"
  say "  check/rotate: proxyctl bot api show | proxyctl bot api rotate-password | proxyctl bot api rotate-key"
fi
if [[ -n "$PUBLIC_HOST" ]]; then
  say "Bootstrap link (iOS paste): https://t.me/proxy?server=${PUBLIC_HOST}&port=${CLIENT_PORT}&secret=${LINK_SECRET}"
  say "Bootstrap deep-link: tg://proxy?server=${PUBLIC_HOST}&port=${CLIENT_PORT}&secret=${LINK_SECRET}"
fi
