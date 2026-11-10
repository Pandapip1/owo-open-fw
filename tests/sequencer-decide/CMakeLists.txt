# The open radio sequencer's staged-reply decision (efr32_seq_conn_decide) run on crafted stages,
# for comparing the C sequencer with the assembly one (-DSEQ_ASM=ON, as
# CONFIG_EFR32_RADIO_SEQ_ASM selects it).
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sequencer_decide)

option(SEQ_ASM "Build the assembly sequencer (radio_seq.S) instead of the C one" OFF)

# The sequencer image exactly as modules/efr32-radio/CMakeLists.txt builds and embeds it.
set(RADIO ${CMAKE_CURRENT_LIST_DIR}/../../modules/efr32-radio)
if(SEQ_ASM)
  set(SEQ_SRC ${RADIO}/sequencer/radio_seq.S)
else()
  set(SEQ_SRC ${RADIO}/sequencer/radio_seq_core.S ${RADIO}/sequencer/radio_seq.c)
endif()
set(SEQ_CFLAGS -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns
               -Wall -Wextra -Werror)
set(SEQ_LD ${RADIO}/sequencer/link.ld)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/sequencer)
set(SEQ_ELF ${GEN}/radio-sequencer.elf)
set(SEQ_C ${GEN}/radio-sequencer-image.c)
set(SEQ_SYMS ${GEN}/seq-syms.h)
string(REGEX REPLACE "objcopy$" "nm" NM ${CMAKE_OBJCOPY})
add_custom_command(
  OUTPUT ${SEQ_C} ${SEQ_SYMS}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
  COMMAND ${CMAKE_C_COMPILER} -mcpu=cortex-m0plus -mthumb -nostdlib ${SEQ_CFLAGS}
          -I${RADIO}/include -I${RADIO}/sequencer
          -Wl,--build-id=none -Wl,-T,${SEQ_LD}
          ${SEQ_SRC} -o ${SEQ_ELF}
  COMMAND ${CMAKE_OBJCOPY} -O binary ${SEQ_ELF} ${GEN}/radio-sequencer.bin
  COMMAND ${PYTHON_EXECUTABLE} ${RADIO}/sequencer/embed_binary.py
          ${GEN}/radio-sequencer.bin ${SEQ_C} open
  COMMAND ${CMAKE_COMMAND} -DNM=${NM} -DELF=${SEQ_ELF} -DOUT=${SEQ_SYMS}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/syms.cmake
  DEPENDS ${SEQ_SRC} ${SEQ_LD} ${RADIO}/include/hal/efr32/seq_abi.h
          ${RADIO}/sequencer/seq_regs.h ${RADIO}/sequencer/embed_binary.py syms.cmake
  VERBATIM
  COMMENT "Building and embedding the radio sequencer for the decision harness"
)

# The M0+ entry the mailbox IRQ is pointed at.
add_custom_command(
  OUTPUT ${GEN}/tramp-image.c
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
  COMMAND ${CMAKE_C_COMPILER} -mcpu=cortex-m0plus -mthumb -O2 -ffreestanding -nostdlib
          -Wall -Wextra -Werror -I${RADIO}/include -I${CMAKE_CURRENT_SOURCE_DIR}/src
          -Wl,--build-id=none -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/tramp.ld
          ${CMAKE_CURRENT_SOURCE_DIR}/src/tramp.c -o ${GEN}/tramp.elf
  COMMAND ${CMAKE_OBJCOPY} -O binary ${GEN}/tramp.elf ${GEN}/tramp.bin
  COMMAND ${CMAKE_COMMAND} -DIN=${GEN}/tramp.bin -DOUT=${GEN}/tramp-image.c
          -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  DEPENDS src/tramp.c src/harness.h tramp.ld embed.cmake ${RADIO}/include/hal/efr32/seq_abi.h
  VERBATIM
  COMMENT "Building and embedding the decision harness's M0+ entry"
)

target_include_directories(app PRIVATE ${RADIO}/include ${GEN} src)
target_sources(app PRIVATE src/main.c ${SEQ_C} ${GEN}/tramp-image.c)
set_source_files_properties(src/main.c PROPERTIES OBJECT_DEPENDS ${SEQ_SYMS})
