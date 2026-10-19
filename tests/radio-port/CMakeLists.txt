cmake_minimum_required(VERSION 3.20.0)
# Compile test for modules/owo-radio-efr32: Zephyr's own split controller on the EFR32BG22.
# UNVALIDATED ON HARDWARE; the radio hooks are stubs. Build with:
#   west build -b bg22_ek4108a tests/radio-port -- -DZEPHYR_EXTRA_MODULES=<repo>/modules/owo-radio-efr32
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../modules/owo-radio-efr32)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(owo_radio_port)
target_sources(app PRIVATE src/main.c)
