#!/usr/bin/env sh
# Run the output-stage integration test on Zephyr's native_sim (no hardware).
# Needs the pinned submodules (vendor/) and the host's Rust target <arch>-unknown-none (the
# dev shell has it).
# Extra arguments are passed to cmake.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
build="${BUILD_DIR:-$here/build-native-sim}"
v="$here/vendor"
export ZEPHYR_BASE="${ZEPHYR_BASE:-$v/zephyr}"
cmake -GNinja -S "$here/tests/native-sim" -B "$build" -DBOARD=native_sim/native/64 \
	-DZEPHYR_MODULES="$v/cmsis_6;$v/zephyr-lang-rust" "$@"
ninja -C "$build"
exec "$build/zephyr/zephyr.exe"
