cmake_minimum_required(VERSION 3.20.0)

# Integration test of the output stage on Zephyr's native_sim: the real shim, the real Zephyr
# drivers (ti,sn74hc595 with read-back, the Rust ltc2634 driver, regulator-fixed, the async ADC
# sequence), emulated GPIO/SPI/ADC/counter, and a small model of the circuit (src/test_hw.c).
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../modules/owo-stim)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(owo_native_sim_test)

target_sources(app PRIVATE src/test_hw.c)
rust_cargo_application()
