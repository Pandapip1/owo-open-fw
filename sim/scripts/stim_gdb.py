# gdb-Python driver for stimulation scenarios on the QEMU suit board (docs/stim-simulation.md).
#
# Loaded by stim_scenario.py into a batch gdb attached to QEMU's gdb stub. The board's
# "script" property pauses the VM at every action time; each pause arrives here as a stop,
# the actions due at that time are applied through the firmware adapter, and the VM resumes.
# The adapter (a Python file named by STIM_ADAPTER) defines:
#   setup(), command(name, data: bytes), disconnect(), hang(kind), poke(addr, size, value),
#   radio_shutdown(), status() -> dict
# Actions are logged as JSON lines to STIM_LOG with the virtual time of the pause.
import json
import os
import runpy

import gdb

LOG = open(os.environ["STIM_LOG"], "a", buffering=1)
STEPS = json.load(open(os.environ["STIM_STEPS"]))  # [[t_ms, [action, ...]], ...]
ADAPTER = runpy.run_path(os.environ["STIM_ADAPTER"])


def log(**kw):
    LOG.write(json.dumps(kw) + "\n")


def m33():
    """QEMU's gdb stub shows each core as a thread; the M33 is CPU 0, gdb thread 1 (the radio
    sequencer M0+ is thread 2). A stop may land on either."""
    gdb.execute("thread 1", to_string=True)


def apply(action):
    kind = action["do"]
    if kind == "cmd":
        r = ADAPTER["command"](action["name"], bytes.fromhex(action["hex"]),
                               not action.get("nowait", False))
    elif kind == "disconnect":
        r = ADAPTER["disconnect"]()
    elif kind == "hang":
        r = ADAPTER["hang"](action["kind"])
    elif kind == "poke":
        r = ADAPTER["poke"](action["addr"], action["size"], action["value"])
    elif kind == "prep":
        r = ADAPTER["prep"]()
    elif kind == "radio_shutdown":
        r = ADAPTER["radio_shutdown"](action.get("kind"))
    elif kind == "status":
        r = None
    elif kind == "eval":
        r = {}
        for e in action["exprs"]:
            try:
                r[e] = gdb.execute(e, to_string=True) if e.split()[0].split("/")[0] in (
                    "bt", "info", "x", "p", "print") else str(gdb.parse_and_eval(e))
            except gdb.error as err:
                r[e] = f"error: {err}"
    else:
        raise ValueError(kind)
    return r


def main():
    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    ADAPTER["setup"]()
    # Diagnostic gdb commands for this run only (never part of a measurement).
    if os.environ.get("STIM_GDB_INIT"):
        gdb.execute(f"source {os.environ['STIM_GDB_INIT']}")
        log(event="gdb-init", file=os.environ["STIM_GDB_INIT"])
    for t_ms, actions in STEPS:
        try:
            gdb.execute("continue")
        except gdb.error as e:
            log(t_ms=t_ms, event="target-lost", error=str(e))
            return
        m33()
        for a in actions:
            try:
                r = apply(a)
                log(t_ms=t_ms, action=a, result=r, status=ADAPTER["status"]())
            except gdb.error as e:
                log(t_ms=t_ms, action=a, error=str(e))
    log(event="steps-done", status=ADAPTER["status"]())
    try:
        gdb.execute("continue")  # until the scripted quit
    except gdb.error:
        pass


main()
