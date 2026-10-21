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
# A single exchange demonstrates the wiring, but it can also hide stale state
# from startup. Require several independently ordered exchanges by default.
# Callers can raise this for soak runs without extending the test contract.
MIN_EXCHANGES=${MIN_EXCHANGES:-3}
# The QEMU TIFS worker's measured 150-us interval needs shift=5; shift=1
# stretches it to about 1.5 ms and lets the LLL clean up before TXEN.
ICOUNT_SHIFT=${ICOUNT_SHIFT:-5}
# This is a QEMU scheduler-observability bound, not a silicon TIFS claim.
# It bounds QEMU virtual time from IRQ18 assertion to the persistent job's
# RAC RXSEARCH transition: three 32.768-kHz cycles, rounded up.
MAX_TIFS_JOB_HANDOFF_NS=${MAX_TIFS_JOB_HANDOFF_NS:-100000}
TX_IMAGE="$TX_BUILD_DIR/zephyr/zephyr.bin"
RX_IMAGE="$RX_BUILD_DIR/zephyr/zephyr.bin"

if [ ! -x "$QEMU_BIN" ] || [ ! -f "$TX_IMAGE" ] || [ ! -f "$RX_IMAGE" ]; then
    echo "active-scan-e2e: missing QEMU binary, advertiser image, or active scanner image" >&2
    exit 2
fi

case "$MIN_EXCHANGES" in
    ''|*[!0-9]*|0)
        echo "active-scan-e2e: MIN_EXCHANGES must be a positive integer" >&2
        exit 2
        ;;
esac

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
    -trace enable=efr32bg22_rf_rx_handoff \
    -trace enable=efr32bg22_tx_snapshot \
    -trace enable=efr32bg22_tx_offer_count \
    -trace enable=efr32bg22_burtc_compare_program \
    -trace enable=efr32bg22_burtc_compare_expire \
    -trace enable=efr32bg22_burtc_irq \
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
# the scanner. Startup is deliberately unconstrained, but each accepted cycle
# must be ordered advertisement -> SCAN_REQ -> SCAN_RSP. The scanner request
# is 14 bytes (`c3 0c`, QEMU little-endian word `0x1a1a0cc3`) and the response
# is eight bytes (`44 06`, word `0x743a0644`). These are QEMU snapshot
# boundaries, not real RF airtime or hardware FRC/PROTIMER assertions.
#
# A later advertisement supersedes an unmatched one. This avoids counting an
# old startup advertisement with a request belonging to a newer advertising
# event while still allowing the two CPUs to start in either order.
exchange_summary=$(awk '
    /endpoint=0 TX snapshot sequence=.* bytes=11 first=0x743a0940/ {
        advertisement = NR
        state = 1
        next
    }
    state == 1 && /endpoint=1 TX snapshot sequence=.* bytes=14 first=0x1a1a0cc3/ {
        request = NR
        state = 2
        next
    }
    state == 2 && /endpoint=0 TX snapshot sequence=.* bytes=8 first=0x743a0644/ {
        count++
        printf "%d:%d:%d\n", advertisement, request, NR
        state = 0
    }
    END {
        if (!count) {
            exit 1
        }
    }
' "$RUNTIME_DIR/radio.trace") || exchange_summary=
exchange_count=$(printf '%s\n' "$exchange_summary" | sed '/^$/d' | wc -l | tr -d ' ')

if [ "$exchange_count" -lt "$MIN_EXCHANGES" ]; then
    echo "active-scan-e2e: observed $exchange_count ordered exchange(s); need $MIN_EXCHANGES" >&2
    grep -E 'RAC.CMD=|RAC state|TX snapshot' "$RUNTIME_DIR/radio.trace" >&2 || true
    exit 1
fi

# The active-scan success criterion above proves the packet sequence, but it
# does not say whether it was reached by running radio work in IRQ context.
# For every accepted advertising turn below, require the model-side chain:
# a short CC0 arm after TX retires, COMP expiry, IRQ18 assertion, then the
# advertiser's RAC RXSEARCH transition.  The RAC transition is a guest MMIO
# write and therefore occurs from the persistent TIFS job, not from QEMU's
# BURTC callback.  The elapsed virtual time is reported and bounded only as a
# QEMU scheduling handoff diagnostic.  It is not RF timing or a claim about
# the hardware PROTIMER/RAC sequencer.
tifs_summary=$(awk -v max_handoff_ns="$MAX_TIFS_JOB_HANDOFF_NS" '
    function now_ns(line, value) {
        value = line
        sub(/^.*now=/, "", value)
        sub(/[^0-9].*$/, "", value)
        return value + 0
    }
    /endpoint=0 TX snapshot sequence=.* bytes=11 first=0x743a0940/ {
        stage = 1
        next
    }
    stage == 1 && /endpoint=0 .*RAC state 9 -> 0/ {
        stage = 2
        next
    }
    stage == 2 && /endpoint=0 .*BURTC compare program .* ticks=[1-7]$/ {
        stage = 3
        next
    }
    stage == 3 && /endpoint=0 .*BURTC compare expired/ {
        stage = 4
        next
    }
    stage == 4 && /endpoint=0 .*BURTC IRQ18 level=1 / {
        irq_now = now_ns($0)
        stage = 5
        next
    }
    stage == 5 && /endpoint=0 .*RAC state 0 -> 2/ {
        handoff = now_ns($0) - irq_now
        if (handoff < 0 || handoff > max_handoff_ns) {
            exit 2
        }
        stage = 6
        next
    }
    stage == 6 && /endpoint=1 TX snapshot sequence=.* bytes=14 first=0x1a1a0cc3/ {
        count++
        if (handoff > max_seen) {
            max_seen = handoff
        }
        stage = 0
    }
    END {
        if (!count) {
            exit 1
        }
        printf "%d:%d\n", count, max_seen
    }
' "$RUNTIME_DIR/radio.trace") || tifs_summary=
tifs_count=${tifs_summary%%:*}
tifs_max_handoff_ns=${tifs_summary#*:}
if [ -z "$tifs_summary" ] || [ "$tifs_count" -lt "$MIN_EXCHANGES" ]; then
    echo "active-scan-e2e: missing causal BURTC IRQ18 -> persistent TIFS-job RX transition" >&2
    grep -E 'BURTC compare|BURTC IRQ18|RAC state|TX snapshot' "$RUNTIME_DIR/radio.trace" >&2 || true
    exit 1
fi
echo "active-scan-e2e: $tifs_count BURTC IRQ18 -> TIFS-job RX handoff(s), max ${tifs_max_handoff_ns} ns virtual"

echo "active-scan-e2e: observed $exchange_count QEMU-only ordered advertisement -> SCAN_REQ -> SCAN_RSP exchanges"
