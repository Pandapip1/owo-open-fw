#!/usr/bin/env bash
set -eu

# run once, report stuck/unmodelled/last PC context
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
RUNTIME_DIR="${RUNTIME_DIR:-$ROOT/build/qemu-runtime}"

SRV_ARGS="${SRV_ARGS:-}" "$ROOT/sim/scripts/run-qemu.sh" "${1:-25}" "${@:2}" 2>&1 | head -3
grep -h "STUCK\|unmodelled" "$RUNTIME_DIR/srv.log" | sort -u
tail -2 "$RUNTIME_DIR/srv.log" | cut -c1-160
