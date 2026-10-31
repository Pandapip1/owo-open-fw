#!/usr/bin/env python3
"""Compare stock and open-firmware scenario runs (docs/stim-simulation.md).

    stim_report.py RUNS_DIR [--fault-ms 5000] [--json out.json] [--md out.md]

RUNS_DIR holds stock-<scenario>/ and ours-<scenario>/ run directories (board.vcd,
actions.jsonl). Per run it reports, from the recording only:
  before:  live pulses (DAC code > 0) before the fault, their phase widths, gap, period and
           peak current;
  after:   live pulses after the fault, the time the bridge still drove current after the fault
           (drive_ms), the charge it delivered (q_uc), the time of the last live drive
           (time_to_safe_ms, from the fault), MCU resets;
  end:     bridge, gates, DAC code, switch, boost, rail voltage and the largest residual pad
           voltage at the end of the run, shoot-throughs.
A run is marked UNSAFE when, at the end, the bridge still drives, or current flows, or the rail
is up with a pad selected and the DAC non-zero, or a pad holds more than 1 V.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stim_vcd  # noqa: E402

NS_PER_MS = 1_000_000
LIVE_MA = 0.05
RESIDUAL_V = 1.0
DRIVE = (1, 2)


def run_metrics(d, fault_ms):
    s = stim_vcd.parse(os.path.join(d, "board.vcd"))
    t_end = max((v[-1][0] for v in s.values() if v), default=0)
    tf = int(fault_ms * NS_PER_MS)
    summ = stim_vcd.summarise(s)
    pulses = summ["pulse_list"]
    live = [p for p in pulses if p["dac_code"] and p["i_peak_ma"] > LIVE_MA]
    before = [p for p in live if p["t_ms"] < fault_ms]
    after = [p for p in live if p["t_ms"] >= fault_ms]
    br = s.get("bridge", [])
    drive = [iv for iv in stim_vcd.intervals(br, lambda v: v in DRIVE, t_end) if iv[1] > tf]
    i_ma = s.get("i_out_ma", [])
    q_after = sum(abs(stim_vcd.integrate(i_ma, max(a, tf), b)) for a, b, _ in drive) * 1e3
    live_drive_end = None
    for a, b, _ in drive:
        if any(abs(v) > LIVE_MA for t, v in i_ma if max(a, tf) <= t <= b):
            live_drive_end = b
    final = summ["final"]
    resets = [t for t in summ["mcu_resets"] if t >= fault_ms]
    status = []
    if os.path.exists(os.path.join(d, "actions.jsonl")):
        for line in open(os.path.join(d, "actions.jsonl")):
            r = json.loads(line)
            if "status" in r:
                status.append((r.get("t_ms"), r["status"]))
    unsafe = []
    if final.get("bridge") in DRIVE:
        unsafe.append("bridge still driving at end")
    if final.get("i_out_ma") and abs(final["i_out_ma"]) > LIVE_MA:
        unsafe.append("current flowing at end")
    if (final.get("rail_v") or 0) > 1 and final.get("pad") not in (None, 255) and final.get("dac_code"):
        unsafe.append("rail up, pad selected, DAC non-zero at end")
    if (final.get("pad_vc_max_v") or 0) > RESIDUAL_V:
        unsafe.append("pad holds charge at end")
    return {
        "run": os.path.basename(d),
        "t_end_ms": t_end / NS_PER_MS,
        "live_before": len(before),
        "pads": sorted({p["pad"] for p in before}),
        "phase1_us": round(sum(p["phase1_us"] for p in before) / len(before), 1) if before else None,
        "phase2_us": round(sum(p["phase2_us"] or 0 for p in before) / len(before), 1) if before else None,
        "gap_us": round(sum(p["gap_us"] or 0 for p in before) / len(before), 1) if before else None,
        "period_ms": round((before[-1]["t_ms"] - before[0]["t_ms"]) / (len(before) - 1), 2)
        if len(before) > 1 else None,
        "i_peak_ma": round(max((p["i_peak_ma"] for p in before), default=0), 3),
        "dac_codes": sorted({p["dac_code"] for p in before}),
        "net_q_uc_max": round(max((abs(p["net_q_uc"]) for p in before), default=0), 4),
        "live_after": len(after),
        "drive_ms_after": round(sum(b - max(a, tf) for a, b, _ in drive) / NS_PER_MS, 3),
        "q_uc_after": round(q_after, 3),
        "time_to_safe_ms": round(live_drive_end / NS_PER_MS - fault_ms, 3) if live_drive_end else 0.0,
        "resets_after": resets,
        "shoot_through": summ["shoot_through"],
        "boost_pwm_period_us": summ["boost_pwm_period_us"],
        "final": final,
        "unsafe": unsafe,
        "status": status[-1][1] if status else None,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs")
    ap.add_argument("--fault-ms", type=float, default=5000)
    ap.add_argument("--json")
    ap.add_argument("--md")
    ap.add_argument("--prefixes", default="stock,ours")
    a = ap.parse_args()
    prefixes = a.prefixes.split(",")
    names = sorted({e.split("-", 1)[1] for e in os.listdir(a.runs)
                    if e.split("-", 1)[0] in prefixes and
                    os.path.exists(os.path.join(a.runs, e, "board.vcd"))})
    out = {}
    for n in names:
        out[n] = {}
        for p in prefixes:
            d = os.path.join(a.runs, f"{p}-{n}")
            if os.path.exists(os.path.join(d, "board.vcd")):
                try:
                    out[n][p] = run_metrics(d, a.fault_ms)
                except Exception as e:  # a truncated recording
                    out[n][p] = {"error": str(e)}
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)
    rows = ["| scenario | fw | live pulses before / after | pad | phases us (+/gap/-) | period ms | "
            "i peak mA | drive ms after | q uC after | safe after ms | resets | end: bridge, DAC, "
            "pad, rail V, pad V | unsafe |", "|" + "---|" * 13]
    for n, by in out.items():
        for p, m in by.items():
            if "error" in m:
                rows.append(f"| {n} | {p} | error: {m['error']} |" + " |" * 11)
                continue
            f = m["final"]
            rows.append(
                f"| {n} | {p} | {m['live_before']} / {m['live_after']} | {m['pads']} | "
                f"{m['phase1_us']}/{m['gap_us']}/{m['phase2_us']} | {m['period_ms']} | "
                f"{m['i_peak_ma']} | {m['drive_ms_after']} | {m['q_uc_after']} | "
                f"{m['time_to_safe_ms']} | {m['resets_after']} | {f.get('bridge')}, "
                f"{f.get('dac_code')}, {f.get('pad')}, {round(f.get('rail_v') or 0, 2)}, "
                f"{round(f.get('pad_vc_max_v') or 0, 3)} | {'; '.join(m['unsafe']) or '-'} |")
    text = "\n".join(rows)
    if a.md:
        open(a.md, "w").write(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
