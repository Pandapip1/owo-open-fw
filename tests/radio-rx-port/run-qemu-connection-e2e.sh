#!/usr/bin/env bash
# Establish an LE connection between the QEMU advertiser (peripheral) and the
# QEMU central, then prove the connection-establishment exchange reached the
# host on both endpoints. This is the connection data path of the handoff's
# radio-data-path step, distinct from the advertising-only active-scan e2e:
# a passing run requires CONNECT_IND delivered to the advertiser and Zephyr's
# host reporting an established link, not merely an advertisement.
#
# Build prerequisites, from the repository root:
#   nix develop . -c cmake -G Ninja -S tests/radio-port -B build/radio-port-conn-qemu \
#     -DBOARD=bg22_ek4108a -DZEPHYR_BASE="$PWD/vendor/zephyr" \
#     -DZephyr_DIR="$PWD/vendor/zephyr/share/zephyr-package/cmake" \
#     -DZEPHYR_MODULES="$PWD/vendor/cmsis_6;$PWD/vendor/hal_silabs;$PWD/vendor/mbedtls;$PWD/vendor/tf-psa-crypto;$PWD/vendor/zephyr-lang-rust" \
#     -DEXTRA_CONF_FILE=prj-connection.conf \
#     -DDTC_OVERLAY_FILE="$PWD/tests/radio-port/boards/bg22_ek4108a.overlay"
#   nix develop . -c cmake --build build/radio-port-conn-qemu
#   (same pattern for build/radio-rx-port-central-qemu with tests/radio-rx-port
#    and -DEXTRA_CONF_FILE=prj-central.conf)
#
# Both profiles run the hardware configuration (the silicon path; both QEMU-only
# seams off). What this proves: the central receives ADV_IND and answers with a
# 36-byte CONNECT_IND at T_IFS that the RF medium delivers to the advertiser,
# the central's host connected callback fires with HCI error 0, and the link is
# sustained: the advertiser answers connection events on the data channels for
# the whole run and the central reports no disconnection. The Peripheral tracks
# the Central's anchor from the received frames' end, dated from RAC.STIMER
# (docs/radio-tables.md 7.79). What it does not prove: spec timing in QEMU. The
# model runs lll_conn's turnaround callbacks past their spec-derived start-by
# deadlines, so these QEMU-only profiles count misses instead of shutting the
# radio down (7.79 attributes every one); nor anything about silicon.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TX_BUILD_DIR=${TX_BUILD_DIR:-"$ROOT/build/radio-port-conn-qemu"}
RX_BUILD_DIR=${RX_BUILD_DIR:-"$ROOT/build/radio-rx-port-central-qemu"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
RUN_SECONDS=${1:-8}
ICOUNT_SHIFT=${ICOUNT_SHIFT:-3}
TX_IMAGE="$TX_BUILD_DIR/zephyr/zephyr.bin"
RX_IMAGE="$RX_BUILD_DIR/zephyr/zephyr.bin"

if [ ! -x "$QEMU_BIN" ] || [ ! -f "$TX_IMAGE" ] || [ ! -f "$RX_IMAGE" ]; then
	echo "connection-e2e: missing QEMU binary, advertiser image, or central image" >&2
	exit 2
fi

RUNTIME_DIR=$(mktemp -d "$ROOT/build/radio-connection-e2e-runtime.XXXXXX")
trap 'rm -rf "$RUNTIME_DIR"' EXIT HUP INT TERM

"$QEMU_BIN" \
	-M "efr32bg22-dual,flash0=$TX_IMAGE,flash1=$RX_IMAGE${MACHINE_OPTS:+,$MACHINE_OPTS}" \
	-display none -monitor none -icount "shift=$ICOUNT_SHIFT,sleep=off" -d guest_errors \
	-D "$RUNTIME_DIR/qemu.log" \
	-chardev file,id=soc0.usart1,path="$RUNTIME_DIR/console0" \
	-chardev file,id=soc1.usart1,path="$RUNTIME_DIR/console1" \
	-trace enable=efr32bg22_tx_snapshot \
	-trace enable=efr32bg22_rf_rx_handoff \
	-trace enable=efr32bg22_rac_state \
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

# Each endpoint's console (USART1, the boards' zephyr,console) goes to its own file, so the
# two guests' lines cannot interleave.
console=$(cat "$RUNTIME_DIR/console0" "$RUNTIME_DIR/console1" 2>/dev/null | tr -d '\r\000')
printf '%s\n' "$console" | grep -aE '^(bt_enable|scan_start|scan_stop|conn_le_create|adv_start|central_connected|gatt_discover|gatt_rw|set_security|central_[a-z_]*|peripheral_[a-z_]*):' || true

# A fatal error on either endpoint (stack overflow, deadline panic, assert) is
# a failure regardless of how far the exchange got.
if printf '%s\n' "$console" | grep -q 'FATAL ERROR'; then
	echo "connection-e2e: a guest hit a fatal error during establishment" >&2
	printf '%s\n' "$console" | grep -aE 'FATAL ERROR|Stack overflow|ran .* late' >&2
	exit 1
fi

# 1. The central must have issued bt_conn_le_create with no host-side error.
if ! printf '%s\n' "$console" | grep -aq 'conn_le_create: 0'; then
	echo "connection-e2e: central did not accept the connection-create request" >&2
	exit 1
fi

# 2. The central's host connected callback fired with HCI error 0: the
#    controller delivered a Connection Complete for the established link.
if ! printf '%s\n' "$console" | grep -aq 'central_connected: 0'; then
	echo "connection-e2e: central did not report an established connection (err 0)" >&2
	exit 1
fi

# 3. The CONNECT_IND reached the advertiser through the RF medium. Endpoint 1
#    is the central, endpoint 0 the advertiser; a 36-byte legacy PDU is the
#    ADV_IND size, and the 36-byte handoff into endpoint 0 is the CONNECT_IND
#    (the ADV_IND the central received is also 36 bytes, so require the
#    advertiser-side handoff specifically).
if ! grep -q 'endpoint=0 now=[0-9]* RF RX handoff bytes=36' "$RUNTIME_DIR/radio.trace"; then
	echo "connection-e2e: CONNECT_IND was not delivered to the advertiser" >&2
	exit 1
fi

# 4. The advertiser switched from advertising to the connection: after the
#    CONNECT_IND handoff it must program the SYNTH to a data channel (a
#    channel number outside the 37/38/39 advertising set, i.e. not 0, 12, 39).
#    Require at least one post-handoff RAC RX arm on endpoint 0: the
#    peripheral armed for the first connection event.
post_handoff_rx=$(awk '
	/endpoint=0 now=[0-9]* RF RX handoff bytes=36/ { handed = NR; next }
	handed && NR > handed && /endpoint=0.*RAC state 1 -> 2/ { count++ }
	END { print count + 0 }
' "$RUNTIME_DIR/radio.trace")
if [ "$post_handoff_rx" -lt 1 ]; then
	echo "connection-e2e: advertiser never armed RX for a connection event after CONNECT_IND" >&2
	exit 1
fi

# 5. The link is sustained: the advertiser answers connection events (its
#    data-channel PDUs after the handoff, 2-octet empty PDUs and longer), at
#    least one per second of the run at the central's longest interval (50 ms
#    allows 20), and the central's host never reports a disconnection.
answers=$(awk '
	/endpoint=0 now=[0-9]* RF RX handoff bytes=36/ { handed = 1; next }
	handed && /endpoint=0 TX snapshot/ && !/bytes=11 / { count++ }
	END { print count + 0 }
' "$RUNTIME_DIR/radio.trace")
if [ "$answers" -lt "$RUN_SECONDS" ]; then
	echo "connection-e2e: advertiser answered only $answers connection events" >&2
	exit 1
fi
if printf '%s\n' "$console" | grep -aq 'central_disconnected'; then
	echo "connection-e2e: the central lost the connection" >&2
	printf '%s\n' "$console" | grep -a 'central_disconnected' >&2
	exit 1
fi

# 5b. With the pairing profiles (PAIRING=1: prj-pairing.conf and prj-central-pairing.conf, or a
#     product image as the Peripheral) the link reached security level 4 (LE Secure Connections,
#     authenticated by passkey entry) before the GATT rounds, so every round below ran over the
#     encrypted link: the test characteristic (and the product's) refuses access otherwise.
if [ "${PAIRING:-0}" = 1 ] &&
   ! printf '%s\n' "$console" | grep -aq 'central_security: level 4 err 0'; then
	echo "connection-e2e: the link did not reach LE Secure Connections security level 4" >&2
	printf '%s\n' "$console" | grep -aE 'central_(security|pair|passkey)|peripheral_pair' >&2 || true
	exit 1
fi

# 6. GATT over the link: the central found the advertiser's test characteristic and
#    every write-then-read-back round it ran matched (src/central.c reports every 30).
if ! printf '%s\n' "$console" | grep -aq 'gatt_rw: [0-9]* rounds, [0-9]* ok, 0 failed' ||
   printf '%s\n' "$console" | grep -aq 'gatt_rw: round .* failed\|gatt_discover: failed'; then
	echo "connection-e2e: GATT write/read-back over the link did not succeed" >&2
	printf '%s\n' "$console" | grep -a 'gatt_' >&2 || true
	exit 1
fi

echo "connection-e2e: QEMU connection established and sustained (CONNECT_IND delivered, central err 0, $post_handoff_rx Peripheral windows, $answers answers, no disconnection)"
echo "connection-e2e: not a timing claim: the profiles count deadline misses (docs/radio-tables.md 7.79)"
