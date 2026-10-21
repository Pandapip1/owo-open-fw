cmake_minimum_required(VERSION 3.20.0)
# Scanner smoke for the EFR32 lower link layer. Kept separate from the TX
# workload so the bounded run has exactly one scheduler-owned radio role.
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_LIST_DIR}/../../modules/efr32-radio
    ${CMAKE_CURRENT_LIST_DIR}/../../vendor/zephyr-lang-rust)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(efr32_radio_rx_port)
rust_cargo_application()
