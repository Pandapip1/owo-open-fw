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

common_args=(
  -s "$scripts"
  -f interface/jlink.cfg
  -c 'transport select swd'
  -f target/silabs/xg21.cfg
  -f "$helpers"
  -c 'adapter speed 1900'
)

# Keep preflight in its own OpenOCD process.  If it fails, no erase command has
# even been parsed in that process, and the existing firmware remains intact.
"$openocd" "${common_args[@]:0:4}" -c 'set WORKAREASIZE 0x800' \
  "${common_args[@]:4}" \
  -c 'init; if {[catch {bg22_preflight} err]} { echo $err; shutdown error }; shutdown'

if ! "$openocd" "${common_args[@]:0:4}" -c 'set WORKAREASIZE 0x800' \
  "${common_args[@]:4}" -c "set OWO_RUN_AFTER $run_after" \
  -c 'init; if {[catch {bg22_fast_flash $::env(OWO_BG22_FLASH_IMAGE) $OWO_RUN_AFTER} err]} { echo $err; shutdown error }; shutdown'; then
  echo "fast flash failed after erase; restoring with AP writes" >&2
  "$openocd" "${common_args[@]:0:4}" -c 'set WORKAREASIZE 0' \
    "${common_args[@]:4}" -c "set OWO_RUN_AFTER $run_after" \
    -c 'init; if {[catch {bg22_slow_recover $::env(OWO_BG22_FLASH_IMAGE) $OWO_RUN_AFTER} err]} { echo $err; shutdown error }; shutdown'
  exit 1
fi
