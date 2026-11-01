cmake_minimum_required(VERSION 3.20.0)

# Out-of-tree board definitions (boards/owo/owo_suit: the suit's control unit).
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})

# Reusable stimulation-output subsystem (Kconfig + Rust driver crate).
# The EFR32 module provides the open split Bluetooth controller; it replaces
# the board's blob-dependent Silicon Labs HCI driver.
list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_LIST_DIR}/modules/owo-stim
  ${CMAKE_CURRENT_LIST_DIR}/modules/efr32-radio
)

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

# The OWO product needs link-layer encryption (pairing protects the output stage). Amateur
# operation refuses encryption (47 CFR 97.113(a)(4)), so this firmware never carries it, in any
# configuration (modules/efr32-radio, docs/amateur-radio.md).
if(CONFIG_EFR32_RADIO_AMATEUR)
  message(FATAL_ERROR "CONFIG_EFR32_RADIO_AMATEUR is not allowed in the OWO product firmware")
endif()

rust_cargo_application()

if(CONFIG_OWO_GECKO_APP_PROPERTIES)
  target_sources(app PRIVATE src/gecko_app_properties.c)
endif()

if(CONFIG_OWO_WATCHDOG)
  target_sources(app PRIVATE src/watchdog.c)
endif()

if(CONFIG_OWO_SIM_INJECT)
  message(WARNING "CONFIG_OWO_SIM_INJECT: test-only simulation mailbox; not for hardware")
  target_sources(app PRIVATE src/sim_inject.c)
endif()
