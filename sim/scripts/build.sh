#!/usr/bin/env bash
set -eu
# usage: build.sh <dir> [extra cmake -D args]
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export ZEPHYR_BASE="${ZEPHYR_BASE:-$ROOT/vendor/zephyr}"
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH="${GNUARMEMB_TOOLCHAIN_PATH:-/usr}"
export BINDGEN_EXTRA_CLANG_ARGS="${BINDGEN_EXTRA_CLANG_ARGS:--D_GCC_ARM_ACLE_H -Wno-error=implicit-function-declaration}"
D=$1
shift
BUILD_DIR="${BUILD_DIR:-$ROOT/build/$D}"
MODULES="${ZEPHYR_MODULES:-$ROOT/vendor/cmsis_6;$ROOT/vendor/hal_silabs;$ROOT/vendor/mbedtls;$ROOT/vendor/tf-psa-crypto;$ROOT/vendor/zephyr-lang-rust}"

# No west workspace: the submodules are the Zephyr tree.
rm -rf "$BUILD_DIR"
cmake -GNinja -S "$ROOT" -B "$BUILD_DIR" -DBOARD=bg22_ek4108a -DZEPHYR_MODULES="$MODULES" "$@"
ninja -C "$BUILD_DIR" ${NINJA_JOBS:+-j "$NINJA_JOBS"}
