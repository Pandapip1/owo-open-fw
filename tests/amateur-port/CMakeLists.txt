cmake_minimum_required(VERSION 3.20.0)
# Amateur-radio operation of modules/efr32-radio (docs/amateur-radio.md): an advertiser or a
# scanner that switches to amateur rules with a call sign and a non-standard channel plan, plus
# on-target checks of the switch. QEMU dual-machine run: run-qemu-amateur-e2e.sh.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../modules/efr32-radio)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(efr32_amateur_port)
target_sources(app PRIVATE src/main.c)
