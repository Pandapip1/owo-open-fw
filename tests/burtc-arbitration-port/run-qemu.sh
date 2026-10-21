#!/usr/bin/env bash
# Build externally, then run the image against the in-tree EFR32 QEMU model.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build/burtc-arbitration-qemu"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
IMAGE="$BUILD_DIR/zephyr/zephyr.bin"
RUNTIME_DIR=$(mktemp -d "$ROOT/build/burtc-arbitration-runtime.XXXXXX")
trap 'rm -rf "$RUNTIME_DIR"' EXIT HUP INT TERM

if [ ! -x "$QEMU_BIN" ] || [ ! -f "$IMAGE" ]; then
	echo "burtc-arbitration: missing QEMU binary or image" >&2
	exit 2
fi

"$QEMU_BIN" -M "efr32bg22,flash=$IMAGE" -display none -monitor none \
	-icount shift=1 -trace enable=efr32bg22_usart_tx \
	-trace file="$RUNTIME_DIR/trace" >"$RUNTIME_DIR/guest.log" 2>&1 &
qemu_pid=$!
(
	sleep 3
	kill -TERM "$qemu_pid" 2>/dev/null || exit 0
) &
watchdog_pid=$!

set +e
wait "$qemu_pid"
status=$?
set -e
kill "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true
if [ "$status" -ne 0 ] && [ "$status" -ne 143 ]; then
	cat "$RUNTIME_DIR/guest.log" >&2
	exit "$status"
fi

console=$(sed -n 's/.*USART[01] TX=0x\([[:xdigit:]][[:xdigit:]]\).*/\1/p' \
	"$RUNTIME_DIR/trace" | xxd -r -p | tr -d '\r')
if ! printf '%s\n' "$console" | grep -Fqx 'burtc-arbitration: PASS'; then
	echo "burtc-arbitration: Counter/system-clock arbitration did not complete" >&2
	printf '%s\n' "$console" >&2
	exit 1
fi

echo "burtc-arbitration: PASS"
