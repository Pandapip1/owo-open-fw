#!/usr/bin/env bash
# The staged-reply decision of the C sequencer against the assembly one, case by case, on the
# in-tree EFR32 QEMU model (M0+ included). Build both first (no west workspace):
#   for v in c asm; do
#     ZEPHYR_BASE="$PWD/vendor/zephyr" nix develop . -c cmake -GNinja -S tests/sequencer-decide -B build/sequencer-decide-$v \
#       -DBOARD=bg22_ek4108a -DZEPHYR_BASE="$PWD/vendor/zephyr" \
#       -DZEPHYR_MODULES="$PWD/vendor/cmsis_6;$PWD/vendor/hal_silabs" \
#       -DDTC_OVERLAY_FILE="$PWD/tests/sequencer-decide/boards/bg22_ek4108a.overlay" \
#       -DSEQ_ASM=$([ $v = asm ] && echo ON || echo OFF)
#     nix develop . -c cmake --build build/sequencer-decide-$v
#   done
# Each image runs every case (src/main.c, which checks whether each answered); compare.py then
# requires the two to agree on every case: return, OUTCOME, the stage's state, the sequencer's
# shared words, and the M0+'s register writes in order (the reply into the BUFC TX FIFO among
# them). It prints each case's decision cycles for both.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
C_BUILD_DIR=${C_BUILD_DIR:-"$ROOT/build/sequencer-decide-c"}
ASM_BUILD_DIR=${ASM_BUILD_DIR:-"$ROOT/build/sequencer-decide-asm"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
TIMEOUT_SECONDS=${1:-120}
OUT=${OUT:-"$ROOT/build/sequencer-decide-run"}

for d in "$C_BUILD_DIR" "$ASM_BUILD_DIR"; do
	if [ ! -x "$QEMU_BIN" ] || [ ! -f "$d/zephyr/zephyr.bin" ]; then
		echo "sequencer-decide: missing QEMU binary or $d/zephyr/zephyr.bin" >&2
		exit 2
	fi
done
mkdir -p "$OUT"

# Runs one image until ztest reports the end of the run (or the timeout).
run() {
	local name=$1 image=$2
	rm -f "$OUT/$name.console" "$OUT/$name.trace"
	"$QEMU_BIN" -M "efr32bg22,flash=$image" -display none -monitor none \
		-icount shift=3,sleep=off -serial null -serial "file:$OUT/$name.console" \
		-trace enable=memory_region_ops_write -trace file="$OUT/$name.trace" \
		>"$OUT/$name.guest" 2>&1 &
	local pid=$! start
	start=$(date +%s)
	while kill -0 "$pid" 2>/dev/null &&
	      [ $(($(date +%s) - start)) -lt "$TIMEOUT_SECONDS" ] &&
	      ! grep -aq 'PROJECT EXECUTION' "$OUT/$name.console" 2>/dev/null; do
		sleep 1
	done
	kill -TERM "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	tr -d '\r' <"$OUT/$name.console" >"$OUT/$name.log"
	if ! grep -q 'PROJECT EXECUTION SUCCESSFUL' "$OUT/$name.log"; then
		echo "sequencer-decide: the $name image's cases failed" >&2
		grep -E 'FAIL|Assertion|answered' "$OUT/$name.log" >&2 || tail -20 "$OUT/$name.log" >&2
		return 1
	fi
}

status=0
run c "$C_BUILD_DIR/zephyr/zephyr.bin" || status=1
run asm "$ASM_BUILD_DIR/zephyr/zephyr.bin" || status=1
python3 "$ROOT/tests/sequencer-decide/compare.py" "$OUT/c.log" "$OUT/c.trace" \
	"$OUT/asm.log" "$OUT/asm.trace" || status=1
[ "$status" -eq 0 ] && echo "sequencer-decide: PASS"
exit "$status"
