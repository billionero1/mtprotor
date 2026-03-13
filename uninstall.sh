#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="mtproxy-fork"
UNIT_FILE="/etc/systemd/system/mtproxy-fork.service"
EXPIRE_SYNC_SERVICE_FILE="/etc/systemd/system/mtproxy-fork-expire-sync.service"
EXPIRE_SYNC_TIMER_FILE="/etc/systemd/system/mtproxy-fork-expire-sync.timer"
BIN_PATH="/usr/local/bin/mtproto-proxy-fork"
CTL_PATH="/usr/local/bin/proxyctl"
MENU_PATH="/usr/local/bin/mtproxymenu"
DOG_MENU_PATH="/usr/local/bin/dogmenu"
DOG_CTL_PATH="/usr/local/bin/dogctl"
DISPATCH_PATH="/usr/local/bin/proxybot-dispatch"
BOT_SETUP_PATH="/usr/local/bin/mtproxybot-setup"
CONF_DIR="/etc/mtproxy-fork"
DATA_DIR="/var/lib/mtproxy-fork"
ENV_FILE="/etc/default/mtproxy-fork"
SRC_DIR="${MTPROXY_INSTALL_DIR:-/opt/mtproxy-fork-src}"

PURGE_DATA=0
PURGE_SOURCE=0

for arg in "$@"; do
  case "$arg" in
    --purge-data) PURGE_DATA=1 ;;
    --purge-source) PURGE_SOURCE=1 ;;
    *)
      echo "Unknown option: $arg" >&2
      echo "Usage: $0 [--purge-data] [--purge-source]" >&2
      exit 1
      ;;
  esac
done

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "Run as root" >&2
  exit 1
fi

systemctl disable --now "$SERVICE_NAME" mtproxy-fork-expire-sync.timer mtproxy-fork-expire-sync.service 2>/dev/null || true
rm -f "$UNIT_FILE" "$EXPIRE_SYNC_SERVICE_FILE" "$EXPIRE_SYNC_TIMER_FILE"
systemctl daemon-reload

rm -f "$BIN_PATH" "$CTL_PATH" "$MENU_PATH" "$DOG_MENU_PATH" "$DOG_CTL_PATH" "$DISPATCH_PATH" "$BOT_SETUP_PATH"

if (( PURGE_DATA )); then
  rm -rf "$CONF_DIR" "$DATA_DIR" "$ENV_FILE" /etc/mtproxy-fork/bot-access.env
  if id -u mtproxy >/dev/null 2>&1; then
    userdel mtproxy 2>/dev/null || true
  fi
else
  echo "Keeping data/config. Use --purge-data to remove $CONF_DIR and $DATA_DIR"
fi

if (( PURGE_SOURCE )); then
  rm -rf "$SRC_DIR"
fi

echo "Uninstall complete"
