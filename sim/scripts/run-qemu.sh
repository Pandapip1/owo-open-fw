#!/usr/bin/env bash
set -eu
# usage: run-qemu.sh <seconds> [extra qemu args]
# env: FLASH=<image>; QEMU's own log is build/qemu-runtime/qemu.log
#
# The Nix development shell is used on macOS as well as Linux, so do not
# depend on GNU coreutils' `timeout`. The local watchdog deliberately keeps
# timeout's conventional 124 status for a bounded run.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
QEMU_BIN="${QEMU_BIN:-$ROOT/build/qemu/qemu-system-arm}"
RUNTIME_DIR="${RUNTIME_DIR:-$ROOT/build/qemu-runtime}"
mkdir -p "$RUNTIME_DIR"
T=$1; shift
FLASH=${FLASH:-$ROOT/evidence/stock/flash-stock.bin}

"$QEMU_BIN" -M efr32bg22,flash="$FLASH" -display none -monitor none \
  -icount shift=5 -d guest_errors,int -D "$RUNTIME_DIR/qemu.log" "$@" &
qemu_pid=$!
(
  sleep "$T"
  kill -TERM "$qemu_pid" 2>/dev/null || exit 0
  sleep 1
  kill -KILL "$qemu_pid" 2>/dev/null || true
) &
watchdog_pid=$!

set +e
wait "$qemu_pid"
status=$?
set -e
kill "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true

if [ "$status" -eq 143 ] || [ "$status" -eq 137 ]; then
  exit 124
fi
exit "$status"
