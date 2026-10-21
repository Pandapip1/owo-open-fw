#!/usr/bin/env bash
# Run the in-tree Zephyr advertiser and scanner together through the
# QEMU-only RF medium. A successful run requires the scanner's normal Zephyr
# host callback, not merely a BUFC/FRC completion.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TX_BUILD_DIR=${TX_BUILD_DIR:-"$ROOT/build/radio-port-qemu"}
RX_BUILD_DIR=${RX_BUILD_DIR:-"$ROOT/build/radio-rx-port-qemu"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
RUN_SECONDS=${1:-5}
# This is a QEMU TCG startup convention, not a modeled silicon clock.  A
# smaller round-robin quantum lets the receiver arm before the peer's one-shot
# advertiser starts; the trace-order assertion below makes that prerequisite
# explicit instead of relying on an RF-medium packet queue.
ICOUNT_SHIFT=${ICOUNT_SHIFT:-1}
TX_IMAGE="$TX_BUILD_DIR/zephyr/zephyr.bin"
RX_IMAGE="$RX_BUILD_DIR/zephyr/zephyr.bin"

if [ ! -x "$QEMU_BIN" ] || [ ! -f "$TX_IMAGE" ] || [ ! -f "$RX_IMAGE" ]; then
    echo "missing QEMU binary, advertiser image, or scanner image" >&2
    exit 2
fi

RUNTIME_DIR=$(mktemp -d "$ROOT/build/radio-dual-e2e-runtime.XXXXXX")
trap 'rm -rf "$RUNTIME_DIR"' EXIT HUP INT TERM

"$QEMU_BIN" \
    -M "efr32bg22-dual,flash0=$TX_IMAGE,flash1=$RX_IMAGE" \
    -display none -monitor none -icount "shift=$ICOUNT_SHIFT" -d guest_errors \
    -D "$RUNTIME_DIR/qemu.log" \
    -trace enable=efr32bg22_usart_tx \
    -trace enable=efr32bg22_radio_write \
    -trace enable=efr32bg22_rac_command \
    -trace enable=efr32bg22_rac_state \
    -trace enable=efr32bg22_tx_snapshot \
    -trace enable=efr32bg22_tx_offer_count \
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
    sed -n '1,120p' "$RUNTIME_DIR/guest.log" >&2
    exit "$status"
fi

# The tracepoint is emitted for every modeled USART instance.  The current
# board images use USART1, while accepting USART0 here keeps the harness from
# silently dropping a future board-console choice.
console=$(sed -n 's/.*USART[01] TX=0x\([[:xdigit:]][[:xdigit:]]\).*/\1/p' \
    "$RUNTIME_DIR/radio.trace" | xxd -r -p | tr -d '\r')
printf '%s\n' "$console" | grep -E '^(bt_enable|scan_callback_register|scan_start|rx_callback):' || true

if ! grep -q 'RAC.CMD=0x00000001' "$RUNTIME_DIR/radio.trace"; then
    echo "radio-dual-e2e: advertiser did not reach RAC TXEN" >&2
    exit 1
fi

rx_arm_line=$(grep -n 'addr=0xa8021008 value=0x00000002' "$RUNTIME_DIR/radio.trace" |
    head -1 | cut -d: -f1)
tx_snapshot_line=$(grep -n 'TX snapshot sequence=' "$RUNTIME_DIR/radio.trace" |
    head -1 | cut -d: -f1)
if [ -z "$rx_arm_line" ] || [ -z "$tx_snapshot_line" ] ||
   [ "$rx_arm_line" -ge "$tx_snapshot_line" ]; then
    echo "radio-dual-e2e: scanner RX arm did not precede advertiser snapshot" >&2
    echo "radio-dual-e2e: set ICOUNT_SHIFT only for QEMU startup scheduling; this is not silicon timing" >&2
    exit 1
fi

if ! grep -Eq 'TX snapshot RX mailbox offers=[1-9][0-9]*' "$RUNTIME_DIR/radio.trace"; then
    echo "radio-dual-e2e: advertiser snapshot produced no positive RF-medium offer" >&2
    echo "radio-dual-e2e: current blocker is TX completion retiring the active transaction before peer RXSEARCH" >&2
    grep -E 'RAC.CMD=|RAC state|TX snapshot' "$RUNTIME_DIR/radio.trace" >&2 || true
    exit 1
fi

if ! grep -q 'addr=0xa8021008 value=0x00000002' "$RUNTIME_DIR/radio.trace"; then
    echo "radio-dual-e2e: scanner never armed software RX" >&2
    exit 1
fi

if ! printf '%s\n' "$console" | grep -q '^rx_callback: len='; then
    echo "radio-dual-e2e: positive RF offer did not reach a Zephyr scan callback" >&2
    echo "radio-dual-e2e: expected a QEMU descriptor-normalized PDU" >&2
    exit 1
fi

echo "radio-dual-e2e: QEMU-only Zephyr RX callback received a normalized PDU"
