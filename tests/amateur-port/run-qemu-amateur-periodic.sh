#!/usr/bin/env bash
# The periodic identification (47 CFR 97.119(a), docs/amateur-radio.md section 5) in QEMU:
# an advertiser under amateur rules that never stops (build/am-adv-long:
# prj.conf;prj-advertiser.conf, -DCONFIG_AMATEUR_PORT_ADV_SECONDS=0) for RUN_SECONDS of host
# time (virtual time runs ahead while the guest idles). Checks: at least MIN_IDS identifications,
# each ending at most 600 s after the previous one (or after amateur operation began), each
# frame on air for its whole airtime (RAC TX until the frame's end, not cut by a link-layer
# event), and no safe shutdown.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE=${IMAGE:-"$ROOT/build/am-adv-long/zephyr/zephyr.bin"}
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
RUN_SECONDS=${1:-300}
MIN_IDS=${MIN_IDS:-2}
OUT=${OUT:-"$ROOT/build/amateur-periodic"}

rm -rf "$OUT"
mkdir -p "$OUT"
timeout "$RUN_SECONDS" "$QEMU_BIN" -M "efr32bg22,flash=$IMAGE" \
    -display none -monitor none -icount shift=3,sleep=off \
    -serial null -serial "file:$OUT/console.raw" \
    -trace enable=efr32bg22_ook_tx -trace enable=efr32bg22_rac_state \
    -trace file="$OUT/radio.trace" >"$OUT/qemu.log" 2>&1 || true
tr -d '\r' <"$OUT/console.raw" >"$OUT/console.txt"

python3 - "$OUT" "$MIN_IDS" <<'EOF'
import re, sys
out, min_ids = sys.argv[1], int(sys.argv[2])
INTERVAL_MS = 600000  # 47 CFR 97.119(a)
console = open(out + "/console.txt", errors="replace").read()
ends = [int(m) for m in re.findall(r"identification \d+ \(\S+, [^)]+\) ended at (\d+) ms", console)]
fail = False
if "SAFE SHUTDOWN" in console:
    print("FAIL: safe shutdown"); fail = True
if len(ends) < min_ids:
    print(f"FAIL: {len(ends)} identifications, expected at least {min_ids}"); fail = True
prev = 0
for e in ends:
    if e - prev > INTERVAL_MS:
        print(f"FAIL: identification ended {e - prev} ms after the previous"); fail = True
    prev = e
print("identification ends (ms):", ends, "gaps:", [b - a for a, b in zip([0] + ends, ends)])
# Each OOK frame: RAC stays in TX (9) from before the frame until its airtime has passed.
lines = open(out + "/radio.trace", errors="replace").read().splitlines()
tx_start = None
for i, l in enumerate(lines):
    m = re.search(r"now=(\d+) RAC state 8 -> 9", l)
    if m:
        tx_start = int(m.group(1))
    if "OOK TX" in l:
        air = int(re.search(r"airtime_ns=(\d+)", l).group(1))
        for l2 in lines[i + 1:]:
            m2 = re.search(r"now=(\d+) RAC state 9 -> (\d+)", l2)
            if m2:
                on_air = int(m2.group(1)) - tx_start
                ok = on_air >= air and m2.group(2) == "10"
                print(f"OOK frame: TX {on_air} ns (airtime {air} ns), left TX to state "
                      f"{m2.group(2)}: {'whole' if ok else 'CUT'}")
                fail |= not ok
                break
print("amateur-periodic:", "FAIL" if fail else "PASS")
sys.exit(1 if fail else 0)
EOF
