#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Turn the independently linked M0+ image into M33-owned const data."""

import pathlib
import sys


if len(sys.argv) != 4 or sys.argv[3] not in {"open", "proprietary"}:
    raise SystemExit("usage: embed_binary.py INPUT OUTPUT {open,proprietary}")

source = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])
mode = sys.argv[3]
image = source.read_bytes()
maximum_size = 0x3E00 if mode == "open" else 0x8000
if not image or len(image) > maximum_size:
    raise SystemExit(f"invalid sequencer image size: {len(image)}")

rows = []
for offset in range(0, len(image), 12):
    rows.append("\t" + ", ".join(f"0x{byte:02x}" for byte in image[offset:offset + 12]) + ",")

destination.write_text(
    f"/* Generated {mode} sequencer image; do not edit. */\n"
    "#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n\n"
    "const uint8_t efr32_radio_seq_image[] __attribute__((aligned(4))) = {\n"
    + "\n".join(rows)
    + "\n};\n"
    + f"const size_t efr32_radio_seq_image_size = {len(image)}U;\n"
    + "const bool efr32_radio_seq_image_is_open = "
    + ("true" if mode == "open" else "false")
    + ";\n"
)
