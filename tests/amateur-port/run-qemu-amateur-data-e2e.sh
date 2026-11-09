#!/usr/bin/env bash
# The data identification (BLE-ASI v1, docs/ble-amateur-station-id.md; docs/amateur-radio.md
# section 5) end to end in the QEMU dual machine. Machine 0 transmits under amateur rules on the
# odd-MHz plan (2401-2447 MHz, no Bluetooth carrier) with CONFIG_EFR32_RADIO_AMATEUR_ID_DATA and
# the CW identification off, its application advertising "owo-app"; machine 1 receives on the
# same plan and parses BLE-ASI itself (tests/amateur-port/src/main.c, written from the document).
#
# MODE=short (default): the advertiser (build/am-adv-data: prj.conf, prj-advertiser.conf,
#   prj-dataid.conf, ADV_SECONDS=8) stops, and efr32_amateur_disable() sends the closing
#   identification. Checks: the receiver gets the call sign from BLE-ASI frames, FINAL (header
#   0x11) on the closing one; the application's frames carry only "owo-app", never the call sign;
#   the identification's address is not the application's; every carrier is a plan carrier; no
#   OOK (CW) frame; one identification, then Part 15; no safe shutdown.
# MODE=periodic: the advertiser never stops (build/am-adv-data-long, ADV_SECONDS=0). Checks: the
#   identifications end at most 600 s apart (97.119(a)) and each is received as BLE-ASI with
#   FINAL clear (header 0x10).
# MODE=connection: machine 0 is a connectable Peripheral (build/am-per-data: prj.conf,
#   prj-peripheral.conf, prj-dataid.conf), machine 1 a Central (build/am-cen-data: prj.conf,
#   prj-central.conf, prj-dataid.conf) that connects to "owo-app" and scans while connected.
#   Checks: the connection holds for the whole run; the Peripheral's periodic identifications
#   (and the Central's own) are sent during it and the Central receives the Peripheral's
#   BLE-ASI frames while connected; no safe shutdown.
# Run inside the dev shell (python3).
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MODE=${MODE:-short}
case "$MODE" in
short) TX_DEF=am-adv-data; RX_DEF=am-scan-qemu; RUN_DEF=40;;
periodic) TX_DEF=am-adv-data-long; RX_DEF=am-scan-qemu; RUN_DEF=600;;
connection) TX_DEF=am-per-data; RX_DEF=am-cen-data; RUN_DEF=900;;
*) echo "MODE: short, periodic or connection" >&2; exit 2;;
esac
TX_IMAGE=${TX_IMAGE:-"$ROOT/build/$TX_DEF/zephyr/zephyr.bin"}
RX_IMAGE=${RX_IMAGE:-"$ROOT/build/$RX_DEF/zephyr/zephyr.bin"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
RUN_SECONDS=${1:-$RUN_DEF}
CALLSIGN=${CALLSIGN:-KD3APL}
APP_NAME=${APP_NAME:-owo-app}
ICOUNT_SHIFT=${ICOUNT_SHIFT:-3}
OUT=${OUT:-"$ROOT/build/amateur-data-$MODE"}
TX_ELF=${TX_ELF:-"${TX_IMAGE%.bin}.elf"}
RX_ELF=${RX_ELF:-"${RX_IMAGE%.bin}.elf"}
RUNTIME_DIR=$OUT
. "$ROOT/tests/radio-rx-port/qemu-miss-count.sh"

for f in "$QEMU_BIN" "$TX_IMAGE" "$RX_IMAGE"; do
    if [ ! -e "$f" ]; then
        echo "missing $f" >&2
        exit 2
    fi
done
# CHECK_ONLY=1: check an earlier run's output in $OUT again without running QEMU.
if [ "${CHECK_ONLY:-0}" != 1 ]; then
rm -rf "$OUT"
mkdir -p "$OUT"
# Every radio register write is traced (the carrier check needs SYNTH.FREQ): keep only the
# lines checked below, or a connection run writes gigabytes.
mkfifo "$OUT/trace.fifo"
grep -a --line-buffered -E 'TX snapshot sequence|OOK TX|addr=0x(a|b)8018034 ' \
    <"$OUT/trace.fifo" >"$OUT/radio.trace" &
FILTER=$!

QEMU_BIN=$(realpath "$QEMU_BIN")
TX_IMAGE=$(realpath "$TX_IMAGE")
RX_IMAGE=$(realpath "$RX_IMAGE")
# shellcheck disable=SC2086 # MISS_QMP_ARGS is two words
cd "$OUT" && exec "$QEMU_BIN" \
    -M "efr32bg22-dual,flash0=$TX_IMAGE,flash1=$RX_IMAGE" \
    -display none -monitor none $MISS_QMP_ARGS -icount "shift=$ICOUNT_SHIFT,sleep=off" \
    -serial null -serial "file:$OUT/console0.raw" -serial null -serial "file:$OUT/console1.raw" \
    -trace enable=efr32bg22_tx_snapshot \
    -trace enable=efr32bg22_ook_tx \
    -trace enable=efr32bg22_endpoint_radio_write \
    -trace file="$OUT/trace.fifo" >"$OUT/qemu.log" 2>&1 &
qemu_pid=$!
sleep "$RUN_SECONDS"
miss_count_end "$qemu_pid" "$TX_ELF" "$RX_ELF"
wait "$qemu_pid" || true
wait "$FILTER" || true
rm -f "$OUT/trace.fifo"
fi
# Every guest that counts deadline misses must have none (qemu-miss-count.sh).
miss_status=0
miss_count_check amateur-data-e2e || miss_status=1

for ep in 0 1; do
    tr -d '\r' <"$OUT/console$ep.raw" >"$OUT/console$ep.txt" 2>/dev/null || : >"$OUT/console$ep.txt"
done
echo "--- machine 0"; grep -aE 'amateur|adv_|peripheral|central|bt_enable|SAFE|efr32-radio' "$OUT/console0.txt" | head -40 || true
echo "--- machine 1"; grep -aE 'amateur|scan_|peripheral|central|bt_enable|SAFE|efr32-radio' "$OUT/console1.txt" | head -40 || true

python3 - "$OUT" "$MODE" "$CALLSIGN" "$APP_NAME" "${EXPECT_CW:-0}" <<'EOF'
import re, sys
out, mode, call, app = sys.argv[1:5]
expect_cw = sys.argv[5] == "1"
ID_RE = r"identification \d+ \(\S+, (?:data|data\+CW)\) ended at (\d+) ms"
HFXO = 38400000
INTERVAL_MS = 600000  # 47 CFR 97.119(a)
c0 = open(out + "/console0.txt", errors="replace").read()
c1 = open(out + "/console1.txt", errors="replace").read()
fail = []

def check(cond, what):
    print(("ok:   " if cond else "FAIL: ") + what)
    if not cond:
        fail.append(what)

check("SAFE SHUTDOWN" not in c0 + c1, "no safe shutdown")
check(re.search(r"^amateur: enable 0", c0, re.M) is not None, "machine 0 under amateur rules")
check(re.search(r"^amateur: enable 0", c1, re.M) is not None, "machine 1 under amateur rules")
check("data identification (BLE-ASI v1" in c0, "machine 0 identifies by BLE-ASI")
if mode != "connection":
    check("amateur-selftest: PASS" in c0, "on-target API checks")

asi = re.findall(r"^rx_asi: (\S*) final=(\d) header=0x([0-9a-f]+) addr=(\S+) type=(\d+) (\S+) "
                 r"at (\d+) ms", c1, re.M)
names = re.findall(r"^rx_name: (.*) addr=(\S+) type=(\d+)$", c1, re.M)
id_addrs = {a for _, _, _, a, _, _, _ in asi}
app_addrs = {a for n, a, _ in names if n == app}
print(f"BLE-ASI frames received: {len(asi)} from {sorted(id_addrs)}; application frames: "
      f"{sum(1 for n, _, _ in names if n == app)} from {sorted(app_addrs)}")
check(len(asi) > 0 and all(c == call and ok == "ok" and t == "3" for c, _, _, _, t, ok, _ in asi),
      f"every BLE-ASI frame carries {call}, well formed, as ADV_NONCONN_IND (type 3)")
check(all(h in ("10", "11") and int(f) == (h == "11") for _, f, h, _, _, _, _ in asi),
      "headers are 0x10 (periodic) or 0x11 (FINAL)")
check(len(app_addrs) > 0, f"the application's advertising ({app}) received")
check(not any(n == call for n, _, _ in names), "the application's data never carries the call sign")
check(not (id_addrs & app_addrs), "the identification's own address is not the application's")
ook = [l for l in open(out + "/radio.trace", errors="replace") if "OOK TX" in l and "endpoint=0" in l]
method = "data+CW" if expect_cw else "data"
n_ids = len(re.findall(r"identification \d+ \(\S+, %s\) ended" % re.escape(method), c0))
if expect_cw:
    # EFR32_RADIO_AMATEUR_ID_CW too: one Morse frame after the data frames of each.
    check(len(ook) == n_ids and n_ids > 0, f"one CW (OOK) frame per identification: {len(ook)}")
else:
    check(not ook, "no CW (OOK) frame: data identification only")
# The identification PDUs machine 0 sent: header and length octets, AdvA, the BLE-ASI structure
# (19 + N octets, BLE-ASI 3). One per advertising channel per identification.
id_bytes = 2 + 6 + 19 + len(call)
snaps = re.findall(r"endpoint=0 TX snapshot sequence=\d+ bytes=(\d+) first=0x([0-9a-f]{8})",
                   open(out + "/radio.trace", errors="replace").read())
id_pdus = [f for b, f in snaps if int(b) == id_bytes]
n_ids0 = len(re.findall(ID_RE, c0))
check(len(id_pdus) == 3 * n_ids0 and all(f[-2:] == "42" and f[-4:-2] == "%02x" % (id_bytes - 2)
                                         for f in id_pdus),
      f"machine 0 sent {len(id_pdus)} identification PDUs (ADV_NONCONN_IND, TxAdd, "
      f"length {id_bytes - 2}) for {n_ids0} identifications: three each")

seen = {}
for line in open(out + "/radio.trace", errors="replace"):
    m = re.search(r"endpoint=0 addr=0x(?:a|b)8018034 value=0x([0-9a-f]+)", line)
    if m:
        khz = round(int(m.group(1), 16) * HFXO / 2**19 / 1000)
        seen[khz] = seen.get(khz, 0) + 1
print("machine 0 carriers (kHz: writes):", dict(sorted(seen.items())))
check(seen and not [k for k in seen if (k // 1000) % 2 == 0 and k % 1000 == 0],
      "machine 0 used plan carriers only (odd MHz)")

ends = [int(m) for m in re.findall(ID_RE, c0)]
print("machine 0 identification ends (ms):", ends)
prev = 0
for e in ends:
    check(e - prev <= INTERVAL_MS, f"identification at {e} ms within 600 s of the previous ({prev})")
    prev = e

if mode == "short":
    check(any(f == "1" for _, f, _, _, _, _, _ in asi), "the closing identification is FINAL (0x11)")
    check(re.search(r"^amateur: disable 0, identifications 1, active 0", c0, re.M) is not None,
          "one closing identification, then Part 15")
elif mode == "periodic":
    check(len(ends) >= 2, "at least two periodic identifications")
    check(not any(f == "1" for _, f, _, _, _, _, _ in asi), "periodic identifications not FINAL")
    rx_ms = sorted({int(t) // 1000 for *_, t in asi})
    print("BLE-ASI received at (s, machine 1 clock):", rx_ms)
else:
    con0 = re.search(r"^peripheral: connected 0 at (\d+) ms", c0, re.M)
    con1 = re.search(r"^central: connected 0 at (\d+) ms", c1, re.M)
    check(con0 is not None and con1 is not None, "connected")
    check("disconnected" not in c0 + c1, "the connection held for the whole run")
    if con0:
        during = [e for e in ends if e > int(con0.group(1))]
        check(len(during) >= 1, f"Peripheral identified during the connection: {during}")
    if con1:
        rx_during = [int(t) for *_, t in asi if int(t) > int(con1.group(1))]
        check(len(rx_during) >= 1,
              f"Central received the Peripheral's BLE-ASI while connected: {len(rx_during)} frames")
    ends1 = [int(m) for m in re.findall(ID_RE, c1)]
    print("machine 1 (Central) identification ends (ms):", ends1)
print("amateur-data-e2e (%s): %s" % (mode, "FAIL" if fail else "PASS"))
sys.exit(1 if fail else 0)
EOF
exit "$miss_status"
