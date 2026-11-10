#!/usr/bin/env python3
"""Compare two runs of the decision harness (src/main.c): the C sequencer's and the assembly one's.

usage: compare.py <c.log> <c.trace> <asm.log> <asm.trace>

Each case must agree on everything the decision leaves: its return, OUTCOME, the stage's state,
the sequencer's shared words (traces, RX2TX_DECISION), and the M0+'s register writes in order
(memory_region_ops_write trace: the reply into the BUFC TX FIFO, FRC.WCNTCMP0, RAC.RXENSRCEN).
TAKEN_AT (RAC.STIMER when the stage was taken) and the cycle count may differ. The M33 marks each
case by writing its number to TIMER0.CC0.OC before writing the stage."""
import re
import sys

CASE = re.compile(r"^CASE (\d+) (\S+) ret=(\d+) outcome=(\S+) state=(\S+) cycles=(\d+) "
                  r"taken_at=\S+(.*)$")
WRITE = re.compile(r"memory_region_ops_write cpu (\d+) mr \S+ addr (0x[0-9a-f]+) "
                   r"value (0x[0-9a-f]+) size (\d+) name '([^']*)'")
MARKER_ADDR = 0x50048068  # TIMER0.CC0.OC, the M33's secure address


def cases(log):
    out = {}
    for line in open(log):
        m = CASE.match(line.rstrip("\n"))
        if m:
            idx = int(m.group(1))
            out[idx] = {"name": m.group(2), "ret": int(m.group(3)), "outcome": m.group(4),
                        "state": m.group(5), "cycles": int(m.group(6)),
                        "shared": m.group(7).strip()}
    return out


def writes(trace):
    """The M0+'s writes after each case's marker, by case number."""
    marker_cpu = None
    current = None
    out = {}
    for line in open(trace):
        m = WRITE.search(line)
        if not m:
            continue
        cpu, addr, value = int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16)
        if addr == MARKER_ADDR and (marker_cpu is None or cpu == marker_cpu):
            marker_cpu = cpu
            current = value
            out[current] = []
            continue
        if current is not None and cpu != marker_cpu:
            out[current].append((m.group(5), addr, value, int(m.group(4))))
    return out


def main():
    c_log, c_trace, a_log, a_trace = sys.argv[1:5]
    c, a = cases(c_log), cases(a_log)
    cw, aw = writes(c_trace), writes(a_trace)
    failed = False
    if sorted(c) != sorted(a) or not c:
        print(f"compare: case sets differ (C {len(c)}, asm {len(a)})")
        return 1
    print(f"{'case':<40} {'ret':>3} {'out':>8} {'writes':>6} {'C cyc':>6} {'asm cyc':>7} {'C-asm':>6}")
    for idx in sorted(c):
        x, y = c[idx], a[idx]
        diffs = [k for k in ("name", "ret", "outcome", "state", "shared") if x[k] != y[k]]
        if cw.get(idx) != aw.get(idx):
            diffs.append("writes")
        n = len(cw.get(idx, []))
        print(f"{x['name']:<40} {x['ret']:>3} {x['outcome'][-4:]:>8} {n:>6} {x['cycles']:>6} "
              f"{y['cycles']:>7} {x['cycles'] - y['cycles']:>+6}"
              + (f"  DIFFER: {', '.join(diffs)}" if diffs else ""))
        if diffs:
            failed = True
            for k in diffs:
                if k == "writes":
                    print(f"    C   writes: {cw.get(idx)}")
                    print(f"    asm writes: {aw.get(idx)}")
                else:
                    print(f"    {k}: C {x[k]!r} asm {y[k]!r}")
    print(f"compare: {len(c)} cases, " + ("DIFFERENCES" if failed else "C and assembly agree"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
