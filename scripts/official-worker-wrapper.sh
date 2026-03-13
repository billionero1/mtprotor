#!/usr/bin/env bash
set -euo pipefail

LISTEN_PORT=""
SECRET_HEX=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --listen)
      LISTEN_PORT="${2:-}"
      shift 2
      ;;
    --secret)
      SECRET_HEX="${2:-}"
      shift 2
      ;;
    *)
      echo "unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "$LISTEN_PORT" || -z "$SECRET_HEX" ]]; then
  echo "usage: $0 --listen <port> --secret <hex>" >&2
  exit 2
fi

if ! [[ "$LISTEN_PORT" =~ ^[0-9]+$ ]]; then
  echo "listen port must be numeric, got: $LISTEN_PORT" >&2
  exit 2
fi

PLAIN_OFFSET="${MTPROXY_PLAIN_PORT_OFFSET:-10000}"
if ! [[ "$PLAIN_OFFSET" =~ ^[0-9]+$ ]]; then
  echo "MTPROXY_PLAIN_PORT_OFFSET must be numeric, got: $PLAIN_OFFSET" >&2
  exit 2
fi
PLAIN_PORT="$((LISTEN_PORT + PLAIN_OFFSET))"
if (( PLAIN_PORT < 1 || PLAIN_PORT > 65535 )); then
  echo "computed plain port out of range: $PLAIN_PORT (listen=$LISTEN_PORT offset=$PLAIN_OFFSET)" >&2
  exit 2
fi

MTPROXY_BIN="${MTPROXY_BIN:-/usr/local/bin/mtproto-proxy}"
if [[ ! -x "$MTPROXY_BIN" ]]; then
  echo "mtproto binary not found or not executable: $MTPROXY_BIN" >&2
  exit 1
fi

declare -a BASE_ARGS=()
if [[ -n "${MTPROXY_BASE_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  BASE_ARGS=( ${MTPROXY_BASE_ARGS} )
fi

declare -a CMD=("$MTPROXY_BIN")
CMD+=("${BASE_ARGS[@]}")
CMD+=(-p "$PLAIN_PORT" -H "$LISTEN_PORT" -S "$SECRET_HEX")
if [[ -n "${MTPROXY_CONFIG_FILE:-}" ]]; then
  CMD+=("${MTPROXY_CONFIG_FILE}")
fi

exec "${CMD[@]}"
