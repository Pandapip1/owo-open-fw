#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Decode a modules/efr32-radio/hal/radio.c PHY config table entry into
named public register fields, instead of re-deriving the address
encoding and field offsets by hand each time.

The config tables (efr32_ble_1m_base_table and friends) use a packed
header word per efr32_radio_table_apply(): the top nibble is the
operation (0=WRITE, 1=AND-mask, 3=OR-value), the next nibble is a
"block" selector, the next byte is a word count, and the low 16 bits
are an offset. The real address is 0xB8000000 + (block<<16) + offset.
This script resolves that address against each already-fetched public
*_S_register.py/*_S_field.py model under build/reference-models/ (see
project memory: radio config tables undecomposed) and prints each
field's value.

Usage:
    scripts/decode-radio-table.py <header_hex> <value_hex> [<value_hex> ...]

Example, decoding FRC.CTRL's AND/OR pair from efr32_ble_1m_delta_table:
    scripts/decode-radio-table.py 0x00014048 0x000407A0

Run from the repository root (reads build/reference-models/ relative
to the current directory).
"""
import re
import sys

PERIPHERALS = {
    'AGC':   (0xC000, 'AGC_S'),
    'RFCRC': (0x10000, 'RFCRC_S'),
    'MODEM': (0x14000, 'MODEM_S'),
    'SYNTH': (0x18000, 'SYNTH_S'),
    'RAC':   (0x20000, 'RAC_S'),
    'FRC':   (0x4000, 'FRC_S'),
}

MODELS_DIR = 'build/reference-models'


def load_fields(prefix):
    path = f'{MODELS_DIR}/{prefix}_field.py'
    with open(path) as f:
        text = f.read()
    pat = re.compile(
        r"class RM_Field_" + re.escape(prefix) + r"_(\w+?)_(\w+)\(Base_RM_Field\):.*?"
        r"'" + re.escape(prefix) + r"\.\1\.\2', '[\w-]+',\s*\n\s*u\"\",\s*\n\s*(\d+), (\d+)\)",
        re.S)
    fields = {}
    for m in pat.finditer(text):
        reg, field, off, width = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
        fields.setdefault(reg, []).append((field, off, width))
    return fields


def load_regs(prefix):
    path = f'{MODELS_DIR}/{prefix}_register.py'
    with open(path) as f:
        text = f.read()
    pat = re.compile(
        r"class RM_Register_" + re.escape(prefix) + r"_(\w+)\(Base_RM_Register\):.*?0x[0-9A-Fa-f]+, (0x[0-9A-Fa-f]+),",
        re.S)
    regs = {}
    for m in pat.finditer(text):
        name, off = m.group(1), int(m.group(2), 16)
        regs[off] = name
    return regs


def build_model():
    fields, regs = {}, {}
    for pname, (_, prefix) in PERIPHERALS.items():
        fields[pname] = load_fields(prefix)
        regs[pname] = load_regs(prefix)
    return fields, regs


def resolve(address):
    off_from_b8 = address - 0xB8000000
    candidates = sorted(PERIPHERALS.items(), key=lambda kv: -kv[1][0])
    for pname, (delta, _) in candidates:
        if off_from_b8 >= delta and (off_from_b8 - delta) < 0x1000:
            return pname, off_from_b8 - delta
    return None, off_from_b8


def decode(fields, regs, pname, reg_off, value):
    regname = regs.get(pname, {}).get(reg_off)
    if regname is None:
        return None, None
    parts = []
    for field, off, width in sorted(fields.get(pname, {}).get(regname, []), key=lambda t: t[1]):
        parts.append(f'{field}={(value >> off) & ((1 << width) - 1)}')
    return regname, parts


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    header = int(argv[1], 16)
    op_names = {0: 'WRITE', 1: 'AND-mask', 3: 'OR-value'}
    op = header >> 28
    block = (header >> 24) & 0xF
    count = (header >> 16) & 0xFF
    offset = header & 0xFFFF
    base_addr = 0xB8000000 + (block << 16) + offset
    values = argv[2:]
    print(f'header=0x{header:08X} op={op_names.get(op, op)} count={count} base_addr=0x{base_addr:08X}')
    if count not in (0, len(values)):
        print(f'warning: header count={count} does not match {len(values)} value(s) given',
              file=sys.stderr)
    fields, regs = build_model()
    for i, vhex in enumerate(values):
        addr = base_addr + i * 4
        pname, reg_off = resolve(addr)
        value = int(vhex, 16)
        if pname is None:
            print(f'  0x{addr:08X} = 0x{value:08X}  UNRESOLVED peripheral')
            continue
        regname, parts = decode(fields, regs, pname, reg_off, value)
        if regname is None:
            print(f'  0x{addr:08X} = 0x{value:08X}  {pname}+0x{reg_off:03X} (register name unknown)')
            continue
        nonzero = [p for p in parts if not p.endswith('=0')]
        shown = ', '.join(nonzero) if nonzero else 'all fields 0'
        print(f'  0x{addr:08X} = 0x{value:08X}  {pname}.{regname}: {shown}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
