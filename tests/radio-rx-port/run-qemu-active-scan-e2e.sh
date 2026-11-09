#!/usr/bin/env bash
# Exercise the active-scan exchange in QEMU: the radio-port advertiser and the scanner built
# with prj-active-scan.conf, both on the silicon path (no QEMU-only seam). A passing run requires
# the full advertisement -> SCAN_REQ -> SCAN_RSP sequence, each reply sent by the sequencer at
# T_IFS from RX2TX, and no deadline miss on either endpoint.
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
# shift=3 charges 8 ns per instruction; the two endpoints share that, so each
# runs at roughly 60 MIPS, close to the 76.8 MHz Cortex-M33. Slower settings
# (shift=5 is about 15 MIPS per endpoint) make the firmware miss deadlines its
# scheduler clock would meet on silicon, and the miss check panics.
ICOUNT_SHIFT=${ICOUNT_SHIFT:-3}
# sleep=off: idle time jumps straight to the next deadline instead of tracking
# host time, so a run's virtual timeline does not depend on host scheduling.
TX_IMAGE="$TX_BUILD_DIR/zephyr/zephyr.bin"
RX_IMAGE="$RX_BUILD_DIR/zephyr/zephyr.bin"
TX_ELF=${TX_ELF:-"$TX_BUILD_DIR/zephyr/zephyr.elf"}
RX_ELF=${RX_ELF:-"$RX_BUILD_DIR/zephyr/zephyr.elf"}

if [ ! -x "$QEMU_BIN" ] || [ ! -f "$TX_IMAGE" ] || [ ! -f "$RX_IMAGE" ] ||
   [ ! -f "$TX_ELF" ] || [ ! -f "$RX_ELF" ]; then
    echo "active-scan-e2e: missing QEMU binary, or an advertiser or active scanner image or ELF" >&2
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
. "$ROOT/tests/radio-rx-port/qemu-miss-count.sh"
QEMU_BIN=$(realpath "$QEMU_BIN")
TX_IMAGE=$(realpath "$TX_IMAGE")
RX_IMAGE=$(realpath "$RX_IMAGE")

# shellcheck disable=SC2086 # MISS_QMP_ARGS is two words
cd "$RUNTIME_DIR" && exec "$QEMU_BIN" \
    -M "efr32bg22-dual,flash0=$TX_IMAGE,flash1=$RX_IMAGE" \
    -display none -monitor none $MISS_QMP_ARGS \
    -icount "shift=$ICOUNT_SHIFT,sleep=off" -d guest_errors \
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
    -trace enable=efr32bg22_timer0_event \
    -trace file="$RUNTIME_DIR/radio.trace" \
    >"$RUNTIME_DIR/guest.log" 2>&1 &
