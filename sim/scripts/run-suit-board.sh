#!/usr/bin/env bash
# Run a flash image on the efr32bg22 machine with the OWO suit board model attached.
#
#   run-suit-board.sh OUTDIR SECONDS FLASH [DEVINFO USERDATA] [-- extra QEMU args]
#
# OUTDIR receives board.vcd (the waveform recording), qemu.log (stderr, including the
# board's script log) and trace.log (bounded event trace). The run stops after SECONDS of
# wall time. Environment:
#   QEMU_BIN   QEMU binary (default build/qemu/qemu-system-arm of this checkout)
#   GDB_PORT   start a gdb stub on this TCP port (the guest starts halted; the caller resumes)
#   SCRIPT     owo-suit "script" property (fault schedule, see hw/misc/owo-suit.c)
#   BOARD_OPTS extra "-global owo-suit.<prop>=<value>" arguments
#   TRACE      trace events (default: GPIO pins, USART SPI bytes, I2C events, WDOG0)
#   VCD_MAX    vcd-max-bytes (default 200 MiB)
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
out=$1 secs=$2 flash=$3
shift 3
devinfo= userdata=
if [ $# -ge 2 ] && [ "$1" != "--" ]; then devinfo=$1 userdata=$2; shift 2; fi
[ "${1:-}" = "--" ] && shift
qemu=${QEMU_BIN:-$here/build/qemu/qemu-system-arm}
mkdir -p "$out"
ulimit -f 1000000 # bound every file QEMU writes (~1 GB)
machine="efr32bg22,flash=$flash,board=owo-suit"
[ -n "$devinfo" ] && machine="$machine,devinfo=$devinfo,userdata=$userdata"
trace=${TRACE:-efr32_gpio_pin,efr32_usart_sync,efr32_i2c_event,efr32_wdog}
args=(-M "$machine" -display none -icount shift=3,sleep=off
	-global "owo-suit.vcd=$out/board.vcd"
	-global "owo-suit.vcd-max-bytes=${VCD_MAX:-209715200}"
	-monitor "unix:$out/monitor.sock,server,nowait"
	-trace "events=/dev/null" -D "$out/trace.log")
for ev in ${trace//,/ }; do args+=(-trace "$ev"); done
[ -n "${SCRIPT:-}" ] && args+=(-global "owo-suit.script=$SCRIPT")
[ -n "${GDB_PORT:-}" ] && args+=(-gdb "tcp:127.0.0.1:$GDB_PORT" -S)
# shellcheck disable=SC2206
[ -n "${BOARD_OPTS:-}" ] && args+=($BOARD_OPTS)
"$qemu" "${args[@]}" "$@" > "$out/qemu.log" 2>&1 &
pid=$!
echo "$pid" > "$out/qemu.pid"
sleep "$secs"
echo quit | socat -t 5 - "UNIX-CONNECT:$out/monitor.sock" > /dev/null 2>&1 || true
for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$pid" 2> /dev/null || break; sleep 1; done
kill "$pid" 2> /dev/null || true
wait "$pid" 2> /dev/null || true
rm -f "$out/monitor.sock"
