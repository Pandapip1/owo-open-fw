# Watchdog

**Unvalidated on hardware. Bench-test on a dummy load first.**

## Reset and resource lifecycle

Every output-owning resource starts `Unknown` after reset. C `SYS_INIT` makes an unconditional
ordered safe-off attempt for bridge GPIOs, DAC, switch routing, boost PWM, phase counter, and
supply regulators, then records each acknowledgement. Rust acquires and self-tests each resource
while shut down; only successful resources become `Ready`. BLE and monitored stimulation may not
start while a required resource remains `Unknown` or becomes `Failed`.

* Enabled with `&wdog0 { status = "okay"; }` and the `watchdog0` alias in `boards/bg22_ek4108a.overlay`,
  `CONFIG_WATCHDOG=y`. Zephyr's `drivers/watchdog/wdt_gecko.c` (`silabs,gecko-wdog`) is selected for
  Series 2 by `drivers/watchdog/Kconfig.gecko` (`select SILABS_SISDK_WDOG if SOC_FAMILY_SILABS_S2`).
* A stimulation session holds a counted Rust watchdog lease. Acquiring its first lease calls `owo_wdt_start`
  (modules/owo-stim/shim/owo_shim.c) with a 2000 ms request before a valid frame may arm the real-output
  supervisor; a hardware-output build rejects that frame if acquisition fails. Dropping the final lease calls
  Zephyr's supported `wdt_disable()` only after the ordered bridge/DAC/routing/supply safe-off sequence, so
  idle firmware can enter extended EM4 residency. A lease may carry a safety predicate; acquisition checks it
  before any watchdog-driver call, and every active predicate is checked again before feeding. Any failure
  rejects the lease or moves the resource to `Failed` and requests pump-owned ordered safe-off. A successful
  shutdown records `FailedShutdown`; it does not reboot or silently recover. There is no ambient application
  on/off switch.
  (`wdt_gecko_get_persel_from_timeout`, period `(8 << PERSEL) + 1` cycles of the 1 kHz ULFRCO → 2049 ms),
  `WDT_FLAG_RESET_SOC`, `WDT_OPT_PAUSE_IN_SLEEP` (the driver then clears `em2Run`/`em3Run`).
* Fed only by the pump loop (`src/wdt.rs`) while monitoring is active. The watchdog keeps counting while the
  CPU idles in EM1, so the pump never sleeps longer than 1 s during an armed session; otherwise it may sleep
  indefinitely. OTA teardown holds a temporary lease to bound a wedged Bluetooth shutdown.
* Same configuration as stock 7.9.07: its first WDOG0 write is `CFG = 0x00080000` (PERSEL 8, EM2RUN/EM3RUN 0),
  seen in the QEMU trace (`sim/README.md`), and it feeds from its main loop.
* OTA and safety-reset preparation acquire a temporary lease before disarming the output stage in the pump
  thread. This keeps watchdog containment continuous through the ordered safe-off, optional OTA ATT-response
  settling period, and `bt_disable()` before `sys_reboot`; a stuck teardown is reset-bounded with the output
  already off.
* Not verified: behaviour across the OTA reboot (`sys_reboot` → bootloader). The Gecko Bootloader does not
  start the watchdog in the sources used for the simulator (`sim/bootloader`), but whether WDOG0 is reset by a
  SYSRESETREQ must be confirmed against the EFR32xG22 reference manual (RM) before relying on it.
* Not tested: the build-only target has no radio blobs, so it cannot run in QEMU; the native_sim tests compile the
  stub (`-ENODEV`, logged as "no watchdog on this board") and still pass.
