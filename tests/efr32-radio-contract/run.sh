#!/bin/sh
# Executes without Zephyr, QEMU, or radio MMIO: it tests only abort ownership.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$test_dir/../../build/efr32-radio-contract"
bin="$build_dir/abort_lifecycle_test"
mkdir -p "$build_dir"
trap 'rm -f "$bin"; rmdir "$build_dir" 2>/dev/null || :' EXIT HUP INT TERM

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
	-I"$test_dir/../../modules/efr32-radio/hal" \
	"$test_dir/abort_lifecycle_test.c" -o "$bin"
"$bin"
