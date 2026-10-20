#!/usr/bin/env bash
# Run the real radio-port advertiser briefly under the in-tree EFR32BG22 QEMU.
#
# Prerequisite (from the repository root):
#   nix develop . -c cmake -G Ninja -S tests/radio-port -B build/radio-port-qemu \
#     -DBOARD=bg22_ek4108a -DZEPHYR_BASE="$PWD/vendor/zephyr" \
#     -DZephyr_DIR="$PWD/vendor/zephyr/share/zephyr-package/cmake" \
#     -DZEPHYR_MODULES="$PWD/vendor/cmsis_6;$PWD/vendor/hal_silabs;$PWD/vendor/mbedtls;$PWD/vendor/tf-psa-crypto;$PWD/vendor/zephyr-lang-rust"
#   nix develop . -c cmake --build build/radio-port-qemu
#
# Output is intentionally bounded: the application marker pair (recorded by
# the native USART model's tracepoint) plus QEMU's radio-register and RAC
# traces. It reports the scheduler boundary separately from TX: a RAC disable
# command alone is not evidence that an advertising PDU was started. Temporary
# QEMU logs and trace output are removed on every exit.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build/radio-port-qemu"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
RUN_SECONDS=${1:-3}

IMAGE="$BUILD_DIR/zephyr/zephyr.bin"
if [ ! -x "$QEMU_BIN" ]; then
    echo "missing QEMU binary: $QEMU_BIN" >&2
    exit 2
fi
if [ ! -f "$IMAGE" ]; then
    echo "missing radio-port image: $IMAGE" >&2
    exit 2
fi

RUNTIME_DIR=$(mktemp -d "$ROOT/build/radio-port-qemu-runtime.XXXXXX")
cleanup() {
    rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT HUP INT TERM

# QEMU intentionally exits 124 at the time limit; retain failures other than
# that so a bad invocation is not mistaken for a silent radio path.
set +e
# `-k` protects the smoke bound even if an emulated CPU ignores SIGTERM.
timeout --kill-after=1s "$RUN_SECONDS" "$QEMU_BIN" -M efr32bg22,flash="$IMAGE" \
    -display none -monitor none -icount shift=5 -d guest_errors \
    -D "$RUNTIME_DIR/qemu.log" \
    -trace enable=efr32bg22_usart_tx \
    -trace enable=efr32bg22_radio_write \
    -trace enable=efr32bg22_rac_command \
    -trace enable=efr32bg22_tx_snapshot \
    -trace file="$RUNTIME_DIR/radio.trace" \
    >"$RUNTIME_DIR/guest.log" 2>&1
status=$?
set -e
if [ "$status" -ne 0 ] && [ "$status" -ne 124 ]; then
    sed -n '1,80p' "$RUNTIME_DIR/guest.log" >&2
    exit "$status"
fi

# The EFR32 model intentionally has no host character device.  Reconstruct
# printable console bytes only from QEMU's internal native-USART trace, rather
# than creating a host-I/O shortcut. `xxd` is provided by the Nix shell.
console=$(sed -n 's/.*USART1 TX=0x\([[:xdigit:]][[:xdigit:]]\).*/\1/p' \
    "$RUNTIME_DIR/radio.trace" | xxd -r -p | tr -d '\r')
printf '%s\n' "$console" | grep -E '^(bt_enable|adv_start):' || true
if ! printf '%s\n' "$console" | grep -q '^bt_enable: 0$'; then
    echo "radio-mmio: bt_enable did not report success through native USART tracing"
elif printf '%s\n' "$console" | grep -q '^adv_start: 0$'; then
    if printf '%s\n' "$console" | grep -q \
        '^efr32-radio: radio_tmr_start needs '; then
        echo "radio-mmio: LLL reached scheduled start; PROTIMER compare/IRQ remains unmodeled"
        sed -n '1,40p' "$RUNTIME_DIR/radio.trace"
    elif grep -q 'RAC.CMD=0x00000001' "$RUNTIME_DIR/radio.trace"; then
        echo "radio-mmio: RAC TXEN observed"
        sed -n '1,40p' "$RUNTIME_DIR/radio.trace"
    elif [ -s "$RUNTIME_DIR/radio.trace" ]; then
        echo "radio-mmio: advertiser queued, radio MMIO observed but no RAC command"
        sed -n '1,40p' "$RUNTIME_DIR/radio.trace"
    else
        echo "radio-mmio: advertiser queued, no radio MMIO observed"
    fi
else
    echo "radio-mmio: no advertiser queued (adv_start did not succeed)"
fi
