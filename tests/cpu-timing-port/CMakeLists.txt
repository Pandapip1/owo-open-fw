# SPDX-License-Identifier: Apache-2.0
# Cortex-M33 instruction-class and access timing kernels, functions and instruction-cache
# blocks, on silicon and in QEMU (docs/radio-tables.md 7.93).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cpu_timing_port)
target_sources(app PRIVATE src/main.c src/kern33.S src/kern0.S)
