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
EXTRA_MODULES="${ZEPHYR_EXTRA_MODULES:-$ROOT/vendor/zephyr-lang-rust}"

cd "$ROOT"
west build -p always -b bg22_ek4108a -d "$BUILD_DIR" . -- \
  -DCONFIG_BUILD_ONLY_NO_BLOBS=y -DZEPHYR_EXTRA_MODULES="$EXTRA_MODULES" "$@"
