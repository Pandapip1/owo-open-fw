#!/usr/bin/env bash
# Run a device under test on soc0 and this HCI UART controller on soc1 of the
# dual EFR32BG22 machine, with the host's /dev/vhci passed through to soc1's
# USART1.  The host kernel registers a new hciN for the emulated controller;
# BlueZ tools then use it like any UART-attached controller, e.g.
#   bluetoothctl -> list / select <addr> / scan le
#   python3 scratch/suit/scan.py with bleak's adapter="hciN"
# The host's other controllers are not touched.  Needs read/write access to
# /dev/vhci (hci_vhci).  Consoles: soc0 USART1 and soc1 USART0 go to files in
# $RUNTIME_DIR.  Runs until interrupted.
#
# usage: run-qemu-vhci.sh <soc0-image.bin> [hci-image.bin]
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
QEMU_BIN=${QEMU_BIN:-"$ROOT/build/qemu/qemu-system-arm"}
VHCI=${VHCI:-/dev/vhci}
DUT_IMAGE=$1
HCI_IMAGE=${2:-"$ROOT/build/hci-uart-port-qemu/zephyr/zephyr.bin"}
RUNTIME_DIR=${RUNTIME_DIR:-"$ROOT/build/hci-uart-port-runtime"}
# Virtual time never runs ahead of real time while the guests idle, so the
# host's real-time HCI and ATT timeouts cover at least as much virtual time
# (sim/README.md, "Timing against a real-time host").
ICOUNT=${ICOUNT:-shift=3,sleep=on}

mkdir -p "$RUNTIME_DIR"
exec "$QEMU_BIN" -M "efr32bg22-dual,flash0=$DUT_IMAGE,flash1=$HCI_IMAGE" \
    -display none -monitor none -icount "$ICOUNT" \
    -chardev "vhci,id=soc1.usart1,path=$VHCI" \
    -chardev "file,id=soc1.usart0,path=$RUNTIME_DIR/soc1-console.log" \
    -chardev "file,id=soc0.usart1,path=$RUNTIME_DIR/soc0-console.log"
