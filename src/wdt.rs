//! Feeding the product's hardware watchdog (WDOG0; docs/watchdog.md).
//!
//! The watchdog itself is configured once at boot and locked by `src/watchdog.c`
//! (`CONFIG_OWO_WATCHDOG`): a fixed period, a warning interrupt that forces the output stage
//! safe, and it runs in every energy mode the product uses. Nothing here can start, stop or
//! reconfigure it.
//!
//! **Who feeds, and what a feed certifies.** Only the pump feeds, and only through [`feed`],
//! which it calls once per loop iteration after its supervision pass: every pulse that was
//! due has been run through the supervisor (each phase with its sense-freshness, overdrive and
//! overrun checks) and the supervisor's dead-man poll has run for the current time. The feed
//! is granted only if
//! 1. every live [`Lease`]'s safety predicate holds (an armed stimulation session holds one),
//!    and
//! 2. the output stage is quiescent: no bridge high side is driven on. Between passes the
//!    bridge must be off; a closed bridge outside a supervised phase means supervision has
//!    already failed.
//!
//! So a feed means "the output-safety supervisor completed a pass and left the output off",
//! not merely "some thread ran". The pump also bounds its own sleep ([`cap_sleep`]) so that,
//! while it works, consecutive feeds are closer together than the warning.

use core::ffi::c_int;
use log::error;
use zephyr::sync::Mutex;

#[cfg(all(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE, not(CONFIG_OWO_WATCHDOG)))]
compile_error!("the real output stage requires the product watchdog (CONFIG_OWO_WATCHDOG)");

extern "C" {
    #[cfg(CONFIG_OWO_WATCHDOG)]
    fn owo_wdt_feed() -> c_int;
    #[cfg(CONFIG_OWO_WATCHDOG)]
    fn owo_wdt_warning_us() -> u32;
    fn owo_suit_output_quiescent() -> c_int;
}

/// The pump sleeps at most this fraction of the time from a feed to the warning; the rest
/// covers its wake-up latency and one supervision pass (measured worst cases and margin in
/// docs/watchdog.md).
const SLEEP_SHARE_DIVISOR: u64 = 2;

/// A predicate attached to a lease. Returning false withholds the feed and makes the pump run
/// its ordered safe-off; the watchdog is the containment if that does not complete.
pub(crate) type SafetyCheck = fn() -> bool;

const MAX_LEASES: usize = 2;

struct State {
    checks: [Option<SafetyCheck>; MAX_LEASES],
    held: [bool; MAX_LEASES],
}

static STATE: Mutex<State> = Mutex::new(State { checks: [None; MAX_LEASES], held: [false; MAX_LEASES] });

/// A safety predicate that must hold for every feed while this exists. Held by a live
/// stimulation session; neither `Clone` nor constructible outside [`Lease::acquire`].
pub(crate) struct Lease(usize);

impl Lease {
    /// The predicate is also an admission condition: unsafe work does not get a lease.
    /// Only a real output stage takes one (a dry-run build has nothing to supervise).
    #[cfg_attr(not(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE), allow(dead_code))]
    pub(crate) fn acquire(safety_check: Option<SafetyCheck>) -> Result<Self, ()> {
        if let Some(check) = safety_check {
            if !check() {
                error!("watchdog safety check failed; refusing lease");
                return Err(());
            }
        }
        let mut state = STATE.lock().unwrap();
        let slot = state.held.iter().position(|h| !h).ok_or(())?;
        state.held[slot] = true;
        state.checks[slot] = safety_check;
        Ok(Self(slot))
    }
}

impl Drop for Lease {
    fn drop(&mut self) {
        let mut state = STATE.lock().unwrap();
        state.held[self.0] = false;
        state.checks[self.0] = None;
    }
}

/// Feed the watchdog if the feed condition (module documentation) holds. Call only from the
/// pump, at the end of a supervision pass. `false` asks the pump to run its ordered safe-off:
/// nothing was fed, so the warning interrupt and the reset remain the containment if that
/// safe-off cannot complete.
pub(crate) fn feed() -> bool {
    // Copy the predicates out: one may be the acquire-time gate of a caller holding STATE.
    let checks = STATE.lock().unwrap().checks;
    for check in checks.into_iter().flatten() {
        if !check() {
            error!("watchdog safety check failed; withholding feed");
            return false;
        }
    }
    // SAFETY: reads the bridge's GPIO output latches.
    if unsafe { owo_suit_output_quiescent() } == 0 {
        error!("bridge closed outside a supervised phase; withholding feed");
        return false;
    }
    #[cfg(CONFIG_OWO_WATCHDOG)]
    // SAFETY: one register access; only this thread feeds.
    unsafe {
        owo_wdt_feed();
    }
    true
}

/// Limit a requested wake-up time (`None` = sleep forever) so the next feed comes before the
/// warning.
pub(crate) fn cap_sleep(until: Option<u64>, now_us: u64) -> Option<u64> {
    #[cfg(CONFIG_OWO_WATCHDOG)]
    {
        // SAFETY: pure computation from build-time constants.
        let latest = now_us + unsafe { owo_wdt_warning_us() } as u64 / SLEEP_SHARE_DIVISOR;
        Some(until.map_or(latest, |t| t.min(latest)))
    }
    #[cfg(not(CONFIG_OWO_WATCHDOG))]
    {
        let _ = (now_us, SLEEP_SHARE_DIVISOR);
        until
    }
}

/// Sleep for `ms` milliseconds in the pump without starving the watchdog: in steps no longer
/// than [`cap_sleep`] allows, feeding after each under the same condition as the pump loop.
/// Returns false as soon as a feed is refused.
pub(crate) fn sleep_fed(ms: u64) -> bool {
    let end = now_us() + ms * 1000;
    loop {
        let now = now_us();
        if now >= end {
            return true;
        }
        let until = cap_sleep(Some(end), now).unwrap_or(end);
        zephyr::time::sleep(zephyr::time::Duration::micros_at_least(until.saturating_sub(now)));
        if !feed() {
            return false;
        }
    }
}

fn now_us() -> u64 {
    crate::now_us()
}
