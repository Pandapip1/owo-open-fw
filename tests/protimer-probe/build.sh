#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-"$here/build"}
mkdir -p "$out"

arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -nostdlib \
  -Wl,--build-id=none -Wl,-T,"$here/link.ld" \
  "$here/probe.S" -o "$out/probe.elf"
arm-none-eabi-objcopy -O binary "$out/probe.elf" "$out/probe.bin"
arm-none-eabi-size "$out/probe.elf"
