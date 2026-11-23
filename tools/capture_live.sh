#!/usr/bin/env bash
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

set -euo pipefail
umask 077

if [[ $# -ne 2 ]]; then
  echo "usage: capture_live.sh OUTPUT_DIR SECONDS" >&2
  exit 2
fi

capture_root=$(realpath -m -- "$(dirname "$0")/../captures")
capture_dir=$(realpath -m -- "$1")
capture_seconds=$2
if [[ $capture_dir != "$capture_root/"* ]]; then
  echo "capture directory must be under the private research captures directory" >&2
  exit 2
fi
if ! [[ $capture_seconds =~ ^[0-9]+$ ]] || (( capture_seconds < 1 || capture_seconds > 300 )); then
  echo "capture duration must be 1..300 seconds" >&2
  exit 2
fi
mkdir -p "$capture_dir"
btmon -w "$capture_dir/bluetooth.snoop" >"$capture_dir/bluetooth.log" 2>&1 &
btmon_pid=$!
cleanup() {
  kill "$btmon_pid" 2>/dev/null || true
  wait "$btmon_pid" 2>/dev/null || true
  if [[ -n ${SUDO_USER:-} ]]; then
    chown "$SUDO_USER" "$capture_dir" "$capture_dir"/*
  fi
}
trap cleanup EXIT

python3 "$(dirname "$0")/capture_lan.py" wlan0 "$capture_dir/wifi.pcap" --seconds "$capture_seconds"
