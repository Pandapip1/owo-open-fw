#!/usr/bin/env bash
# Run Zephyr's deadline_budget suite (run budget enforcement) on the in-tree EFR32BG22 QEMU model,
# where the budget timer is a scheduler clock event (TIMER0 while the CPU runs).
#
# Build prerequisite, from the repository root:
#   nix develop . -c cmake -G Ninja \
#     -S vendor/zephyr/tests/kernel/sched/deadline_budget -B build/zephyr-budget-efr32 \
#     -DBOARD=bg22_ek4108a -DZEPHYR_BASE="$PWD/vendor/zephyr" \
#     -DZEPHYR_MODULES="$PWD/vendor/cmsis_6;$PWD/vendor/hal_silabs;$PWD/vendor/mbedtls;$PWD/vendor/tf-psa-crypto;$PWD/vendor/zephyr-lang-rust" \
#     -DEXTRA_CONF_FILE="$PWD/tests/scheduler-deadline-qemu/budget.conf" \
#     -DDTC_OVERLAY_FILE="$PWD/tests/scheduler-deadline-qemu/sched-clock.overlay"
#   nix develop . -c cmake --build build/zephyr-budget-efr32
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build/zephyr-budget-efr32"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
RUN_SECONDS=${1:-30}
IMAGE="$BUILD_DIR/zephyr/zephyr.bin"

if [ ! -x "$QEMU_BIN" ] || [ ! -f "$IMAGE" ]; then
	echo "budget-qemu: missing QEMU binary or image" >&2
	exit 2
fi
RUNTIME_DIR=$(mktemp -d "$ROOT/build/budget-qemu-runtime.XXXXXX")
trap 'rm -rf "$RUNTIME_DIR"' EXIT HUP INT TERM

timeout "$RUN_SECONDS" "$QEMU_BIN" -M "efr32bg22,flash=$IMAGE" -display none -monitor none \
	-icount shift=3 -chardev file,id=soc.usart1,path="$RUNTIME_DIR/console" \
	>"$RUNTIME_DIR/qemu.log" 2>&1 || true
console=$(tr -d '\r\000' <"$RUNTIME_DIR/console")
printf '%s\n' "$console" | grep -aE ' (PASS|FAIL) - |budget overrun detected' || true
if ! printf '%s\n' "$console" | grep -aq '^TESTSUITE deadline_budget succeeded$'; then
	echo "budget-qemu: the deadline_budget suite did not succeed" >&2
	exit 1
fi
