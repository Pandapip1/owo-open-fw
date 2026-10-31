#!/usr/bin/env python3
"""Run one stimulation scenario on the QEMU suit board and summarise the recording.

    stim_scenario.py SCENARIO.json --adapter ADAPTER.py --flash FLASH.bin --out DIR
                     [--devinfo F --userdata F] [--elf ELF] [--qemu BIN]

SCENARIO: {"duration_ms": N, "board": {prop: value, ...},
           "steps": [{"t": ms, "do": "cmd", "name": "frame", "hex": "..."},
                     {"t": ms, "do": "fault", "path": "/machine/suit", "prop": "fault-open",
                      "value": "0x20"},
                     {"t": ms, "do": "disconnect" | "radio_shutdown" | "status"},
                     {"t": ms, "do": "hang", "kind": "irqs-off" | ...},
                     {"t": ms, "do": "poke", "addr": A, "size": 1|2|4, "value": V}]}
Faults are applied by the board model itself at their exact virtual time; every other step
pauses the VM there and runs in gdb through the adapter (stim_gdb.py). DIR receives board.vcd,
qemu.log, gdb.log, actions.jsonl and summary.json.
"""
import argparse
import json
import os
import resource
import socket
import subprocess
import sys
import time
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FILE_LIMIT = 1 << 30  # bound every file QEMU writes
WALL_LIMIT_S = 1800


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario")
    ap.add_argument("--adapter", required=True)
    ap.add_argument("--flash", required=True)
    ap.add_argument("--devinfo")
    ap.add_argument("--userdata")
    ap.add_argument("--elf")
    ap.add_argument("--out", required=True)
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu/qemu-system-arm"))
    ap.add_argument("--trace", default="efr32_wdog,efr32_i2c_event")
    a = ap.parse_args()
    sc = json.load(open(a.scenario))
    os.makedirs(a.out, exist_ok=True)

    script, pauses = [], OrderedDict()
    for st in sorted(sc["steps"], key=lambda s: s["t"]):
        if st["do"] == "fault":
            script.append(f'{st["t"]},{st["path"]},{st["prop"]},{st["value"]}')
        else:
            pauses.setdefault(st["t"], []).append(st)
    for t in pauses:
        script.append(f"{t},pause,-,-")
    script.append(f'{sc["duration_ms"]},quit,-,-')
    script.sort(key=lambda e: float(e.split(",")[0]))
    steps_path = os.path.join(a.out, "steps.json")
    json.dump([[t, acts] for t, acts in pauses.items()], open(steps_path, "w"))

    machine = f"efr32bg22,flash={a.flash},board=owo-suit"
    if a.devinfo:
        machine += f",devinfo={a.devinfo},userdata={a.userdata}"
    port = free_port()
    args = [a.qemu, "-M", machine, "-display", "none", "-icount", "shift=3,sleep=off",
            "-global", f"owo-suit.vcd={a.out}/board.vcd",
            "-global", f"owo-suit.script={';'.join(script)}",
            "-gdb", f"tcp:127.0.0.1:{port}", "-S", "-D", f"{a.out}/trace.log"]
    for ev in a.trace.split(","):
        args += ["-trace", ev]
    for k, v in sc.get("board", {}).items():
        args += ["-global", f"owo-suit.{k}={v}"]
    open(f"{a.out}/cmdline.txt", "w").write(" ".join(args) + "\n")

    def limits():
        resource.setrlimit(resource.RLIMIT_FSIZE, (FILE_LIMIT, FILE_LIMIT))

    qemu = subprocess.Popen(args, stdout=open(f"{a.out}/qemu.log", "w"),
                            stderr=subprocess.STDOUT, preexec_fn=limits)
    time.sleep(1)
    env = dict(os.environ, STIM_LOG=f"{a.out}/actions.jsonl", STIM_STEPS=steps_path,
               STIM_ADAPTER=a.adapter)
    gdb_args = ["gdb", "-q", "-batch", "-nx"]
    if a.elf:
        gdb_args += ["-ex", f"file {a.elf}"]
    gdb_args += ["-ex", f"target remote 127.0.0.1:{port}",
                 "-ex", f"source {os.path.join(HERE, 'stim_gdb.py')}"]
    open(f"{a.out}/actions.jsonl", "w").close()
    gdbp = subprocess.Popen(gdb_args, env=env, stdout=open(f"{a.out}/gdb.log", "w"),
                            stderr=subprocess.STDOUT)
    try:
        qemu.wait(timeout=WALL_LIMIT_S)
    except subprocess.TimeoutExpired:
        qemu.kill()
        print("QEMU wall-time limit reached", file=sys.stderr)
    try:
        gdbp.wait(timeout=30)
    except subprocess.TimeoutExpired:
        gdbp.kill()
    sys.path.insert(0, HERE)
    import stim_vcd
    s = stim_vcd.summarise(stim_vcd.parse(f"{a.out}/board.vcd"))
    json.dump(s, open(f"{a.out}/summary.json", "w"), indent=1)
    print(json.dumps({k: v for k, v in s.items() if k != "pulse_list"}, indent=1))


if __name__ == "__main__":
    main()
