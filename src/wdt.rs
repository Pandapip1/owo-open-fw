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
use zephyr::sync::Mutex;

extern "C" {
    fn owo_wdt_start(timeout_ms: u32) -> c_int;
    fn owo_wdt_feed() -> c_int;
    fn owo_wdt_stop() -> c_int;
}

/// Reset timeout. The driver rounds up to a hardware period: 2049 ms.
const TIMEOUT_MS: u32 = 2000;
/// Feed at half the period at the latest.
pub(crate) const FEED_EVERY_US: u64 = 1_000_000;

struct State {
    holders: u8,
    running: bool,
}

/*
 * This is a counted lease, rather than a public on/off switch.  Its mutex
 * serializes GATT, pump, monitor, and OTA callers around the C driver's one
 * installed timeout, so dropping the final lease cannot race a new setup.
 */
static STATE: Mutex<State> = Mutex::new(State { holders: 0, running: false });

/// Keeps the hardware watchdog enabled while it exists.  It is deliberately
/// neither `Clone` nor publicly constructible: supervision is a resource held
/// by a live safety-sensitive operation, not an ambient application setting.
pub(crate) struct Lease(());

impl Lease {
    /// Acquire a watchdog lease before making the output supervisor live.
    pub(crate) fn acquire() -> Result<Self, ()> {
        let mut state = STATE.lock().unwrap();
        if !state.running {
            // SAFETY: serialized plain Zephyr calls through the C shim.
            match unsafe { owo_wdt_start(TIMEOUT_MS) } {
                0 => {
                    info!("watchdog started ({} ms)", TIMEOUT_MS);
                    state.running = true;
                }
                -19 => {
                    info!("no watchdog on this board"); // -ENODEV
                    return Err(());
                }
                e => {
                    error!("watchdog not started: {}", e);
                    return Err(());
                }
            }
        }
        state.holders = state.holders.checked_add(1).ok_or(())?;
        Ok(Self(()))
    }
}

impl Drop for Lease {
    fn drop(&mut self) {
        let mut state = STATE.lock().unwrap();
        debug_assert!(state.holders != 0);
        state.holders -= 1;
        if state.holders != 0 || !state.running {
            return;
        }

        // SAFETY: serialized plain Zephyr call through the C shim.
        match unsafe { owo_wdt_stop() } {
            0 => {
                state.running = false;
                info!("watchdog stopped (final monitoring lease released)");
            }
            e => error!("watchdog not stopped: {}", e),
        }
    }
}

pub(crate) fn feed() {
    let state = STATE.lock().unwrap();
    if state.running {
        // SAFETY: serialized plain Zephyr call through the C shim.
        let _ = unsafe { owo_wdt_feed() };
    }
}

/// Limit a requested wake-up time (`None` = sleep forever) to the next feed deadline.
pub(crate) fn cap_sleep(until: Option<u64>, now_us: u64) -> Option<u64> {
    if !STATE.lock().unwrap().running {
        return until;
    }
    let latest = now_us + FEED_EVERY_US;
    Some(until.map_or(latest, |t| t.min(latest)))
}
