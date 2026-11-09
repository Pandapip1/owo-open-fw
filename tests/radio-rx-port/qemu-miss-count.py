#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Stop a QEMU run and read each guest's deadline-miss count.

usage: qemu-miss-count.py <qmp-socket> <out-file> <elf>[,<soc>] ...

Each ELF is a guest image built with CONFIG_SCHED_DEADLINE_MISS_COUNT; its
kernel counts every missed, late or unsat deadline in the static `miss_count`
(vendor/zephyr/kernel/deadline_miss.c). The guests are stopped, the counter is
read through each SoC's M33 (`memsave` on that CPU: every SoC has
its own address space), the counts are written to <out-file> one per line as
`<soc> <count>` and QEMU is told to quit. An image without the counter (the
policy is not COUNT) is reported as `<soc> -`: its misses stop the radio
instead, which the scripts see. <soc> defaults to the ELF's position.
"""

import json
import os
import socket
import struct
import sys
import tempfile

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection


def counter_address(path):
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        for section in elf.iter_sections():
            if not isinstance(section, SymbolTableSection):
                continue
            for sym in section.iter_symbols():
                name = sym.name
                if name == 'miss_count' or name.startswith('miss_count.'):
                    if sym['st_info']['type'] == 'STT_OBJECT':
                        return sym['st_value']
    return None


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        # A UNIX socket path is limited to 108 bytes: connect relative to its directory.
        cwd = os.getcwd()
        os.chdir(os.path.dirname(os.path.abspath(path)))
        try:
            self.sock.connect(os.path.basename(path))
        finally:
            os.chdir(cwd)
        self.file = self.sock.makefile('rw')
        self._read()
        self.command('qmp_capabilities')

    def _read(self):
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError('QMP closed')
            msg = json.loads(line)
            if 'event' not in msg:
                return msg

    def command(self, name, **args):
        self.file.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.file.flush()
        reply = self._read()
        if 'error' in reply:
            raise RuntimeError(f'{name}: {reply["error"]}')
        return reply['return']

    def hmp(self, line, cpu):
        # Each call is a fresh monitor: the CPU is selected per call, not with `cpu N`.
        return self.command('human-monitor-command', **{'command-line': line, 'cpu-index': cpu})


def m33_cpus(qmp):
    """CPU index of each SoC's M33, by SoC number."""
    cpus = {}
    for cpu in qmp.command('query-cpus-fast'):
        path = cpu['qom-path']
        # The SoC's M33 is its "armv7m" container; "radio-sequencer" is the M0+.
        if '/armv7m/' not in path:
            continue
        parts = path.split('/')
        soc = next((p for p in parts if p.startswith('soc') and p[3:].isdigit()), 'soc0')
        cpus.setdefault(int(soc[3:]), cpu['cpu-index'])
    return cpus


def main():
    qmp_path, out_path = sys.argv[1], sys.argv[2]
    images = []
    for i, arg in enumerate(sys.argv[3:]):
        elf, _, soc = arg.partition(',')
        images.append((int(soc) if soc else i, elf))
    qmp = Qmp(qmp_path)
    qmp.command('stop')
    cpus = m33_cpus(qmp)
    lines = []
    with tempfile.TemporaryDirectory(dir=os.path.dirname(out_path)) as tmp:
        for soc, elf in images:
            address = counter_address(elf)
            if address is None:
                lines.append(f'{soc} -')
                continue
            dump = os.path.join(tmp, f'soc{soc}.bin')
            qmp.hmp(f'memsave 0x{address:x} 4 "{dump}"', cpus[soc])
            with open(dump, 'rb') as f:
                (count,) = struct.unpack('<I', f.read(4))
            lines.append(f'{soc} {count}')
    with open(out_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    try:
        qmp.command('quit')
    except (RuntimeError, OSError):
        pass


if __name__ == '__main__':
    main()
