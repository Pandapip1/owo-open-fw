#!/usr/bin/env sh
# Run the output-stage integration test on Zephyr's native_sim (no hardware).
# Needs the west workspace (`west update`), the Zephyr patches applied (scripts/apply-patches.sh),
# and the Rust target:  rustup target add x86_64-unknown-none
# Extra arguments are passed to cmake, e.g. -DZEPHYR_EXTRA_MODULES=<path>.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
build="${BUILD_DIR:-$here/build-native-sim}"
west build -p auto -b native_sim/native/64 -d "$build" "$here/tests/native-sim" -- "$@"
exec "$build/zephyr/zephyr.exe"
