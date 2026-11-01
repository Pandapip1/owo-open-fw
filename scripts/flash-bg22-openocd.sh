#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 IMAGE [--run]" >&2
  exit 2
fi

case "${2:-}" in
  ""|--run) ;;
  *) echo "unknown option: $2" >&2; exit 2 ;;
esac

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
image_dir=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)
export OWO_BG22_FLASH_IMAGE="$image_dir/$(basename -- "$1")"

if [ ! -f "$OWO_BG22_FLASH_IMAGE" ]; then
  echo "image not found: $OWO_BG22_FLASH_IMAGE" >&2
  exit 2
fi

openocd_out=$(nix build --no-link --print-out-paths "$repo_dir#openocd")
openocd="$openocd_out/bin/openocd"
scripts="$openocd_out/share/openocd/scripts"
helpers="$repo_dir/scripts/bg22-openocd-flash.cfg"

# A running BG22 application can put the M33 into a state that OpenOCD reports
# as TARGET_UNKNOWN.  A DHCSR write and a subsequent `poll` can even report an
# all-zero TARGET_HALTED context without proving that instructions execute.
# Run a checked SRAM sentinel fixture before any destructive operation, and
# repeat it after erase before starting the asynchronous flash loader.
run_after=0
if [ "${2:-}" = --run ]; then
  run_after=1
fi

# The EK's on-board J-Link by serial.  OWO_BG22_JLINK_SERIAL selects another
# J-Link, e.g. a simulated one (docs/debug-port-model.md); OWO_BG22_ADAPTER_CFG
# replaces the adapter altogether, e.g. scripts/sim/qemu-swd.cfg for QEMU's
# remote_bitbang probe.
if [ -n "${OWO_BG22_ADAPTER_CFG:-}" ]; then
  adapter_args=(-f "$OWO_BG22_ADAPTER_CFG")
else
  adapter_args=(
    -f interface/jlink.cfg
    -c "adapter serial ${OWO_BG22_JLINK_SERIAL:-000440200548}"
    -c 'transport select swd'
  )
fi

# OWO_BG22_NO_SERVERS=1: no GDB, Tcl or telnet listener, so several OpenOCD
# sessions (e.g. simulator rehearsals) can run side by side without
# colliding on the default ports 3333, 6666 and 4444.
if [ -n "${OWO_BG22_NO_SERVERS:-}" ]; then
  adapter_args+=(-c 'gdb_port disabled' -c 'tcl_port disabled'
                 -c 'telnet_port disabled')
fi

target_args=(
  -f target/silabs/xg21.cfg
  -f "$helpers"
  -c 'adapter speed 1900'
)

# Keep preflight in its own OpenOCD process.  If it fails, no erase command has
# even been parsed in that process, and the existing firmware remains intact.
"$openocd" -s "$scripts" "${adapter_args[@]}" -c 'set WORKAREASIZE 0x800' \
  "${target_args[@]}" \
  -c 'init; if {[catch {bg22_preflight} err]} { echo $err; shutdown error }; shutdown'

if ! "$openocd" -s "$scripts" "${adapter_args[@]}" -c 'set WORKAREASIZE 0x800' \
  "${target_args[@]}" -c "set OWO_RUN_AFTER $run_after" \
  -c 'init; if {[catch {bg22_fast_flash $::env(OWO_BG22_FLASH_IMAGE) $OWO_RUN_AFTER} err]} { echo $err; shutdown error }; shutdown'; then
  echo "fast flash failed after erase; restoring with AP writes" >&2
  "$openocd" -s "$scripts" "${adapter_args[@]}" -c 'set WORKAREASIZE 0' \
    "${target_args[@]}" -c "set OWO_RUN_AFTER $run_after" \
    -c 'init; if {[catch {bg22_slow_recover $::env(OWO_BG22_FLASH_IMAGE) $OWO_RUN_AFTER} err]} { echo $err; shutdown error }; shutdown'
  exit 1
fi
