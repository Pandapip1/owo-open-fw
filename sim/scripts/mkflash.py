#!/usr/bin/env python3
"""mkflash.py out.bin bootloader.bin [app.bin@0x16000]... : compose a 512K EFR32BG22 flash image."""
import sys
img = bytearray(b"\xff" * 0x80000)
out = sys.argv[1]
parts = sys.argv[2:]
for i, p in enumerate(parts):
    name, _, addr = p.partition("@")
    d = open(name, "rb").read()
    a = int(addr, 0) if addr else 0
    img[a:a + len(d)] = d
open(out, "wb").write(img)
print("wrote", out, [(p) for p in parts])
