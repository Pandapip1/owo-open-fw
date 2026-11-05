cmake_minimum_required(VERSION 3.20.0)
# Transmit power control of modules/efr32-radio (docs/tx-power.md): the controller's HCI power
# commands, the power amplifier level each one programs, and the amateur station's ceiling.
# QEMU run: run-qemu.sh.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../modules/efr32-radio)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(efr32_tx_power_port)
target_sources(app PRIVATE src/main.c)
