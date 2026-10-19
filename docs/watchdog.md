# Watchdog

**Unvalidated on hardware. Bench-test on a dummy load first.**

* Enabled with `&wdog0 { status = "okay"; }` and the `watchdog0` alias in `boards/bg22_ek4108a.overlay`,
  `CONFIG_WATCHDOG=y`. Zephyr's `drivers/watchdog/wdt_gecko.c` (`silabs,gecko-wdog`) is selected for
  Series 2 by `drivers/watchdog/Kconfig.gecko` (`select SILABS_SISDK_WDOG if SOC_FAMILY_SILABS_S2`).
* Started from `rust_main` through `owo_wdt_start` (modules/owo-stim/shim/owo_shim.c) with a 2000 ms request
  (`wdt_gecko_get_persel_from_timeout`, period `(8 << PERSEL) + 1` cycles of the 1 kHz ULFRCO → 2049 ms),
  `WDT_FLAG_RESET_SOC`, `WDT_OPT_PAUSE_IN_SLEEP` (the driver then clears `em2Run`/`em3Run`).
* Fed only by the pump loop (`src/wdt.rs`). The watchdog keeps counting while the CPU idles in EM1, so the
  pump never sleeps longer than 1 s even with nothing to do (this replaces the earlier "sleep forever" idle).
* Same configuration as stock 7.9.07: its first WDOG0 write is `CFG = 0x00080000` (PERSEL 8, EM2RUN/EM3RUN 0),
  seen in the QEMU trace (`sim/README.md`), and it feeds from its main loop.
* Not verified: behaviour across the OTA reboot (`sys_reboot` → bootloader). The Gecko Bootloader does not
  start the watchdog in the sources used for the simulator (`sim/bootloader`), but whether WDOG0 is reset by a
  SYSRESETREQ must be confirmed against the EFR32xG22 reference manual (RM) before relying on it.
* Not tested: the build-only target has no radio blobs, so it cannot run in QEMU; the native_sim tests compile the
  stub (`-ENODEV`, logged as "no watchdog on this board") and still pass.
