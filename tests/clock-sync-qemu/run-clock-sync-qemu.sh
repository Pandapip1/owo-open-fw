#!/usr/bin/env bash
# The scheduler clock's self-checks (CONFIG_CLOCK_EVENT_SCHED_SYNC_CHECK) against clock faults
# injected into the EFR32BG22 model at run time (vendor/qemu x-hf-ppm, x-timer0-stopped,
# x-lf-ppm, x-lf-stopped). Each scenario boots the image afresh on the suit board model, injects
# one fault at a point fixed in the guest, never by the model's speed (inject.py), and checks
# the guest's report:
#
# - none: no fault, no report;
# - TIMERs' clock +2000 ppm, BURTC's clock -2000 ppm, TIMER0 or BURTC's clock stopped, as the
#   awake phase starts: drift (reason 1);
# - BURTC's clock stopped at the first entry to sleep: the edge capture overruns its window
#   (reason 2).
#
# Build prerequisite, from the repository root (scratch build recipe as the other QEMU suites):
#   cmake -GNinja -S tests/clock-sync-qemu -B build/clock-sync-qemu -DBOARD=bg22_ek4108a \
#     -DDTC_OVERLAY_FILE=$PWD/tests/scheduler-deadline-qemu/sched-clock.overlay ...
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build/clock-sync-qemu"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
IMAGE="$BUILD_DIR/zephyr/zephyr.bin"
fails=0

run() {
	local name=$1 phase=$2 prop=$3 value=$4 expect=$5
	local dir
	dir=$(mktemp -d "$ROOT/build/clock-sync-run.XXXXXX")
	(cd "$dir" && exec "$QEMU_BIN" -M "efr32bg22,flash=$IMAGE,board=owo-suit" -display none -monitor none \
		-qmp unix:qmp.sock,server=on,wait=off -gdb unix:gdb.sock,server=on,wait=off \
		-icount shift=5,sleep=off \
		-trace enable=efr32bg22_usart_tx -trace file="$dir/qemu.trace" \
		>"$dir/guest.log" 2>&1) &
	local qemu=$!
	local out
	out=$(python3 "$ROOT/tests/clock-sync-qemu/inject.py" "$dir/qmp.sock" "$dir/gdb.sock" \
		"$BUILD_DIR/zephyr/zephyr.elf" "$dir/qemu.trace" "$phase" "$prop" "$value" 600 || true)
	# The model runs on after the guest's end: end it.
	kill -9 "$qemu" 2>/dev/null || true
	wait || true
	local got
	got=$(printf '%s\n' "$out" | sed -n 's/^clock sync fault: reason \([0-9]*\).*/\1/p' | head -1)
	if [ "${got:-0}" = "$expect" ]; then
		echo "clock-sync: $name: ok (reason ${got:-none})"
	else
		echo "clock-sync: $name: FAILED, expected reason $expect, got ${got:-none}"
		printf '%s\n' "$out" | tail -5
		fails=$((fails + 1))
	fi
	rm -rf "$dir"
}

run none awake none 0 0
run hf-drift awake x-hf-ppm 2000 1
run lf-drift awake x-lf-ppm -2000 1
run timer0-stopped awake x-timer0-stopped true 1
run lf-stopped awake x-lf-stopped true 1
run lf-stopped-capture sleeping x-lf-stopped true 2
[ "$fails" -eq 0 ]
