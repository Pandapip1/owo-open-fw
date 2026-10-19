#!/bin/bash
# usage: build.sh <dir> [extra cmake -D args]
export ZEPHYR_BASE=/tmp/claude/repos/zephyr ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb GNUARMEMB_TOOLCHAIN_PATH=/usr BINDGEN_EXTRA_CLANG_ARGS="-D_GCC_ARM_ACLE_H -Wno-error=implicit-function-declaration"
S=/tmp/claude/ws/app; mkdir -p $S
cd /home/claude/owo-open-fw-rs && rm -rf $S/modules $S/src $S/boards && tar --exclude=target --exclude=.git -cf - . | tar -xf - -C $S
cd $S && D=$1; shift
west build -p always -b bg22_ek4108a -d /tmp/claude/$D . -- -DCONFIG_BUILD_ONLY_NO_BLOBS=y -DZEPHYR_EXTRA_MODULES=/tmp/claude/repos/zephyr-lang-rust-rtfreq "$@" 2>&1
