#!/usr/bin/env bash
set -euo pipefail

if ! command -v mtprotor >/dev/null 2>&1; then
  echo "mtprotor binary not found in PATH" >&2
  exit 1
fi

gen_secret() {
  if command -v openssl >/dev/null 2>&1; then
    openssl rand -hex 16
    return
  fi
  if command -v xxd >/dev/null 2>&1; then
    head -c 16 /dev/urandom | xxd -p -c 32
    return
  fi
  echo "00112233445566778899aabbccddeeff"
}

SECRET="${1:-$(gen_secret)}"
LABEL="smoke-$(date +%s)"

echo "[1/5] status"
mtprotor status >/dev/null

echo "[2/5] add secret"
ADD_OUT="$(mtprotor secret add --label "$LABEL" "$SECRET")"
ID="$(printf '%s\n' "$ADD_OUT" | sed -n 's/.*"id": "\([^"]*\)".*/\1/p' | head -n1)"
if [[ -z "$ID" ]]; then
  echo "failed to parse secret id from add output" >&2
  echo "$ADD_OUT"
  exit 1
fi

echo "secret id: $ID"

echo "[3/5] disable secret"
mtprotor secret disable "$ID" >/dev/null

echo "[4/5] enable secret"
mtprotor secret enable "$ID" >/dev/null

echo "[5/5] remove secret"
mtprotor secret remove "$ID" >/dev/null

echo "smoke test passed"
