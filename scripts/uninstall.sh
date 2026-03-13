#!/usr/bin/env bash
set -euo pipefail

PURGE_DATA=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --purge-data)
      PURGE_DATA=1
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

systemctl disable --now mtprotor.service >/dev/null 2>&1 || true
rm -f /etc/systemd/system/mtprotor.service
systemctl daemon-reload

rm -f /usr/local/bin/mtprotor
rm -f /usr/local/bin/mtprotor-uninstall
rm -f /usr/local/bin/mtprotor-smoke
rm -f /usr/local/bin/mtproto-worker-wrapper.sh

if [[ "$PURGE_DATA" -eq 1 ]]; then
  rm -rf /etc/mtprotor /var/lib/mtprotor /run/mtprotor
  rm -f /etc/default/mtprotor
  userdel mtprotor >/dev/null 2>&1 || true
fi

echo "Uninstalled mtprotor."
if [[ "$PURGE_DATA" -eq 0 ]]; then
  echo "Config and state were preserved in /etc/mtprotor and /var/lib/mtprotor."
fi
