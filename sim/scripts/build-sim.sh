#!/bin/bash
export ZEPHYR_BASE=/tmp/claude/repos/zephyr ZEPHYR_TOOLCHAIN_VARIANT=host BINDGEN_EXTRA_CLANG_ARGS="-Wno-error=implicit-function-declaration"
S=/tmp/claude/ws/simapp; rm -rf $S; mkdir -p $S
cd /home/claude/owo-open-fw-rs && tar --exclude=target --exclude=.git -cf - . | tar -xf - -C $S
cd $S/tests/native-sim && west build -p always -b native_sim/native/64 -d /tmp/claude/sim-build . -- -DZEPHYR_EXTRA_MODULES=/tmp/claude/repos/zephyr-lang-rust-rtfreq "$@" 2>&1
