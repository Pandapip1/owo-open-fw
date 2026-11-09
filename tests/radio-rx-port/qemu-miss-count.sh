# Sourced by the QEMU e2e scripts: every guest's deadline-miss count, read at the end of the run.
#
# A guest built with CONFIG_SCHED_DEADLINE_MISS_COUNT counts a missed, late or unsat deadline
# instead of stopping; every one is a scheduler, budget or model bug, so the run fails on any.
# The run ends by stopping the guests over QMP and reading the counters (qemu-miss-count.py),
# not by killing QEMU. Needs $RUNTIME_DIR. ELFs default to zephyr.elf next to each image; set
# TX_ELF / RX_ELF when the image was repacked (a product flash image).

MISS_QMP_SOCKET="$RUNTIME_DIR/qmp.sock"
MISS_COUNT_FILE="$RUNTIME_DIR/miss-count"
# Relative: a UNIX socket path must stay under 108 bytes. QEMU runs with $RUNTIME_DIR as its
# working directory.
MISS_QMP_ARGS="-qmp unix:qmp.sock,server=on,wait=off"

# End the run at its time: stop the guests, read each ELF's counter, quit QEMU. Arguments: the
# ELF of SoC 0, then SoC 1 (dual machine). Falls back to killing QEMU ($1 is its PID).
miss_count_end() {
	local pid=$1
	shift
	if ! python3 "$(dirname "${BASH_SOURCE[0]}")/qemu-miss-count.py" \
		"$MISS_QMP_SOCKET" "$MISS_COUNT_FILE" "$@" >"$RUNTIME_DIR/miss-count.log" 2>&1; then
		kill -TERM "$pid" 2>/dev/null || return 0
		sleep 1
		kill -KILL "$pid" 2>/dev/null || true
	fi
}

# Print the counts and fail (return 1) unless every counted guest has none. $1: script name.
miss_count_check() {
	local name=$1 soc count failed=0
	if [ ! -s "$MISS_COUNT_FILE" ]; then
		echo "$name: deadline-miss counts could not be read" >&2
		cat "$RUNTIME_DIR/miss-count.log" >&2 2>/dev/null || true
		return 1
	fi
	while read -r soc count; do
		if [ "$count" = - ]; then
			echo "$name: soc$soc deadline misses: not counted (the image stops on a miss)"
			continue
		fi
		echo "$name: soc$soc deadline misses: $count"
		if [ "$count" -ne 0 ]; then
			failed=1
		fi
	done <"$MISS_COUNT_FILE"
	if [ "$failed" -ne 0 ]; then
		echo "$name: deadline misses are scheduler, budget or model bugs" >&2
		return 1
	fi
}
