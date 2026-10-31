#!/usr/bin/env python3
"""Summarise a suit board recording (owo-suit "vcd" property) into pulse and safety figures.

    stim_vcd.py board.vcd [--from-ms T] [--json out.json]

Signals are those hw/misc/owo-suit.c declares. Bridge codes: 0 low sides on (pads shorted, the
stock "off"), 1 positive (high A + low B), 2 negative, 3 floating, 4 shoot-through.
"""
import argparse
import json
import sys
from collections import defaultdict

BRIDGE_NAMES = {0: "shorted-off", 1: "positive", 2: "negative", 3: "floating", 4: "shoot-through"}
NS_PER_US = 1000
NS_PER_MS = 1_000_000


def parse(path):
    """Return {name: [(t_ns, value), ...]} with value int, float or 'z'."""
    ids, series, t = {}, defaultdict(list), 0
    with open(path) as f:
        header = True
        for line in f:
            line = line.strip()
            if not line:
                continue
            if header:
                if line.startswith("$var"):
                    p = line.split()
                    ids[p[3]] = p[4]
                elif line.startswith("$enddefinitions"):
                    header = False
                continue
            c = line[0]
            if c == "#":
                t = int(line[1:])
            elif c in "01zx" and len(line) > 1 and line[1] not in " ":
                series[ids[line[1:]]].append((t, "z" if c in "zx" else int(c)))
            elif c == "b":
                v, i = line[1:].split()
                series[ids[i]].append((t, int(v, 2)))
            elif c == "r":
                v, i = line[1:].split()
                series[ids[i]].append((t, float(v)))
    return series


def value_at(s, t, default=None):
    v = default
    for tt, vv in s:
        if tt > t:
            break
        v = vv
    return v


def intervals(s, pred, t_end):
    """[(start, end, value)] where pred(value) holds."""
    out, start, cur = [], None, None
    for t, v in s:
        if pred(v) and start is None:
            start, cur = t, v
        elif start is not None and (not pred(v) or v != cur):
            out.append((start, t, cur))
            start, cur = (t, v) if pred(v) else (None, None)
    if start is not None:
        out.append((start, t_end, cur))
    return out


def integrate(s, t0, t1):
    """Integral of a sample-and-hold real signal over [t0, t1] (value x seconds)."""
    acc, last_t, last_v = 0.0, t0, value_at(s, t0, 0.0)
    for t, v in s:
        if t <= t0:
            continue
        if t >= t1:
            break
        acc += last_v * (t - last_t) / 1e9
        last_t, last_v = t, v
    return acc + last_v * (t1 - last_t) / 1e9


def summarise(series, t_from=0):
    t_end = max((s[-1][0] for s in series.values() if s), default=0)
    br = series.get("bridge", [])
    phases = [p for p in intervals(br, lambda v: v in (1, 2), t_end) if p[1] > t_from]
    pulses, i = [], 0
    while i < len(phases):
        a = phases[i]
        b = phases[i + 1] if i + 1 < len(phases) else None
        pulse = {"t_ms": a[0] / NS_PER_MS, "first": BRIDGE_NAMES[a[2]],
                 "phase1_us": (a[1] - a[0]) / NS_PER_US,
                 "pad": value_at(series.get("pad", []), a[0]),
                 "dac_code": value_at(series.get("dac_code", []), a[0]),
                 "q1_uc": integrate(series.get("i_out_ma", []), a[0], a[1]) * 1e3}
        if b and b[2] != a[2] and b[0] - a[1] < 5 * NS_PER_MS:
            pulse.update(gap_us=(b[0] - a[1]) / NS_PER_US, phase2_us=(b[1] - b[0]) / NS_PER_US,
                         q2_uc=integrate(series.get("i_out_ma", []), b[0], b[1]) * 1e3)
            i += 2
        else:
            pulse.update(gap_us=None, phase2_us=None, q2_uc=0.0)
            i += 1
        i_ma = [v for t, v in series.get("i_out_ma", []) if a[0] <= t <= (b or a)[1]]
        pulse["i_peak_ma"] = max((abs(v) for v in i_ma), default=0.0)
        pulse["net_q_uc"] = pulse["q1_uc"] + pulse["q2_uc"]
        pulses.append(pulse)
    starts = defaultdict(list)
    for p in pulses:
        starts[p["pad"]].append(p["t_ms"])
    periods = {str(k): [round(b - a, 3) for a, b in zip(v, v[1:])][:20] for k, v in starts.items()}
    pwm = [t for t, v in series.get("PC7_boost_pwm", []) if v == 1]
    pwm_periods = [b - a for a, b in zip(pwm, pwm[1:]) if b - a < 10 * NS_PER_MS]
    boost = intervals(series.get("boost_active", []), lambda v: v == 1, t_end)
    sense = series.get("adc_sense_mv", [])
    final = {name: (s[-1][1] if s else None) for name, s in series.items()}
    return {
        "t_end_ms": t_end / NS_PER_MS,
        "pulses": len(pulses),
        "pulse_list": pulses,
        "pulse_periods_ms_by_pad": periods,
        "phase_widths_us": sorted({round(p["phase1_us"], 1) for p in pulses}),
        "gaps_us": sorted({round(p["gap_us"], 1) for p in pulses if p["gap_us"] is not None}),
        "dac_codes": sorted({p["dac_code"] for p in pulses if p["dac_code"] is not None}),
        "pads": sorted({p["pad"] for p in pulses if p["pad"] is not None}),
        "max_abs_net_q_uc": max((abs(p["net_q_uc"]) for p in pulses), default=0.0),
        "boost_intervals_ms": [(a / NS_PER_MS, b / NS_PER_MS) for a, b, _ in boost][:50],
        "boost_pwm_period_us": (sum(pwm_periods) / len(pwm_periods) / NS_PER_US) if pwm_periods else None,
        "sense_samples": len(sense),
        "sense_max_mv": max((v for _, v in sense), default=None),
        "battery_samples": len(series.get("adc_battery_mv", [])),
        "shoot_through": final.get("shoot_through_count"),
        "mcu_resets": [t / NS_PER_MS for t, _ in series.get("mcu_reset", [])[1:]],
        "script_steps_ms": [t / NS_PER_MS for t, _ in series.get("script_step", [])],
        "last_drive_end_ms": (phases[-1][1] / NS_PER_MS) if phases else None,
        "final": {k: final.get(k) for k in (
            "bridge", "PB0_hs_a", "PB1_hs_b", "PB2_ls_a", "PB3_ls_b", "PC7_boost_pwm", "pad",
            "switch_latched", "dac_code", "expander_pins", "boost_active", "rail_v",
            "pad_vc_max_v", "i_out_ma")},
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vcd")
    ap.add_argument("--from-ms", type=float, default=0)
    ap.add_argument("--json")
    a = ap.parse_args()
    s = summarise(parse(a.vcd), int(a.from_ms * NS_PER_MS))
    text = json.dumps(s, indent=1)
    if a.json:
        open(a.json, "w").write(text)
    brief = {k: v for k, v in s.items() if k != "pulse_list"}
    json.dump(brief, sys.stdout, indent=1)
    print()


if __name__ == "__main__":
    main()
