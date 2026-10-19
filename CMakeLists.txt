cmake_minimum_required(VERSION 3.20.0)

# Reusable stimulation-output subsystem (Kconfig + Rust driver crate).
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/modules/owo-stim)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(owo_open_fw)

rust_cargo_application()
