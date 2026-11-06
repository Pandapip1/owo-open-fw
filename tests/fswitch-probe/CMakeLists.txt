cmake_minimum_required(VERSION 3.20.0)
# Synthesizer frequency-switch probe (docs/radio-tables.md 7.90): receive only. A passive scanner
# brings the radio to RXSEARCH; the M33 then measures the synthesizer with interrupts locked.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../modules/efr32-radio)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(efr32_fswitch_probe)
target_sources(app PRIVATE src/main.c)
