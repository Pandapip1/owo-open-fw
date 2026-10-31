#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Rehearse the hardware debug and flashing procedures against QEMU, through
# the same OpenOCD configurations as on silicon with only the adapter
# swapped, and check that each behaves as it does on silicon
# (docs/debug-port-model.md):
#   1. read-only mem_ap session (scratch/suit/ro.cfg's target half): DEVINFO
#   2. DCI: silabs_s2_dci_read_se_status
#   3. `reset halt` times out (no reset vector catch on this part)
#   4. scripts/flash-bg22-openocd.sh IMAGE --run: preflight, fast flash
#   5. bg22_reset_to_breakpoint catches main() across SYSRESETREQ
#   6. the suit's raw AHB-AP procedure: FPB on the reset handler, then
#      SYSRESETREQ, reads DHCSR 0x03130003 and the reset PC and MSP
#
# Probe: PROBE=rbb (default) uses QEMU's swd-remote-bitbang probe and
# scripts/sim/qemu-swd.cfg.  PROBE=jlink hands a host UDC to QEMU's
# simulated J-Link (UDC_DRIVER, UDC_DEVICE; JLINK_MODEL ob or edu-mini) and
# uses interface/jlink.cfg with the simulated probe's serial number.
#
# Inputs: BOOT_IMAGE (raw flash image QEMU starts from), IMAGE (ELF to
# flash), QEMU_BIN, optional DEVINFO (a DEVINFO page dump for the machine's
# devinfo= property: the part OpenOCD's flash probe identifies).  Logs go to
# a temporary directory under build/, kept on failure.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
BOOT_IMAGE=${BOOT_IMAGE:?set BOOT_IMAGE to a raw flash image}
IMAGE=${IMAGE:?set IMAGE to the ELF to flash}
PROBE=${PROBE:-rbb}
PORT=${OWO_QEMU_SWD_PORT:-44242}
# Simulated J-Link serial numbers (hw/misc/jlink.c): never a real probe's
JLINK_MODEL=${JLINK_MODEL:-ob}
case "$JLINK_MODEL" in
  ob) JLINK_SERIAL=004242000105 ;;
  edu-mini) JLINK_SERIAL=004242000101 ;;
  *) echo "JLINK_MODEL must be ob or edu-mini" >&2; exit 2 ;;
esac

openocd_out=$(nix build --no-link --print-out-paths "$ROOT#openocd")
openocd="$openocd_out/bin/openocd"
scripts="$openocd_out/share/openocd/scripts"
nm_main=$(nix develop "$ROOT" -c arm-none-eabi-nm "$IMAGE" 2>/dev/null |
          awk '$3 == "main" { print $1 }')
[ -n "$nm_main" ] || { echo "no main() in $IMAGE" >&2; exit 2; }

mkdir -p "$ROOT/build"
RUNTIME_DIR=$(mktemp -d "$ROOT/build/debug-port-e2e.XXXXXX")
qemu_pid=
status=1
cleanup() {
  [ -n "$qemu_pid" ] && kill "$qemu_pid" 2>/dev/null || true
  if [ "$status" -eq 0 ]; then
    rm -rf "$RUNTIME_DIR"
  else
    echo "logs kept in $RUNTIME_DIR" >&2
  fi
}
trap cleanup EXIT HUP INT TERM

machine="efr32bg22,flash=$BOOT_IMAGE"
if [ -n "${DEVINFO:-}" ]; then
  machine="$machine,devinfo=$DEVINFO"
fi
qemu_args=(-M "$machine" -display none -monitor none
           -serial none -icount shift=5 -D "$RUNTIME_DIR/qemu.log")
case "$PROBE" in
  rbb)
    qemu_args+=(-chardev socket,id=swd,host=localhost,port="$PORT",server=on,wait=off
                -device swd-remote-bitbang,chardev=swd)
    adapter=(-f "$ROOT/scripts/sim/qemu-swd.cfg")
    export OWO_BG22_ADAPTER_CFG="$ROOT/scripts/sim/qemu-swd.cfg"
    ;;
  jlink)
    qemu_args+=(-object usb-gadget-host,id=udc0,udc-driver="${UDC_DRIVER:?}",udc-device="${UDC_DEVICE:?}")
    if [ "$JLINK_MODEL" = ob ]; then
      # The OB's VCOM is the kit's console USART1 (xg22_explorer_kit.dtsi
      # zephyr,console = &usart1): a socket pair joins the two line sides.
      qemu_args+=(-chardev socket,id=soc.usart1,host=localhost,port=$((PORT + 1)),server=on,wait=off
                  -chardev socket,id=vcom,host=localhost,port=$((PORT + 1))
                  -device jlink,model=ob,gadget=udc0,vcom=vcom)
    else
      qemu_args+=(-device jlink,model="$JLINK_MODEL",gadget=udc0)
    fi
    adapter=(-f interface/jlink.cfg -c "adapter serial $JLINK_SERIAL"
             -c 'transport select swd')
    export OWO_BG22_JLINK_SERIAL=$JLINK_SERIAL
    ;;
  *) echo "PROBE must be rbb or jlink" >&2; exit 2 ;;
