#!/bin/bash
# run once, report stuck/unmodelled/last PC context
cd /tmp/claude
SRV_ARGS="${SRV_ARGS:-}" ./run-qemu.sh ${1:-25} "${@:2}" 2>&1 | head -3
grep -h "STUCK\|unmodelled" srv.log | sort -u
tail -2 srv.log | cut -c1-160
