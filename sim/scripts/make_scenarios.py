#!/usr/bin/env python3
"""Write the stimulation comparison scenarios (docs/stim-simulation.md) as JSON files.

    make_scenarios.py OUTDIR

Every scenario starts the same stimulus (wire channel 0, level 20, 50 Hz, table widths 0x49,
5 s frame from t = 4.0 s) and applies its fault at t = 5.0 s; the run ends at 7.5 s.
Pads 7 and 8 are faulted together: the stock firmware drives pad 7 for wire channel 0, the open
firmware before the route fix drove pad 8.
"""
import json
import os
import sys

BASE = [
    {"t": 3400, "do": "prep"},
    {"t": 3500, "do": "cmd", "name": "widths", "hex": "49494949494949494949"},
    {"t": 3600, "do": "cmd", "name": "run", "hex": "25"},
    {"t": 4000, "do": "cmd", "name": "frame", "hex": "1400000000000000000032003232000000010027"},
]
FAULT_T = 5000
END = 7500
SUIT = "/machine/suit"
PADS = "0x180"


def fault(path, prop, value, t=FAULT_T):
    return {"t": t, "do": "fault", "path": path, "prop": prop, "value": value}


SCENARIOS = {
    "baseline": [],
    "stop": [{"t": FAULT_T, "do": "cmd", "name": "run", "hex": "00"}],
    "open": [fault(SUIT, "fault-open", PADS)],
    "short": [fault(SUIT, "fault-short", PADS)],
    "overcurrent": [fault(SUIT, "fault-overcurrent-pct", "300")],
    "sense-stuck-high": [fault(SUIT, "fault-sense-stuck-mv", "1500")],
    "sense-stuck-zero": [fault(SUIT, "fault-sense-stuck-mv", "0")],
    "adc-stall": [fault("/machine/soc/periph", "x-iadc-stall", "1")],
    "switch-readback": [fault(SUIT, "fault-readback-xor", "0x01000100")],
    "dac-dead": [fault(SUIT, "fault-dac-ignore", "1"),
                 {"t": 6000, "do": "cmd", "name": "run", "hex": "00"}],
    "expander-nack": [fault(SUIT, "fault-expander-nack", "1")],
    "i2c-stuck": [fault("/machine/soc/i2c0", "x-sda-stuck", "1")],
    "hang-irqs-off": [{"t": FAULT_T, "do": "hang", "kind": "irqs-off"}],
    "hang-mid-pulse": [{"t": FAULT_T, "do": "hang", "kind": "mid-pulse"}],
    "hang-irqs-off-gates-float-on": [{"t": FAULT_T, "do": "hang", "kind": "irqs-off"}],
    "battery-low": [fault(SUIT, "battery-mv", "6500"), fault(SUIT, "battery-mv", "5000", 6000)],
    "button-power": [fault(SUIT, "expander-ext-low", "0x1")],
    "disconnect": [{"t": FAULT_T, "do": "disconnect"}],
    "radio-shutdown": [{"t": FAULT_T, "do": "radio_shutdown"}],
    # Inside the open firmware's active window in the model (4.05-4.15 s), see the doc.
    "early-hang-mid-pulse": [{"t": 4040, "do": "hang", "kind": "mid-pulse"}],
    "early-stop": [{"t": 4100, "do": "cmd", "name": "run", "hex": "00"}],
    "early-radio-shutdown": [{"t": 4040, "do": "radio_shutdown"}],
}
BOARD = {"hang-irqs-off-gates-float-on": {"gate-float-on": "on"}}


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, extra in SCENARIOS.items():
        sc = {"duration_ms": END, "steps": BASE + extra + [{"t": END - 100, "do": "status"}],
              "board": BOARD.get(name, {})}
        json.dump(sc, open(os.path.join(out, f"{name}.json"), "w"), indent=1)
    print(" ".join(SCENARIOS))


if __name__ == "__main__":
    main()
