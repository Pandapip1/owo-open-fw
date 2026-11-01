#!/usr/bin/env bash
# Amateur operation end to end in the QEMU dual machine (docs/amateur-radio.md):
# an advertiser and a scanner, both under amateur rules on the same non-standard plan
# (odd MHz carriers), the call sign in the advertising data. Checks:
#  - the advertiser's on-target API checks pass;
#  - the scanner receives the call sign on the amateur plan;
#  - every advertiser frame went out on a plan carrier, none on a Bluetooth carrier;
#  - after AMATEUR_PORT_ADV_SECONDS the advertiser stops, returns to Part 15, and the closing
#    identification is the call sign in Morse code (OOK) at 20 WPM PARIS timing.
# Images: build/am-adv-qemu (prj.conf;prj-advertiser.conf, -DCONFIG_AMATEUR_PORT_ADV_SECONDS=N)
# and build/am-scan-qemu (prj.conf;prj-scanner.conf).
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TX_IMAGE=${TX_IMAGE:-"$ROOT/build/am-adv-qemu/zephyr/zephyr.bin"}
RX_IMAGE=${RX_IMAGE:-"$ROOT/build/am-scan-qemu/zephyr/zephyr.bin"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
RUN_SECONDS=${1:-40}
CALLSIGN=${CALLSIGN:-KD3APL}
ICOUNT_SHIFT=${ICOUNT_SHIFT:-3}
OUT=${OUT:-"$ROOT/build/amateur-e2e"}

for f in "$QEMU_BIN" "$TX_IMAGE" "$RX_IMAGE"; do
    if [ ! -e "$f" ]; then
        echo "missing $f" >&2
        exit 2
    fi
done
rm -rf "$OUT"
mkdir -p "$OUT"

timeout "$RUN_SECONDS" "$QEMU_BIN" \
    -M "efr32bg22-dual,flash0=$TX_IMAGE,flash1=$RX_IMAGE" \
    -display none -monitor none -icount "shift=$ICOUNT_SHIFT,sleep=off" \
    -serial null -serial "file:$OUT/console0.raw" -serial null -serial "file:$OUT/console1.raw" \
    -trace enable=efr32bg22_tx_snapshot \
    -trace enable=efr32bg22_ook_tx \
    -trace enable=efr32bg22_endpoint_radio_write \
    -trace file="$OUT/radio.trace" >"$OUT/qemu.log" 2>&1 || true

# Console of each machine (USART1; serial ports 0-1 are machine 0's USART0-1, 2-3 machine 1's).
for ep in 0 1; do
    tr -d '\r' <"$OUT/console$ep.raw" >"$OUT/console$ep.txt" 2>/dev/null || : >"$OUT/console$ep.txt"
done
echo "--- advertiser"; grep -aE 'amateur|adv_|bt_enable|SAFE|efr32-radio' "$OUT/console0.txt" || true
echo "--- scanner"; grep -aE 'amateur|scan_|bt_enable|SAFE|efr32-radio' "$OUT/console1.txt" || true
echo "--- scanner names: $(grep -ac "^rx_name: $CALLSIGN\$" "$OUT/console1.txt" || true)"

fail=0
grep -aq 'amateur-selftest: PASS' "$OUT/console0.txt" || { echo "FAIL: advertiser self-test"; fail=1; }
grep -aq '^amateur: enable 0' "$OUT/console0.txt" || { echo "FAIL: advertiser enable"; fail=1; }
grep -aq '^amateur: enable 0' "$OUT/console1.txt" || { echo "FAIL: scanner enable"; fail=1; }
grep -aq "^rx_name: $CALLSIGN\$" "$OUT/console1.txt" || { echo "FAIL: call sign not received"; fail=1; }
if grep -aq 'SAFE SHUTDOWN' "$OUT/console0.txt" "$OUT/console1.txt"; then
    echo "FAIL: safe shutdown"; fail=1
fi

# Every advertiser carrier write (SYNTH.FREQ, 0x...018034) is a plan carrier: an odd MHz.
python3 - "$OUT/radio.trace" <<'EOF' || fail=1
import re, sys
HFXO = 38400000
seen = {}
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"endpoint=0 addr=0x(?:a|b)8018034 value=0x([0-9a-f]+)", line)
    if m:
        khz = round(int(m.group(1), 16) * HFXO / 2**19 / 1000)
        seen[khz] = seen.get(khz, 0) + 1
bad = {k: v for k, v in seen.items() if (k // 1000) % 2 == 0 and k % 1000 == 0}
print("advertiser carriers (kHz: writes):", dict(sorted(seen.items())))
if not seen or bad:
    print("FAIL: Bluetooth (even MHz) carriers used:", bad)
    sys.exit(1)
EOF

# The closing identification: decode the OOK key pattern.
python3 - "$OUT/radio.trace" "$CALLSIGN" <<'EOF' || fail=1
import re, sys
MORSE = {".-": "A", "-...": "B", "-.-.": "C", "-..": "D", ".": "E", "..-.": "F", "--.": "G",
         "....": "H", "..": "I", ".---": "J", "-.-": "K", ".-..": "L", "--": "M", "-.": "N",
         "---": "O", ".--.": "P", "--.-": "Q", ".-.": "R", "...": "S", "-": "T", "..-": "U",
         "...-": "V", ".--": "W", "-..-": "X", "-.--": "Y", "--..": "Z", "-----": "0",
         ".----": "1", "..---": "2", "...--": "3", "....-": "4", ".....": "5", "-....": "6",
         "--...": "7", "---..": "8", "----.": "9", "-..-.": "/"}
ids = [l for l in open(sys.argv[1], errors="replace") if "OOK TX" in l and "endpoint=0" in l]
if not ids:
    print("FAIL: no OOK identification"); sys.exit(1)
for l in ids:
    f = dict(re.findall(r"(\w+)=(\S+)", l))
    raw = bytes.fromhex(f["bits"])
    bits = "".join("1" if (b >> i) & 1 else "0" for b in raw for i in range(8))
    baud = int(f["baud"])
    # 20 WPM PARIS: 60 ms a unit.
    per_unit = baud * 60 // 1000
    runs = re.findall(r"1+|0+", bits.rstrip("0"))
    text, sym = "", ""
    for r in runs:
        units = len(r) / per_unit
        if units != int(units):
            print("FAIL: run of", len(r), "symbols is not whole units of", per_unit); sys.exit(1)
        if r[0] == "1":
            sym += "." if units == 1 else "-" if units == 3 else "?"
        elif units >= 3:
            text += MORSE.get(sym, "?"); sym = ""
            if units >= 7:
                text += " "
    text += MORSE.get(sym, "?")
    print(f"OOK identification: {f['khz']} kHz, {baud} Bd, {f['airtime_ns']} ns, "
          f"synth_in_range={f['synth_in_range']}: '{text}'")
    if text != sys.argv[2]:
        print("FAIL: decoded", text); sys.exit(1)
EOF
grep -aq '^amateur: disable 0, identifications 1, active 0' "$OUT/console0.txt" ||
    { echo "FAIL: closing identification and return to Part 15"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "amateur-e2e: FAIL"
    exit 1
fi
echo "amateur-e2e: PASS"
