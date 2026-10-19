#!/usr/bin/env bash
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export ZEPHYR_BASE="${ZEPHYR_BASE:-$ROOT/vendor/zephyr}"
export ZEPHYR_TOOLCHAIN_VARIANT=host
export BINDGEN_EXTRA_CLANG_ARGS="${BINDGEN_EXTRA_CLANG_ARGS:--Wno-error=implicit-function-declaration}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/native-sim}"
EXTRA_MODULES="${ZEPHYR_EXTRA_MODULES:-$ROOT/vendor/zephyr-lang-rust}"

cd "$ROOT/tests/native-sim"
west build -p always -b native_sim/native/64 -d "$BUILD_DIR" . -- \
  -DZEPHYR_EXTRA_MODULES="$EXTRA_MODULES" "$@"
