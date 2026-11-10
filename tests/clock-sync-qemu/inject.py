#!/usr/bin/env python3
"""inject.py <qmp-socket> <trace-file> <phase> <property> <value> <timeout-s>

Wait for the guest to print "clock sync: phase <phase>" (its USART1 bytes in the QEMU trace),
then set the clock fault <property> (x-hf-ppm, x-lf-ppm: an integer; x-timer0-stopped,
x-lf-stopped: true) on the SoC's peripherals, then wait for the guest to finish. Prints the
guest's console. Exits non-zero if the phase or the end never comes.
"""
import importlib.util
import os
import re
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    'mc', os.path.join(here, '../radio-rx-port/qemu-miss-count.py'))
mc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mc)

sock, trace, phase, prop, value, timeout = sys.argv[1:7]


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


def wait_for(*texts):
    end = time.monotonic() + float(timeout)
    while time.monotonic() < end:
        c = console()
        if any(t in c for t in texts):
            return True
        time.sleep(0.2)
    return False


q = None
for _ in range(50):
    try:
        q = mc.Qmp(sock)
        break
    except OSError:
        time.sleep(0.1)
if q is None or not wait_for(f'clock sync: phase {phase}'):
    print(console())
    sys.exit('inject: the phase never came')
if prop != 'none':
    v = True if value == 'true' else int(value)
    q.command('qom-set', path='/machine/soc/periph', property=prop, value=v)
# A failed clock may stop the guest's own sleeps: its report is the end too.
ok = wait_for('clock sync: done', 'clock sync fault:')
time.sleep(0.5)
print(console())
sys.exit(0 if ok else 'inject: the run never finished')
