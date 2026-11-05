# The open radio sequencer's timed-command handler against TIMER0 counts across 2^31 and 2^32.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sequencer_timed_wrap)

# The sequencer image exactly as modules/efr32-radio/CMakeLists.txt builds and embeds it.
set(RADIO ${CMAKE_CURRENT_LIST_DIR}/../../modules/efr32-radio)
set(SEQ_SRC ${RADIO}/sequencer/radio_seq.S)
set(SEQ_LD ${RADIO}/sequencer/link.ld)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/sequencer)
set(SEQ_C ${GEN}/radio-sequencer-image.c)
add_custom_command(
  OUTPUT ${SEQ_C}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
  COMMAND ${CMAKE_C_COMPILER} -mcpu=cortex-m0plus -mthumb -nostdlib
          -I${RADIO}/include
          -Wl,--build-id=none -Wl,-T,${SEQ_LD}
          ${SEQ_SRC} -o ${GEN}/radio-sequencer.elf
  COMMAND ${CMAKE_OBJCOPY} -O binary ${GEN}/radio-sequencer.elf ${GEN}/radio-sequencer.bin
  COMMAND ${PYTHON_EXECUTABLE} ${RADIO}/sequencer/embed_binary.py
          ${GEN}/radio-sequencer.bin ${SEQ_C} open
  DEPENDS ${SEQ_SRC} ${SEQ_LD} ${RADIO}/include/hal/efr32/seq_abi.h
          ${RADIO}/sequencer/embed_binary.py
  VERBATIM
)

target_include_directories(app PRIVATE ${RADIO}/include)
target_sources(app PRIVATE src/main.c ${SEQ_C})