qemu_pid=$!
(
    sleep "$RUN_SECONDS"
    miss_count_end "$qemu_pid" "$TX_ELF" "$RX_ELF"
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
# TRACE_OUT: keep the run's radio trace there for further analysis.
if [ -n "${TRACE_OUT:-}" ]; then
    cp "$RUNTIME_DIR/radio.trace" "$TRACE_OUT"
fi
# Every guest that counts deadline misses must have none (qemu-miss-count.sh).
miss_count_check active-scan-e2e

console=$(sed -n 's/.*USART[01] TX=0x\([[:xdigit:]][[:xdigit:]]\).*/\1/p' \
    "$RUNTIME_DIR/radio.trace" |
    LC_ALL=C awk 'BEGIN { for (i = 0; i < 256; i++) byte[sprintf("%02x", i)] = i }
                  { printf "%c", byte[tolower($1)] }' | tr -d '\r')
printf '%s\n' "$console" | grep -E '^(bt_enable|scan_callback_register|scan_start):' || true
# Guest-console progress is informative only: ICOUNT's host pacing can delay
# printk after the controller has already issued its RF request. The
# endpoint-tagged TX snapshots below are the authoritative active-scan proof.

# Each machine has a private address space, so its MMIO windows do not tag a
# sender. The QEMU model does: endpoint 0 is the advertiser and endpoint 1 is
# the scanner. Startup is deliberately unconstrained, but each accepted cycle
# must be ordered advertisement -> SCAN_REQ -> SCAN_RSP. Both devices take a
# new random address at every boot, so the first word (QEMU little-endian) is
# matched on the header only: the advertisement is 11 bytes (`40 09`), the
# scanner request 14 bytes (`c3 0c`) and the response 26 bytes (`44 18`, the
# staged scan response since 87fa854), whose AdvA must start as the
# advertisement's. These are QEMU snapshot boundaries, not real RF airtime or
# hardware FRC/PROTIMER assertions.
#
# A later advertisement supersedes an unmatched one. This avoids counting an
# old startup advertisement with a request belonging to a newer advertising
# event while still allowing the two CPUs to start in either order.
exchange_summary=$(awk '
    function adva(line) {
        match(line, /first=0x[[:xdigit:]]+/)
        return substr(line, RSTART + 8, 4)
    }
    /endpoint=0 TX snapshot sequence=.* bytes=11 first=0x[[:xdigit:]]+0940$/ {
        advertisement = NR
        advertiser = adva($0)
        state = 1
        next
    }
    state == 1 && /endpoint=1 TX snapshot sequence=.* bytes=14 first=0x[[:xdigit:]]+0cc3$/ {
        request = NR
        state = 2
        next
    }
    state == 2 && /endpoint=0 TX snapshot sequence=.* bytes=26 first=0x[[:xdigit:]]+1844$/ &&
    adva($0) == advertiser {
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

# The packet order above does not say how each reply was sent. On the silicon path the
# sequencer turns around at T_IFS without the M33 (docs/radio-tables.md 7.82, 7.87): the
# scanner's SCAN_REQ and the advertiser's SCAN_RSP each leave RXFRAME through RX2TX into TX
# (RAC 3 -> 7 -> 9), never a software TXEN from OFF (0 -> 8). For every SCAN_REQ and SCAN_RSP,
# require the sender's last RAC transitions before its snapshot to be 3 -> 7 and 7 -> 9, and
# report the time from the received frame's handoff to that TX entry (QEMU model timing, not an
# RF T_IFS claim).
turn_summary=$(awk '
    function now_ns(line, value) {
        value = line
        sub(/^.*now=/, "", value)
        sub(/[^0-9].*$/, "", value)
        return value + 0
    }
    function note(t) {
        count++
        if (!min_turn || t < min_turn) {
            min_turn = t
        }
        if (t > max_turn) {
            max_turn = t
        }
    }
    /endpoint=[01] now=[0-9]* RF RX handoff/ {
        ep = substr($2, 10, 1)
        handoff[ep] = now_ns($0)
        next
    }
    /endpoint=[01] now=[0-9]* RAC state / {
        ep = substr($2, 10, 1)
        prev[ep] = last[ep]
        last[ep] = $(NF - 2) " -> " $NF
        at[ep] = now_ns($0)
        next
    }
    /endpoint=1 TX snapshot sequence=.* bytes=14 first=0x[[:xdigit:]]+0cc3$/ ||
    /endpoint=0 TX snapshot sequence=.* bytes=26 first=0x[[:xdigit:]]+1844$/ {
        ep = substr($2, 10, 1)
        if (prev[ep] != "3 -> 7" || last[ep] != "7 -> 9") {
            bad++
            next
        }
        note(at[ep] - handoff[ep])
    }
    END {
        if (!count || bad) {
            printf "%d:%d\n", count, bad
            exit 1
        }
        printf "%d:%d:%d\n", count, min_turn, max_turn
    }
' "$RUNTIME_DIR/radio.trace") || turn_failed=1
if [ -n "${turn_failed:-}" ]; then
    echo "active-scan-e2e: a SCAN_REQ or SCAN_RSP did not leave RXFRAME through RX2TX (replies:bad $turn_summary)" >&2
    exit 1
fi
turn_count=${turn_summary%%:*}
turn_range=${turn_summary#*:}
echo "active-scan-e2e: $turn_count replies sent by the sequencer from RX2TX, RX handoff -> TX entry ${turn_range%%:*}..${turn_range#*:} ns virtual (QEMU model timing, not an RF T_IFS claim)"
echo "active-scan-e2e: observed $exchange_count ordered advertisement -> SCAN_REQ -> SCAN_RSP exchanges"
