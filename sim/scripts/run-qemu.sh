#!/usr/bin/env bash
set -eu
# usage: run-qemu.sh <seconds> [extra qemu args]
# env: FLASH=<image>; QEMU's own log is build/qemu-runtime/qemu.log
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
QEMU_BIN="${QEMU_BIN:-$ROOT/build/qemu/qemu-system-arm}"
RUNTIME_DIR="${RUNTIME_DIR:-$ROOT/build/qemu-runtime}"
mkdir -p "$RUNTIME_DIR"
T=$1; shift
FLASH=${FLASH:-$ROOT/evidence/stock/flash-stock.bin}
timeout "$T" "$QEMU_BIN" -M efr32bg22,flash="$FLASH" -display none -monitor none \
  -icount shift=5 -d guest_errors,int -D "$RUNTIME_DIR/qemu.log" "$@"
