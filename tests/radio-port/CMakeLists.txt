cmake_minimum_required(VERSION 3.20.0)
# Compile test for modules/efr32-radio: Zephyr's own split controller on the EFR32BG22.
# UNVALIDATED ON HARDWARE; the radio hooks are stubs. Build with:
#   west build -b bg22_ek4108a tests/radio-port -- -DZEPHYR_EXTRA_MODULES=<repo>/modules/efr32-radio
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../modules/efr32-radio)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(efr32_radio_port)
rust_cargo_application()
