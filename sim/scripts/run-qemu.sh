#!/bin/bash
# usage: run-qemu.sh <seconds> [extra qemu args]
#   env: FLASH=<image> SRV_ARGS=<server args>; server trace in /tmp/claude/srv.log
cd /tmp/claude
T=$1; shift
FLASH=${FLASH:-flash-stock.bin}
rm -f efr32.sock
python3 repos/qemu/tools/efr32bg22/server.py --flash $FLASH ${SRV_ARGS:---trace} --sock /tmp/claude/efr32.sock > srv.log 2>&1 &
SP=$!
sleep 1
timeout $T repos/qemu/build/qemu-system-arm -M efr32bg22,flash=$FLASH -display none -monitor none \
  -serial unix:/tmp/claude/efr32.sock,server=off -icount shift=5 -d guest_errors,int -D qemu.log "$@"
kill $SP 2>/dev/null
