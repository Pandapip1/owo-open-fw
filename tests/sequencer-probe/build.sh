#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-"$here/build"}
source=${SOURCE:-"$here/probe.c"}
mkdir -p "$out"

# Per source language: STOP_EXCEPTION reaches C sources through the preprocessor, and
# generated .S replays (generate-replay.py) through the assembler.
case "$source" in
*.c) lang=(-Os -ffreestanding -Wall -Wextra -Werror -DSTOP_EXCEPTION="${STOP_EXCEPTION:-0}") ;;
*) lang=(-Wa,-defsym,STOP_EXCEPTION="${STOP_EXCEPTION:-0}") ;;
esac

arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -nostdlib \
  -DSKIP_WAKE_PREFIX="${SKIP_WAKE_PREFIX:-0}" \
  -DUSE_NONSECURE_RADIO_ALIAS="${USE_NONSECURE_RADIO_ALIAS:-0}" \
  "${lang[@]}" \
  -Wl,--build-id=none -Wl,-T,"$here/link.ld" \
  "$source" -o "$out/probe.elf"
arm-none-eabi-objcopy -O binary "$out/probe.elf" "$out/probe.bin"
arm-none-eabi-size "$out/probe.elf"
