#!/usr/bin/env python3
"""Package a flat application image as a Silicon Labs GBL v3 file for the suit's OTA path.

Writes exactly the structure of the stock images (owo-suit-firmware-analysis.md section 1):
HEADER_V3, APPLICATION (type 0x20), one ERASEPROG tag (flash address + raw data), END (CRC-32).
It adds no signature or encryption tags. Whether the suit's bootloader accepts an image that
differs only in content is inferred, not verified (docs/ota.md).

usage: make-gbl.py app.bin out.gbl [--address 0x16000] [--version N]
       make-gbl.py --selftest stock.gbl   (rebuild a stock file from its own image; must be identical)
"""
import argparse
import struct
import sys
import zlib

HEADER_V3, APPLICATION, ERASEPROG, END = 0x03A617EB, 0xF40A0AF4, 0xFD0303FD, 0xFC0404FC
APP_TYPE_BLUETOOTH = 0x20  # as in the stock images


def tag(t: int, payload: bytes) -> bytes:
    return struct.pack("<II", t, len(payload)) + payload


def build(image: bytes, address: int, version: int = 1, app_type: int = APP_TYPE_BLUETOOTH) -> bytes:
    out = tag(HEADER_V3, struct.pack("<II", 0x03000000, 0))
    out += tag(APPLICATION, struct.pack("<III", app_type, version, 0) + bytes(16))
    out += tag(ERASEPROG, struct.pack("<I", address) + image)
    out += struct.pack("<II", END, 4)
    out += struct.pack("<I", zlib.crc32(out + b"") & 0xFFFFFFFF)
    # The stock files are padded with 0xFF to a multiple of 4 bytes.
    return out + b"\xff" * (-len(out) % 4)


def parse(b: bytes):
    """-> (address, image, version, crc_ok) of a GBL with one ERASEPROG tag."""
    o, addr, img, ver = 0, None, None, None
    crc_ok = False
    while o + 8 <= len(b):
        t, n = struct.unpack_from("<II", b, o)
        if t == APPLICATION:
            ver = struct.unpack_from("<I", b, o + 12)[0]
        if t == ERASEPROG:
            addr = struct.unpack_from("<I", b, o + 8)[0]
            img = b[o + 12 : o + 8 + n]
        if t == END:
            crc_ok = zlib.crc32(b[: o + 8]) & 0xFFFFFFFF == struct.unpack_from("<I", b, o + 8)[0]
            break
        o += 8 + n
    return addr, img, ver, crc_ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--address", type=lambda s: int(s, 0), default=0x16000)
    ap.add_argument("--version", type=int, default=1)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        stock = open(a.input, "rb").read()
        addr, img, ver, ok = parse(stock)
        rebuilt = build(img, addr, ver)
        print(f"stock crc ok={ok} address={addr:#x} len={len(img)} version={ver}")
        print("rebuild identical:", rebuilt == stock)
        return 0 if ok and rebuilt == stock else 1
    img = open(a.input, "rb").read()
    sp, pc = struct.unpack_from("<II", img, 0)
    if not (0x20000000 <= sp <= 0x20008000) or not (a.address <= pc < 0x80000):
        sys.exit(f"does not look like an image linked at {a.address:#x}: SP={sp:#x} reset={pc:#x}")
    if not a.output:
        sys.exit("need an output path")
    open(a.output, "wb").write(build(img, a.address, a.version))
    print(f"wrote {a.output}: {len(img)} bytes at {a.address:#x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
