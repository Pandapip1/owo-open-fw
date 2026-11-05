#!/usr/bin/env bash
# Transmit power control under the in-tree EFR32BG22 QEMU (docs/tx-power.md): one machine runs
# src/main.c's checks and prints "tx-power: PASS".
#
# Image (from the repository root):
#   nix develop . -c cmake -G Ninja -S tests/tx-power-port -B build/tx-power-port-qemu \
#     -DBOARD=bg22_ek4108a -DZEPHYR_BASE="$PWD/vendor/zephyr" \
#     -DZephyr_DIR="$PWD/vendor/zephyr/share/zephyr-package/cmake" \
#     -DZEPHYR_MODULES="$PWD/vendor/cmsis_6;$PWD/vendor/hal_silabs;$PWD/vendor/mbedtls;$PWD/vendor/tf-psa-crypto;$PWD/vendor/zephyr-lang-rust"
#   nix develop . -c cmake --build build/tx-power-port-qemu
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE=${IMAGE:-"$ROOT/build/tx-power-port-qemu/zephyr/zephyr.bin"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
# Virtual time is enough for the checks and the 4.3 s Morse identification.
RUN_SECONDS=${RUN_SECONDS:-60}
OUT=${OUT:-"$ROOT/build/tx-power-port-qemu-run"}

for f in "$QEMU_BIN" "$IMAGE"; do
    if [ ! -e "$f" ]; then
        echo "missing: $f" >&2
        exit 2
    fi
done
rm -rf "$OUT"
mkdir -p "$OUT"

timeout "$RUN_SECONDS" "$QEMU_BIN" -M "efr32bg22,flash=$IMAGE" \
    -display none -monitor none -icount shift=3,sleep=off \
    -serial null -serial "file:$OUT/console.raw" >"$OUT/qemu.log" 2>&1 || true

tr -d '\r' <"$OUT/console.raw" >"$OUT/console.txt" 2>/dev/null || : >"$OUT/console.txt"
grep -aE '^(bt_enable|tx-power|efr32-radio)' "$OUT/console.txt" || true
grep -aq '^tx-power: PASS' "$OUT/console.txt"
