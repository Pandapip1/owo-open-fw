#!/usr/bin/env bash
set -eu
# usage: run-qemu.sh <seconds> [extra qemu args]
#   env: FLASH=<image> SRV_ARGS=<server args>; server trace is in build/qemu-runtime/srv.log
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
QEMU_DIR="${QEMU_DIR:-$ROOT/vendor/qemu}"
RUNTIME_DIR="${RUNTIME_DIR:-$ROOT/build/qemu-runtime}"
mkdir -p "$RUNTIME_DIR"
T=$1; shift
FLASH=${FLASH:-$ROOT/evidence/stock/flash-stock.bin}
SOCK="$RUNTIME_DIR/efr32.sock"
rm -f "$SOCK"
python3 "$QEMU_DIR/tools/efr32bg22/server.py" --flash "$FLASH" ${SRV_ARGS:---trace} --sock "$SOCK" > "$RUNTIME_DIR/srv.log" 2>&1 &
SP=$!
trap 'kill "$SP" 2>/dev/null || true; rm -f "$SOCK"' EXIT
sleep 1
timeout "$T" "$QEMU_DIR/build/qemu-system-arm" -M efr32bg22,flash="$FLASH" -display none -monitor none \
  -serial unix:"$SOCK",server=off -icount shift=5 -d guest_errors,int -D "$RUNTIME_DIR/qemu.log" "$@"
