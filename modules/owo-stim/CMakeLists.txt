# C parts of the module: the Zephyr device glue for the Rust DAC driver and the shim that turns
# Zephyr's static-inline / syscall APIs into plain C functions for the Rust crates in rust/.
# The Rust crates themselves are built by the application's rust_cargo_application().
if(CONFIG_OWO_STIM)
  zephyr_library()
  zephyr_library_sources(shim/owo_shim.c)
  zephyr_library_sources_ifdef(CONFIG_DAC_LTC2634 drivers/dac/dac_ltc2634.c)
endif()