esac

"$QEMU_BIN" "${qemu_args[@]}" > "$RUNTIME_DIR/qemu.out" 2>&1 &
qemu_pid=$!
# The probe is usable once QEMU listens (rbb) or once the host has enumerated
# the simulated J-Link and udev has applied its permissions (jlink).
probe_ready() {
  if [[ $PROBE == jlink ]]; then
    local dev
    for dev in /sys/bus/usb/devices/*; do
      [[ $(cat "$dev/serial" 2>/dev/null) == "$JLINK_SERIAL" ]] || continue
      [[ -w /dev/bus/usb/$(printf %03d "$(cat "$dev/busnum")")/$(printf %03d "$(cat "$dev/devnum")") ]]
      return
    done
    return 1
  fi
  # Look for the listener without connecting: the probe serves one client.
  ss -Hltn "sport = :$PORT" | grep -q .
}
for _ in $(seq 1 100); do
  probe_ready && break
  sleep 0.1
done
probe_ready || { echo "FAIL: probe not ready 10 s after QEMU start"; exit 1; }

ocd() {
  local log=$1
  shift
  "$openocd" -s "$scripts" "${adapter[@]}" "$@" > "$RUNTIME_DIR/$log" 2>&1 ||
    true
}

expect() {
  local log=$1 pattern=$2 what=$3
  if grep -q -- "$pattern" "$RUNTIME_DIR/$log"; then
    echo "PASS: $what"
  else
    echo "FAIL: $what (no '$pattern' in $log)"
    exit 1
  fi
}

# 1. Read-only: identity from DEVINFO through the AHB-AP only
ocd ro.log -f "$ROOT/tests/debug-port/ro-mem-ap.cfg" \
  -c 'init; echo [format "DEVINFO PART 0x%08x" [read_memory 0x0fe08004 32 1]]; shutdown'
expect ro.log "DEVINFO PART 0x" "read-only mem_ap session reads DEVINFO"

# 2. Secure Engine status through the DCI AP
ocd dci.log -f target/silabs/xg21.cfg -c 'init; silabs_s2_dci_read_se_status; shutdown'
expect dci.log "Device erase: Enabled" "DCI SESTATUS: device erase enabled"
expect dci.log "Secure debug: Disabled" "DCI SESTATUS: secure debug disabled"
expect dci.log "Debug lock (hw status): Disabled" "DCI SESTATUS: not locked"

# 3. reset halt: SYSRESETREQ with VC_CORERESET never leaves the core halted
ocd reset-halt.log -f target/silabs/xg21.cfg \
  -c 'init; if {[catch {reset halt} e]} { echo "reset halt failed: $e" }; shutdown'
expect reset-halt.log "timed out while waiting for target halted" \
  "reset halt times out as on silicon"

# 4. The flashing script, unchanged apart from the adapter
"$ROOT/scripts/flash-bg22-openocd.sh" "$IMAGE" --run > "$RUNTIME_DIR/flash.log" 2>&1
expect flash.log "BG22 SRAM execution preflight passed" "preflight"
expect flash.log "BG22 fast erase/write/verify passed" "fast flash"

# 5. FPB breakpoint survives SYSRESETREQ and catches main()
ocd catch.log -c 'set WORKAREASIZE 0x800' -f target/silabs/xg21.cfg \
  -f "$ROOT/scripts/bg22-openocd-flash.cfg" \
  -c "init; bg22_require_sram_execution; bp 0x$nm_main 2 hw; bg22_reset_to_breakpoint; shutdown"
expect catch.log "BG22 reset-to-breakpoint caught: pc (/32): 0x$nm_main" \
  "reset-to-breakpoint catches main() at 0x$nm_main"

# 6. The suit's raw AHB-AP procedure
ocd raw.log -f "$ROOT/tests/debug-port/ro-mem-ap.cfg" \
  -c "source $ROOT/tests/debug-port/raw-catch.tcl" \
  -c 'init; raw_catch vector; shutdown'
expect raw.log "DHCSR-after-reset: 0x03130003" \
  "FPB on the reset handler halts there after SYSRESETREQ (DHCSR 0x03130003)"
pc=$(awk '/^PC:/ { print $2 }' "$RUNTIME_DIR/raw.log")
sp=$(awk '/^SP:/ { print $2 }' "$RUNTIME_DIR/raw.log")
msp=$(awk '/^vector-MSP:/ { print $2 }' "$RUNTIME_DIR/raw.log")
reset=$(awk '/^vector-reset:/ { print $2 }' "$RUNTIME_DIR/raw.log")
if [ $((pc)) -eq $((reset & ~1)) ] && [ "$sp" = "$msp" ]; then
  echo "PASS: halted PC is the reset vector ($pc), SP the initial MSP ($sp)"
else
  echo "FAIL: PC $pc / SP $sp, expected $reset / $msp"
  exit 1
fi
expect raw.log "^PCSR: 0x" "DWT PCSR samples the running core"

status=0
echo "all debug-port procedures behave as on silicon"
