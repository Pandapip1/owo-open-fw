#!/usr/bin/env bash
# Build externally, then run the image against the in-tree EFR32 QEMU model, M0+ included.
#   nix develop . -c cmake -GNinja -S tests/sequencer-timed-wrap -B build/sequencer-timed-wrap \
#     -DBOARD=bg22_ek4108a -DZEPHYR_BASE="$PWD/vendor/zephyr" -DCONFIG_BUILD_ONLY_NO_BLOBS=y \
#     -DZEPHYR_MODULES="$PWD/vendor/cmsis_6;$PWD/vendor/hal_silabs"
#   nix develop . -c cmake --build build/sequencer-timed-wrap
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build/sequencer-timed-wrap"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
IMAGE="$BUILD_DIR/zephyr/zephyr.bin"

if [ ! -x "$QEMU_BIN" ] || [ ! -f "$IMAGE" ]; then
	echo "sequencer-timed-wrap: missing QEMU binary or image" >&2
	exit 2
fi
RUNTIME_DIR=$(mktemp -d "$BUILD_DIR/runtime.XXXXXX")
trap 'rm -rf "$RUNTIME_DIR"' EXIT HUP INT TERM

"$QEMU_BIN" -M "efr32bg22,flash=$IMAGE" -display none -monitor none \
	-icount shift=3,sleep=off -trace enable=efr32bg22_usart_tx \
	-trace file="$RUNTIME_DIR/trace" >"$RUNTIME_DIR/guest.log" 2>&1 &
qemu_pid=$!
(
	sleep "${RUN_SECONDS:-20}"
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

console=$(printf '%b' "$(sed -n 's/.*USART[01] TX=0x\([[:xdigit:]][[:xdigit:]]\).*/\\x\1/p' \
	"$RUNTIME_DIR/trace" | tr -d '\n')" | tr -d '\r')
printf '%s\n' "$console"
if ! printf '%s\n' "$console" | grep -q '^TESTSUITE sequencer_timed_wrap succeeded$'; then
	echo "sequencer-timed-wrap: FAIL" >&2
	exit 1
fi
echo "sequencer-timed-wrap: PASS"
