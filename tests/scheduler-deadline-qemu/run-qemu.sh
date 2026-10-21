#!/usr/bin/env bash
# Run Zephyr's scheduler deadline suite on the in-tree EFR32BG22 QEMU model.
#
# Build prerequisite, from the repository root:
#   nix develop . -c cmake -G Ninja \
#     -S vendor/zephyr/tests/kernel/sched/deadline \
#     -B build/zephyr-deadline-efr32-burtc \
#     -DBOARD=bg22_ek4108a -DZEPHYR_BASE="$PWD/vendor/zephyr" \
#     -DZephyr_DIR="$PWD/vendor/zephyr/share/zephyr-package/cmake" \
#     -DZEPHYR_MODULES="$PWD/vendor/cmsis_6;$PWD/vendor/hal_silabs;$PWD/vendor/mbedtls;$PWD/vendor/tf-psa-crypto;$PWD/vendor/zephyr-lang-rust" \
#     -DEXTRA_CONF_FILE="$PWD/tests/scheduler-deadline-qemu/burtc.conf" \
#     -DDTC_OVERLAY_FILE="$PWD/tests/scheduler-deadline-qemu/bg22_ek4108a.overlay"
#   nix develop . -c cmake --build build/zephyr-deadline-efr32-burtc
#
# The image uses BURTC rather than the unmodeled RTCC sleeptimer.  In
# particular, this validates `test_release_with_deadline_order`: two
# same-priority persistent threads are made ready in IRQ context and normal
# IRQ-exit scheduling chooses the earlier K_DEADLINE first.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build/zephyr-deadline-efr32-burtc"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
RUN_SECONDS=${1:-8}
IMAGE="$BUILD_DIR/zephyr/zephyr.bin"

if [ ! -x "$QEMU_BIN" ]; then
	echo "missing QEMU binary: $QEMU_BIN" >&2
	exit 2
fi
if [ ! -f "$IMAGE" ]; then
	echo "missing deadline-suite image: $IMAGE" >&2
	exit 2
fi

RUNTIME_DIR=$(mktemp -d "$ROOT/build/zephyr-deadline-qemu-runtime.XXXXXX")
cleanup() {
	rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT HUP INT TERM

"$QEMU_BIN" -M "efr32bg22,flash=$IMAGE" \
	-display none -monitor none -icount shift=5 -d guest_errors \
	-D "$RUNTIME_DIR/qemu.log" \
	-trace enable=efr32bg22_usart_tx \
	-trace enable=efr32bg22_burtc_compare_expire \
	-trace enable=efr32bg22_burtc_irq \
	-trace file="$RUNTIME_DIR/qemu.trace" \
	>"$RUNTIME_DIR/guest.log" 2>&1 &
qemu_pid=$!
(
	sleep "$RUN_SECONDS"
	kill -TERM "$qemu_pid" 2>/dev/null || exit 0
	sleep 1
	kill -KILL "$qemu_pid" 2>/dev/null || true
) &
watchdog_pid=$!

qemu_status=0
wait "$qemu_pid" || qemu_status=$?
kill "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true
if [ "$qemu_status" -eq 143 ] || [ "$qemu_status" -eq 137 ]; then
	qemu_status=124
fi
if [ "$qemu_status" -ne 0 ] && [ "$qemu_status" -ne 124 ]; then
	cat "$RUNTIME_DIR/guest.log" >&2
	exit "$qemu_status"
fi

console=$(sed -n 's/.*USART1 TX=0x\([[:xdigit:]][[:xdigit:]]\).*/\1/p' \
	"$RUNTIME_DIR/qemu.trace" | xxd -r -p | tr -d '\r')
printf '%s\n' "$console"

if ! printf '%s\n' "$console" | grep -q '^ PASS - test_release_with_deadline_order '; then
	echo "deadline-qemu: IRQ-offload EDF release-order test did not pass" >&2
	exit 1
fi
if ! printf '%s\n' "$console" | grep -q '^TESTSUITE suite_deadline succeeded$'; then
	echo "deadline-qemu: deadline suite did not succeed" >&2
	exit 1
fi

echo "deadline-qemu: BURTC/IRQ18 scheduler deadline suite passed"
