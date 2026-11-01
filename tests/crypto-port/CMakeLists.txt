# SPDX-License-Identifier: Apache-2.0
# Known-answer, concurrency, timing and stack tests for the RADIOAES driver (modules/efr32-radio/
# crypto) directly and through PSA Crypto, on silicon and in QEMU. prj-software.conf builds the
# same tests against TF-PSA-Crypto's software AES for comparison (docs/crypto.md).
cmake_minimum_required(VERSION 3.20.0)
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../modules/efr32-radio)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(crypto_port)
target_sources(app PRIVATE src/main.c)
