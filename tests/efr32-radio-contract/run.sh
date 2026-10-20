#!/bin/sh
# Executes without Zephyr, QEMU, or radio MMIO: it tests only abort ownership.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$test_dir/../../build/efr32-radio-contract"
abort_bin="$build_dir/abort_lifecycle_test"
timer_bin="$build_dir/timer_state_test"
rx_pdu_bin="$build_dir/rx_pdu_delivery_test"
mux_bin="$build_dir/cntr_mux_test"
stage_bin="$build_dir/packet_stage_lifecycle_test"
dispatch_bin="$build_dir/completion_dispatch_test"
mkdir -p "$build_dir"
trap 'rm -f "$abort_bin" "$timer_bin" "$rx_pdu_bin" "$mux_bin" "$stage_bin" "$dispatch_bin"; rmdir "$build_dir" 2>/dev/null || :' EXIT HUP INT TERM

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
	-I"$test_dir/../../modules/efr32-radio/hal" \
	"$test_dir/abort_lifecycle_test.c" -o "$abort_bin"
"$abort_bin"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
	-I"$test_dir/../../modules/efr32-radio/hal" \
	"$test_dir/timer_state_test.c" \
	"$test_dir/../../modules/efr32-radio/hal/timer_state.c" -o "$timer_bin"
"$timer_bin"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
	-I"$test_dir/../../modules/efr32-radio/hal" \
	"$test_dir/rx_pdu_delivery_test.c" -o "$rx_pdu_bin"
"$rx_pdu_bin"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
	-I"$test_dir/../../modules/efr32-radio/hal" \
	"$test_dir/cntr_mux_test.c" -o "$mux_bin"
"$mux_bin"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
	-I"$test_dir/../../modules/efr32-radio/hal" \
	"$test_dir/packet_stage_lifecycle_test.c" -o "$stage_bin"
"$stage_bin"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
	-I"$test_dir/../../modules/efr32-radio/hal" \
	"$test_dir/completion_dispatch_test.c" -o "$dispatch_bin"
"$dispatch_bin"
