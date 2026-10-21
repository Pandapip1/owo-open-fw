#!/usr/bin/env bash
# Run the scanner-only EFR32 radio workload under the in-tree QEMU model.
#
# Build from the repository root:
#   nix develop . -c cmake -G Ninja -S tests/radio-rx-port -B build/radio-rx-port-qemu \
#     -DBOARD=bg22_ek4108a -DZEPHYR_BASE="$PWD/vendor/zephyr" \
#     -DZephyr_DIR="$PWD/vendor/zephyr/share/zephyr-package/cmake" \
#     -DZEPHYR_MODULES="$PWD/vendor/cmsis_6;$PWD/vendor/hal_silabs;$PWD/vendor/mbedtls;$PWD/vendor/tf-psa-crypto;$PWD/vendor/zephyr-lang-rust"
#   nix develop . -c cmake --build build/radio-rx-port-qemu
#
# Passing proves host scanner acceptance and the source-backed RAC software-RX
# arm write only; it does not claim an RX callback or decoded air PDU.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build/radio-rx-port-qemu"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
RUN_SECONDS=${1:-3}
IMAGE="$BUILD_DIR/zephyr/zephyr.bin"

if [ ! -x "$QEMU_BIN" ] || [ ! -f "$IMAGE" ]; then
    echo "missing QEMU binary or scanner image" >&2
    exit 2
fi

RUNTIME_DIR=$(mktemp -d "$ROOT/build/radio-rx-port-qemu-runtime.XXXXXX")
trap 'rm -rf "$RUNTIME_DIR"' EXIT HUP INT TERM

"$QEMU_BIN" -M efr32bg22,flash="$IMAGE" \
    -display none -monitor none -icount shift=5 -d guest_errors \
    -D "$RUNTIME_DIR/qemu.log" \
    -trace enable=efr32bg22_usart_tx \
    -trace enable=efr32bg22_radio_write \
    -trace file="$RUNTIME_DIR/radio.trace" \
    >"$RUNTIME_DIR/guest.log" 2>&1 &
qemu_pid=$!
(
    sleep "$RUN_SECONDS"
    kill -TERM "$qemu_pid" 2>/dev/null || exit 0
    sleep 1
    kill -KILL "$qemu_pid" 2>/dev/null || true
) &
watchdog_pid=$!

set +e
wait "$qemu_pid"
status=$?
set -e
kill "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true
if [ "$status" -eq 143 ] || [ "$status" -eq 137 ]; then
    status=124
fi
if [ "$status" -ne 0 ] && [ "$status" -ne 124 ]; then
    sed -n '1,80p' "$RUNTIME_DIR/guest.log" >&2
    exit "$status"
fi

console=$(sed -n 's/.*USART1 TX=0x\([[:xdigit:]][[:xdigit:]]\).*/\1/p' \
    "$RUNTIME_DIR/radio.trace" | xxd -r -p | tr -d '\r')
printf '%s\n' "$console" | grep -E '^(bt_enable|scan_start):' || true
if ! printf '%s\n' "$console" | grep -q '^bt_enable: 0$'; then
    echo "radio-rx: bt_enable did not report success"
    exit 1
elif ! printf '%s\n' "$console" | grep -q '^scan_start: 0$'; then
    echo "radio-rx: passive scanner did not report success"
    exit 1
elif grep -q 'addr=0xa8021008 value=0x00000002' "$RUNTIME_DIR/radio.trace"; then
    echo "radio-rx: RAC software-RX arm observed"
else
    echo "radio-rx: scanner accepted, but no RAC software-RX arm observed"
    exit 1
fi
