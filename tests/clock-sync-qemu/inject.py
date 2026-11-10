#!/usr/bin/env python3
"""inject.py <qmp-socket> <gdb-socket> <elf> <trace-file> <phase> <property> <value> <hang-guard-s>

Drive the guest (src/main.c) through its two phases and set the clock fault <property>
(x-hf-ppm, x-lf-ppm: an integer; x-timer0-stopped, x-lf-stopped: true; none: no fault) on
the SoC's peripherals in <phase>. Awake: with the model stopped, set together with the signal
that starts the phase (the suit board model's expander /INT), so where the fault lands does not
depend on the model's speed. Sleeping: at the scheduler clock's first entry to a state deeper
than runtime idle once the phase starts (sched_pm_entry() in <elf>, a breakpoint through the
model's gdbstub). Ends at the guest's report or its end, and prints its console.
<hang-guard-s> bounds only a hang of the model.
"""
import importlib.util
import os
import re
import socket
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    'mc', os.path.join(here, '../radio-rx-port/qemu-miss-count.py'))
mc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mc)

sock, gdb_sock, elf, trace, phase, prop, value, guard = sys.argv[1:9]
end = time.monotonic() + float(guard)


def console():
    out = bytearray()
    try:
        with open(trace, errors='replace') as f:
            for line in f:
                m = re.search(r'USART1 TX=0x([0-9a-fA-F]{2})', line)
                if m:
                    out.append(int(m.group(1), 16))
    except FileNotFoundError:
        pass
    return out.decode(errors='replace').replace('\r', '')


REPORT = re.compile(r'clock sync fault: reason \d+ expected \d+ measured \d+\n')


def wait_for(*texts):
    """The first of @texts in the console (a report only once its line is complete)."""
    while time.monotonic() < end:
        c = console()
        for t in texts:
            if t == 'clock sync fault:' and REPORT.search(c):
                return t
            if t != 'clock sync fault:' and t in c:
                return t
        time.sleep(0.05)
    return None


q = None
while q is None and time.monotonic() < end:
    try:
        q = mc.Qmp(sock)
    except OSError:
        time.sleep(0.05)
if q is None:
    sys.exit('inject: no QMP')


def inject(at):
    if phase == at and prop != 'none':
        v = True if value == 'true' else int(value)
        q.command('qom-set', path='/machine/soc/periph', property=prop, value=v)


def signal(low):
    q.command('qom-set', path='/machine/suit', property='expander-ext-low', value=low)


class Gdb:
    """The model's gdbstub (remote protocol), for a fault at an exact guest instruction."""

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        cwd = os.getcwd()
        os.chdir(os.path.dirname(os.path.abspath(path)))
        try:
            self.sock.connect(os.path.basename(path))
        finally:
            os.chdir(cwd)
        self.buf = b''

    def packet(self, data):
        self.sock.sendall(b'$%s#%02x' % (data.encode(), sum(data.encode()) & 0xff))
        while True:
            m = re.match(rb'[+-]*\$([^#]*)#..', self.buf)
            if m:
                self.buf = self.buf[m.end():]
                self.sock.sendall(b'+')
                return m.group(1).decode()
            self.sock.settimeout(max(end - time.monotonic(), 0.1))
            try:
                chunk = self.sock.recv(4096)
            except TimeoutError:
                print(console())
                sys.exit('inject: no breakpoint hit')
            if not chunk:
                sys.exit('inject: gdbstub closed')
            self.buf += chunk

    def detach(self):
        self.sock.sendall(b'$D#44')
        self.sock.close()


def symbol(name):
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import SymbolTableSection

    with open(elf, 'rb') as f:
        for section in ELFFile(f).iter_sections():
            if isinstance(section, SymbolTableSection):
                for sym in section.iter_symbols():
                    if sym.name == name:
                        return sym['st_value'] & ~1
    sys.exit(f'inject: no {name} in {elf}')


def inject_at_deep_sleep_entry():
    """@prop as the scheduler clock anchors on a fresh BURTC edge: its entry to EM2."""
    g = Gdb(gdb_sock)
    # anchor_locked(wait): with wait, it captures the next BURTC edge (PM entry and exit).
    entry = symbol('anchor_locked')
    signal(0)
    while True:
        g.packet(f'Z1,{entry:x},2')
        g.packet('c')
        g.packet(f'z1,{entry:x},2')
        if int.from_bytes(bytes.fromhex(g.packet('p0')), 'little') != 0:
            break
        # Off the breakpoint before it goes back in: a continue from it would hit it again.
        g.packet('s')
    inject('sleeping')
    g.detach()
    q.command('cont')


for at, low in (('awake', 1), ('sleeping', 0)):
    seen = wait_for(f'clock sync: phase {at}', 'clock sync fault:')
    if seen is None:
        print(console())
        sys.exit(f'inject: the {at} phase never came')
    if seen == 'clock sync fault:':
        break
    if at == phase == 'sleeping':
        inject_at_deep_sleep_entry()
        continue
    # Fault and signal at one virtual instant: the model is stopped in between.
    q.command('stop')
    inject(at)
    signal(low)
    q.command('cont')
ok = wait_for('clock sync: done', 'clock sync fault:') is not None
print(console())
sys.exit(0 if ok else 'inject: the run never finished')
