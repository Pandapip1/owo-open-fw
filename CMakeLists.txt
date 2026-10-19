cmake_minimum_required(VERSION 3.20.0)

# Reusable stimulation-output subsystem (Kconfig + Rust driver crate).
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/modules/owo-stim)

# `-DOWO_HW=ON` enables the real output stage: devicetree overlay + Kconfig fragment.
# Never set by default; see README ("Bench checklist").
if(OWO_HW)
  list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/boards/owo-suit-hw.overlay)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/owo-suit-hw.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(owo_open_fw)

rust_cargo_application()
