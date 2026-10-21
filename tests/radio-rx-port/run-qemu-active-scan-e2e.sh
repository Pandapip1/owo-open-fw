#!/usr/bin/env bash
# Exercise the QEMU-only active-scan exchange after building the scanner with
# prj-active-scan.conf. This is intentionally separate from the passive RX
# descriptor test: a passing run requires the full advertisement -> SCAN_REQ
# -> SCAN_RSP sequence, rather than treating one synthetic RX descriptor as
# proof of an active exchange.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TX_BUILD_DIR=${TX_BUILD_DIR:-"$ROOT/build/radio-port-qemu"}
RX_BUILD_DIR=${RX_BUILD_DIR:-"$ROOT/build/radio-rx-port-active-qemu"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
RUN_SECONDS=${1:-5}
# The QEMU TIFS worker's measured 150-us interval needs shift=5; shift=1
# stretches it to about 1.5 ms and lets the LLL clean up before TXEN.
ICOUNT_SHIFT=${ICOUNT_SHIFT:-5}
TX_IMAGE="$TX_BUILD_DIR/zephyr/zephyr.bin"
RX_IMAGE="$RX_BUILD_DIR/zephyr/zephyr.bin"

if [ ! -x "$QEMU_BIN" ] || [ ! -f "$TX_IMAGE" ] || [ ! -f "$RX_IMAGE" ]; then
    echo "active-scan-e2e: missing QEMU binary, advertiser image, or active scanner image" >&2
    exit 2
fi

RUNTIME_DIR=$(mktemp -d "$ROOT/build/radio-active-scan-e2e-runtime.XXXXXX")
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

console=$(sed -n 's/.*USART[01] TX=0x\([[:xdigit:]][[:xdigit:]]\).*/\1/p' \
    "$RUNTIME_DIR/radio.trace" | xxd -r -p | tr -d '\r')
printf '%s\n' "$console" | grep -E '^(bt_enable|scan_callback_register|scan_start):' || true
# Guest-console progress is informative only: ICOUNT's host pacing can delay
# printk after the controller has already issued its RF request. The
# endpoint-tagged TX snapshots below are the authoritative active-scan proof.

# Each machine has a private address space, so its MMIO windows do not tag a
# sender. The QEMU model does: endpoint 0 is the advertiser and endpoint 1 is
# the scanner. Startup is deliberately unconstrained: select an advertiser
# snapshot immediately preceding an eventual scanner request, rather than
# requiring that the scanner win the initial TCG scheduling race. The scanner
# must issue the observed 14-byte SCAN_REQ (`c3 0c`, QEMU little-endian word
# `0x1a1a0cc3`), followed by endpoint 0's eight-byte SCAN_RSP (`44 06`, word
# `0x743a0644`). These are QEMU snapshot boundaries, not real RF airtime or
# hardware FRC/PROTIMER event-ordering assertions.
scan_request_line=$(grep -n 'endpoint=1 TX snapshot sequence=.* bytes=14 first=0x1a1a0cc3' \
    "$RUNTIME_DIR/radio.trace" | head -1 | cut -d: -f1)
advertisement_line=$(awk -v end="${scan_request_line:-0}" '
    NR < end && /endpoint=0 TX snapshot sequence=.* bytes=11 first=0x743a0940/ {
        candidate = NR
    }
    END { if (candidate) print candidate }
' "$RUNTIME_DIR/radio.trace")
scan_response_line=$(awk -v start="${scan_request_line:-0}" '
    NR > start && /endpoint=0 TX snapshot sequence=.* bytes=8 first=0x743a0644/ {
        print NR
        exit
    }
' "$RUNTIME_DIR/radio.trace")

if [ -z "$advertisement_line" ] || [ -z "$scan_request_line" ] || \
   [ -z "$scan_response_line" ] || \
   [ "$advertisement_line" -ge "$scan_request_line" ] || \
   [ "$scan_request_line" -ge "$scan_response_line" ]; then
    echo "active-scan-e2e: missing ordered advertisement -> SCAN_REQ -> SCAN_RSP exchange" >&2
    grep -E 'RAC.CMD=|RAC state|TX snapshot' "$RUNTIME_DIR/radio.trace" >&2 || true
    exit 1
fi

echo "active-scan-e2e: observed QEMU-only advertisement -> SCAN_REQ -> SCAN_RSP"
