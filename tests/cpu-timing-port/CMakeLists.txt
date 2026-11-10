# SPDX-License-Identifier: Apache-2.0
# Cortex-M33 instruction-class and access timing kernels, functions and instruction-cache
# blocks, on silicon and in QEMU (docs/radio-tables.md 7.93).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cpu_timing_port)
target_sources(app PRIVATE src/main.c src/kern33.c)
# The kernels keep their source order between kern33_start and kern33_end (src/kern33.c).
set_source_files_properties(src/kern33.c PROPERTIES COMPILE_OPTIONS -fno-toplevel-reorder)

# The M0+ kernels: their own image, linked at the M0+'s address 0 and embedded as data, as
# modules/efr32-radio does for the radio sequencer.
set(K0_GEN ${CMAKE_CURRENT_BINARY_DIR}/kern0)
add_custom_command(
  OUTPUT ${K0_GEN}/kern0-image.c
  COMMAND ${CMAKE_COMMAND} -E make_directory ${K0_GEN}
  COMMAND ${CMAKE_C_COMPILER} -mcpu=cortex-m0plus -mthumb -masm-syntax-unified -Os -ffreestanding
          -nostdlib -Wall -Wextra -Werror -Wl,--build-id=none
          -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/kern0.ld
          ${CMAKE_CURRENT_SOURCE_DIR}/src/kern0.c -o ${K0_GEN}/kern0.elf
  COMMAND ${CMAKE_OBJCOPY} -O binary ${K0_GEN}/kern0.elf ${K0_GEN}/kern0.bin
  COMMAND ${CMAKE_COMMAND} -DIN=${K0_GEN}/kern0.bin -DOUT=${K0_GEN}/kern0-image.c
          -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  DEPENDS src/kern0.c kern0.ld embed.cmake
  VERBATIM
  COMMENT "Building and embedding the M0+ timing kernels"
)
target_sources(app PRIVATE ${K0_GEN}/kern0-image.c)
