#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Builds every probe into <out>/<probe>.{elf,bin}; probe.bin is the PROTIMER enable probe.
set -euo pipefail

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-"$here/build"}
mkdir -p "$out"

# probe, linker script, entry symbol
probes=(
  "probe link.ld protimer_probe_enable"
  "probe-ien-sweep link.ld protimer_probe_ien_sweep"
  "probe-read-vtor link.ld protimer_probe_read_vtor"
  "probe-vtor-only link.ld protimer_probe_vtor_only"
  "probe-irq-handler link-loaded-at-0x200007d0.ld protimer_probe_irq_handler"
  "probe-irq-handler-no-enable link-loaded-at-0x200007d0.ld protimer_probe_irq_no_enable"
)
for p in "${probes[@]}"; do
  read -r name ld entry <<<"$p"
  # -fno-delete-null-pointer-checks: the vector-table copy reads from address 0.
  arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -nostdlib -Os -ffreestanding \
    -fno-delete-null-pointer-checks -Wall -Wextra -Werror \
    -Wl,--build-id=none -Wl,-T,"$here/$ld" -Wl,-e,"$entry" \
    "$here/$name.c" -o "$out/$name.elf"
  arm-none-eabi-objcopy -O binary "$out/$name.elf" "$out/$name.bin"
done
arm-none-eabi-size "$out"/*.elf
