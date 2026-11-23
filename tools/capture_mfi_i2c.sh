#!/usr/bin/env bash
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

set -euo pipefail
umask 077

if [[ $# -ne 3 ]]; then
  echo "usage: capture_mfi_i2c.sh HELPER_PID OUTPUT_DIR SECONDS" >&2
  exit 2
fi

helper_pid=$1
capture_root=$(realpath -m -- "$(dirname "$0")/../captures")
capture_dir=$(realpath -m -- "$2")
capture_seconds=$3
if ! [[ $helper_pid =~ ^[0-9]+$ ]] || ! kill -0 "$helper_pid"; then
  echo "helper process is not running" >&2
  exit 2
fi
if [[ $capture_dir != "$capture_root/"* ]]; then
  echo "output must be under the private captures directory" >&2
  exit 2
fi
if ! [[ $capture_seconds =~ ^[0-9]+$ ]] || (( capture_seconds < 1 || capture_seconds > 180 )); then
  echo "duration must be 1..180 seconds" >&2
  exit 2
fi

mkdir -p "$capture_dir"
status=0
timeout --signal=INT --kill-after=5 "${capture_seconds}s" \
  strace -f -yy -P /dev/i2c-1 -e trace=openat,ioctl,read,write \
  -s 256 -ttt -o "$capture_dir/mfi-i2c.strace" -p "$helper_pid" || status=$?
if [[ -n ${SUDO_USER:-} ]]; then
  chown "$SUDO_USER" "$capture_dir" "$capture_dir/mfi-i2c.strace"
fi
if (( status != 0 && status != 124 && status != 130 )); then
  echo "I2C capture failed with status $status" >&2
  exit "$status"
fi
echo "I2C capture finished" >&2
