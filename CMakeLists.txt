cmake_minimum_required(VERSION 3.20.0)

# Reusable stimulation-output subsystem (Kconfig + Rust driver crate).
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/modules/owo-stim)

# `-DOWO_HW=ON` enables the real output stage: devicetree overlay + Kconfig fragment.
# Never set by default; see README ("Bench checklist").
# The board's description of the output stage is boards/<BOARD>_hw.overlay: a custom board only
# needs that file (devicetree nodes for its parts plus one owo,suit-output node), no code changes.
if(OWO_HW)
  set(_hw_overlay ${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}_hw.overlay)
  if(NOT EXISTS ${_hw_overlay})
    message(FATAL_ERROR "-DOWO_HW=ON needs ${_hw_overlay} describing this board's output stage")
  endif()
  list(APPEND EXTRA_DTC_OVERLAY_FILE ${_hw_overlay})
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/owo-suit-hw.conf)
  if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}_hw.conf)
    list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}_hw.conf)
  endif()
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(owo_open_fw)

rust_cargo_application()
