#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "Usage: $0 <gateway_tailnet_host_or_ip> [gateway_port=18789] [listen_port=18789]" >&2
  echo "Example: $0 openclaw-gw.tailxyz.ts.net 18789 18789" >&2
  exit 1
fi

TARGET_HOST="$1"
TARGET_PORT="${2:-18789}"
LISTEN_PORT="${3:-18789}"

if ! command -v socat >/dev/null 2>&1; then
  echo "socat is required. Install it first (e.g. apt install socat)." >&2
  exit 1
fi

echo "[relay] listening on 0.0.0.0:${LISTEN_PORT} -> ${TARGET_HOST}:${TARGET_PORT}"
exec socat \
  TCP-LISTEN:"${LISTEN_PORT}",reuseaddr,fork,keepalive \
  TCP:"${TARGET_HOST}":"${TARGET_PORT}"
