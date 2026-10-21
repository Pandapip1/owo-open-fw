#!/usr/bin/env bash
# Enforce a nonzero RAM reserve for the full BG22 hardware-output image.
#
# Usage: scripts/check-hardware-ram.sh [path/to/zephyr.map]
# The linker map is authoritative here: it includes initialized data, BSS, and
# noinit thread stacks. `size` alone omits the allocation layout that matters
# for the 32 KiB part.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
map=${1:-"$root/build/ram-probe/zephyr/zephyr.map"}
limit=${RAM_BUDGET_BYTES:-31000}

if [[ ! -r "$map" ]]; then
    echo "RAM budget: linker map not readable: $map" >&2
    exit 2
fi

origin=$(awk '$1 == "RAM" && $2 ~ /^0x/ { print $2; exit }' "$map")
end=$(awk '/_image_ram_end =/ { print $1; exit }' "$map")
if [[ ! "$origin" =~ ^0x[0-9a-fA-F]+$ ]] ||
   [[ ! "$end" =~ ^0x[0-9a-fA-F]+$ ]]; then
    echo "RAM budget: RAM origin or _image_ram_end not found in $map" >&2
    exit 2
fi

# `_image_ram_size` starts after any retained-RAM reservation and alignment.
# Subtracting the linker-declared RAM origin reproduces the linker's complete
# "RAM: Used Size" accounting, including those bytes.
used=$((end - origin))
if (( used > limit )); then
    echo "RAM budget: ${used} B exceeds ${limit} B (${map})" >&2
    exit 1
fi

echo "RAM budget: ${used} B <= ${limit} B (${map})"
