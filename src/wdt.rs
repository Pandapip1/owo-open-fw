//! Hardware watchdog (devicetree alias `watchdog0`, wdog0 on the EFR32BG22).
//!
//! The Series 2 WDOG runs from the 1 kHz ULFRCO. Zephyr's `wdt_gecko` driver is told to pause it in
//! EM2/EM3 (`WDT_OPT_PAUSE_IN_SLEEP`), like the stock firmware, which programs WDOG0 CFG with
//! PERSEL=8 (2049 ms) and EM2RUN/EM3RUN clear (seen in a QEMU trace of 7.9.07, sim/README.md).
//! It keeps counting while the CPU idles in EM1, so the pump is woken at least every
//! [`FEED_EVERY_US`] to feed it. Only the pump feeds it: if the pump (or anything it waits on)
//! hangs, the SoC resets and the output stage comes up in its reset state.

use core::ffi::c_int;
use log::{error, info};

extern "C" {
    fn owo_wdt_start(timeout_ms: u32) -> c_int;
    fn owo_wdt_feed() -> c_int;
}

/// Reset timeout. The driver rounds up to a hardware period: 2049 ms.
const TIMEOUT_MS: u32 = 2000;
/// Feed at half the period at the latest.
pub(crate) const FEED_EVERY_US: u64 = 1_000_000;

pub(crate) fn start() {
    // SAFETY: plain Zephyr calls through the C shim; called once from the main thread.
    match unsafe { owo_wdt_start(TIMEOUT_MS) } {
        0 => info!("watchdog started ({} ms)", TIMEOUT_MS),
        -19 => info!("no watchdog on this board"), // -ENODEV
        e => error!("watchdog not started: {}", e),
    }
}

pub(crate) fn feed() {
    // SAFETY: see `start`. Errors (no watchdog configured) are ignored on purpose.
    let _ = unsafe { owo_wdt_feed() };
}

/// Limit a requested wake-up time (`None` = sleep forever) to the next feed deadline.
pub(crate) fn cap_sleep(until: Option<u64>, now_us: u64) -> Option<u64> {
    let latest = now_us + FEED_EVERY_US;
    Some(until.map_or(latest, |t| t.min(latest)))
}
