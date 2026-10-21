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

# The scanner's real Zephyr descriptor worker must acknowledge the synthetic
# channel-2 FIFO, not merely receive one host callback.  WRITEOFFSET setup is
# the producer-side boundary; successive READOFFSET writes prove that the
# scheduler-owned consumer drained repeated records and can cross a physical
# 512-byte ring wrap.  This is a QEMU-only trace contract, not silicon BUFC
# semantics.
descriptor_write_offset_line=$(grep -E \
    'addr=0x(aa|ba)000094 value=0x00000000' "$RUNTIME_DIR/radio.trace" |
    head -1 || true)
descriptor_ack_count=$(grep -Ec \
    'addr=0x(aa|ba)000098 value=0x000000(08|10)' "$RUNTIME_DIR/radio.trace" || true)
if [ -z "$descriptor_write_offset_line" ] || [ "$descriptor_ack_count" -lt 2 ]; then
    echo "radio-dual-e2e: scanner did not acknowledge repeated QEMU RX descriptors" >&2
    echo "radio-dual-e2e: expected channel-2 WRITEOFFSET setup and at least two READOFFSET writes" >&2
    grep -E 'addr=0x(aa|ba)00009[48]' "$RUNTIME_DIR/radio.trace" | head -20 >&2 || true
    exit 1
fi

# The advertiser's QEMU-only TIFS seam is exercised only when its TX state
# retires before a later software-RX arm.  The trace is intentionally a
# machine-level boundary: it does not identify a silicon packet-end event.
tx_off_line=$(grep -n 'RAC state 9 -> 0' "$RUNTIME_DIR/radio.trace" |
    head -1 | cut -d: -f1)
post_tx_rx_arm_line=$(awk -v start="${tx_off_line:-0}" '
    NR > start && /addr=0xa8021008 value=0x00000002/ { print NR; exit }
' "$RUNTIME_DIR/radio.trace")
if [ -z "$tx_off_line" ] || [ -z "$post_tx_rx_arm_line" ] ||
   [ "$tx_off_line" -ge "$post_tx_rx_arm_line" ]; then
    echo "radio-dual-e2e: TX completion did not precede a later QEMU TIFS RX arm" >&2
    echo "radio-dual-e2e: this checks scheduler progression only, not silicon timing" >&2
    exit 1
fi

if ! grep -q 'addr=0xa8021008 value=0x00000002' "$RUNTIME_DIR/radio.trace"; then
    echo "radio-dual-e2e: scanner never armed software RX" >&2
    exit 1
fi

# The descriptor ring is an image-owned QEMU test buffer.  The stock root
# (0x20001178) is evidence about the vendor firmware, not a Zephyr RAM ABI.
descriptor_addr_line=$(grep -E 'addr=0x(aa|ba)000090 value=0x2000[0-9a-f]+' \
    "$RUNTIME_DIR/radio.trace" | head -1 || true)
rx_fifo_addr_line=$(grep -E 'addr=0x(aa|ba)000050 value=0x2000[0-9a-f]+' \
    "$RUNTIME_DIR/radio.trace" | head -1 || true)
descriptor_addr=$(printf '%s\n' "$descriptor_addr_line" |
    sed -n 's/.*value=0x\([0-9a-f][0-9a-f]*\).*/\1/p')
rx_fifo_addr=$(printf '%s\n' "$rx_fifo_addr_line" |
    sed -n 's/.*value=0x\([0-9a-f][0-9a-f]*\).*/\1/p')
if [ -z "$descriptor_addr" ] || [ -z "$rx_fifo_addr" ]; then
    echo "radio-dual-e2e: scanner did not program an image-owned descriptor ring" >&2
    exit 1
fi
descriptor_start=$((16#$descriptor_addr))
descriptor_end=$((descriptor_start + 512))
rx_fifo_start=$((16#$rx_fifo_addr))
rx_fifo_end=$((rx_fifo_start + 512))
printf 'radio-dual-e2e: descriptor=[0x%08x,0x%08x) rx_fifo=[0x%08x,0x%08x)\n' \
    "$descriptor_start" "$descriptor_end" "$rx_fifo_start" "$rx_fifo_end"
if [ "$descriptor_start" -lt "$rx_fifo_end" ] &&
   [ "$rx_fifo_start" -lt "$descriptor_end" ]; then
    echo "radio-dual-e2e: descriptor and RX FIFO backing regions overlap" >&2
    exit 1
fi
if [ "$descriptor_start" -eq $((16#20001178)) ]; then
    echo "radio-dual-e2e: descriptor ring reused stock firmware root" >&2
    exit 1
fi

expected_callback='rx_callback: len=3 first=2 second=1 third=6'
# radio-port publishes one Flags AD structure: length=2, type=0x01, value=0x06.
# Zephyr's host callback receives those three AD bytes, so require the complete
# marker rather than accepting an unrelated callback or malformed payload.
if ! printf '%s\n' "$console" | grep -Fqx "$expected_callback"; then
    echo "radio-dual-e2e: expected advertiser Flags AD callback was not observed" >&2
    echo "radio-dual-e2e: expected: $expected_callback" >&2
    exit 1
fi
if printf '%s\n' "$console" | grep -E '^rx_callback:' | \
    grep -Fvx "$expected_callback"; then
    echo "radio-dual-e2e: observed a callback with an unexpected payload marker" >&2
    exit 1
fi

echo "radio-dual-e2e: QEMU-only Zephyr RX callback received a normalized PDU"
