//! Feeding the product's hardware watchdog (WDOG0; docs/watchdog.md).
//!
//! The watchdog itself is configured once at boot and locked by `src/watchdog.c`
//! (`CONFIG_OWO_WATCHDOG`): a fixed period, a warning interrupt that forces the output stage
//! safe, and it runs in every energy mode the product uses. Nothing here can start, stop or
//! reconfigure it.
//!
//! **Who feeds, and what a feed certifies.** The feed job (src/jobs.c), a periodic job of its
//! own that cannot be paused, feeds once per period through [`owo_feed_pass`]. The feed is
//! granted only if
//! 1. every live [`Lease`]'s safety predicate holds (an armed stimulation session holds one),
//! 2. the output stage is quiescent: no bridge high side is driven on, and
//! 3. while a session is supervised, the pump's last supervision pass ([`pass_done`]) is
//!    fresh: no older than the supervision interval plus the pump job's own spacing, start-by
//!    and run, the bound the scheduler guarantees.
//!
//! So a feed means "the output-safety supervisor is passing on time and left the output off",
//! not merely "some thread ran". A withheld feed asks the pump for its ordered safe-off; the
//! warning interrupt and the reset remain the containment if that does not complete.

use core::ffi::c_int;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use log::error;
use owo_core::watchdog::feed_fits_window;
use zephyr::sync::Mutex;

#[cfg(all(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE, not(CONFIG_OWO_WATCHDOG)))]
compile_error!("the real output stage requires the product watchdog (CONFIG_OWO_WATCHDOG)");

extern "C" {
    #[cfg(CONFIG_OWO_WATCHDOG)]
    fn owo_wdt_feed() -> c_int;
    #[cfg(CONFIG_OWO_WATCHDOG)]
    fn owo_wdt_warning_us() -> u32;
    fn owo_suit_output_quiescent() -> c_int;
    fn owo_feed_job_start(period_us: u32, deadline_us: u32, budget_us: u32) -> c_int;
    fn owo_feed_job_change(period_us: u32, deadline_us: u32, budget_us: u32) -> c_int;
}

/// The feed job's declaration, as fractions of the time from a feed to the warning: released
/// every half of it, starting within a quarter of it, so a feed always comes before the warning
/// ([`feed_fits_window`], checked before the job is declared).
const PERIOD_SHARE_DIVISOR: u32 = 2;
const DEADLINE_SHARE_DIVISOR: u32 = 4;
/// One feed pass: the predicates and the register write.
pub(crate) const FEED_BUDGET_US: u32 = 200;
/// A release of the feed job is delivered up to the scheduler clock's tolerated lateness late.
const FEED_JITTER_US: u32 = (zephyr::kconfig::CONFIG_CLOCK_EVENT_SCHED_LATE_TOLERANCE_NS / 1000 + 1) as u32;

/// A predicate attached to a lease. Returning false withholds the feed and makes the pump run
/// its ordered safe-off; the watchdog is the containment if that does not complete.
pub(crate) type SafetyCheck = fn() -> bool;

const MAX_LEASES: usize = 2;

struct State {
    checks: [Option<SafetyCheck>; MAX_LEASES],
    held: [bool; MAX_LEASES],
}

static STATE: Mutex<State> = Mutex::new(State { checks: [None; MAX_LEASES], held: [false; MAX_LEASES] });

/// The pump's last supervision pass, in microseconds of uptime (low 32 bits), and the age the
/// feed accepts while a session is supervised (0: none supervised).
static LAST_PASS_US: AtomicU32 = AtomicU32::new(0);
static PASS_LIMIT_US: AtomicU32 = AtomicU32::new(0);
static FEED_WITHHELD: AtomicBool = AtomicBool::new(false);

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

/// Conditions 1 and 2 of the module documentation.
fn predicates_hold() -> bool {
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
    true
}

/// The pump completed a supervision pass at `now_us`; `limit_us` is how old the feed may find it
/// while a session is supervised (`None` when none is). `false` asks for the pump's ordered
/// safe-off now.
pub(crate) fn pass_done(now_us: u64, limit_us: Option<u64>) -> bool {
    LAST_PASS_US.store(now_us as u32, Ordering::Release);
    PASS_LIMIT_US.store(limit_us.map_or(0, |l| l as u32), Ordering::Release);
    predicates_hold() && !FEED_WITHHELD.swap(false, Ordering::AcqRel)
}

/// One run of the feed job (src/jobs.c).
#[no_mangle]
extern "C" fn owo_feed_pass() -> bool {
    let limit = PASS_LIMIT_US.load(Ordering::Acquire);
    let age = (now_us() as u32).wrapping_sub(LAST_PASS_US.load(Ordering::Acquire));
    let fresh = limit == 0 || age <= limit;
    if !fresh {
        error!("supervision pass {} us old, limit {} us; withholding feed", age, limit);
    }
    if !fresh || !predicates_hold() {
        FEED_WITHHELD.store(true, Ordering::Release);
        crate::stop_requested();
        return false;
    }
    #[cfg(CONFIG_OWO_WATCHDOG)]
    // SAFETY: one register access; only the feed job feeds.
    unsafe {
        owo_wdt_feed();
    }
    let now = now_us() as u32;
    let last = LAST_FEED_US.swap(now, Ordering::Relaxed);
    if OWO_FEED_COUNT.fetch_add(1, Ordering::Relaxed) > 0 {
        OWO_FEED_GAP_MAX_US.fetch_max(now.wrapping_sub(last), Ordering::Relaxed);
    }
    true
}

/// Feeds and the longest time between two, for reading over a debugger (the bound is the feed
/// declaration's period, jitter, start-by and run; the warning comes at `warning_us()`).
#[no_mangle]
static OWO_FEED_COUNT: AtomicU32 = AtomicU32::new(0);
#[no_mangle]
static OWO_FEED_GAP_MAX_US: AtomicU32 = AtomicU32::new(0);
static LAST_FEED_US: AtomicU32 = AtomicU32::new(0);

/// Microseconds from a feed to the warning.
fn warning_us() -> u32 {
    #[cfg(CONFIG_OWO_WATCHDOG)]
    // SAFETY: pure computation from build-time constants.
    return unsafe { owo_wdt_warning_us() };
    #[cfg(not(CONFIG_OWO_WATCHDOG))]
    return u32::MAX / 2;
}

/// How late past its poll interval the feed accepts a supervision pass from a pump that is not
/// a scheduler job: the feed's own start-by.
#[allow(dead_code)]
pub(crate) fn pass_slack_us() -> u64 {
    (warning_us() / DEADLINE_SHARE_DIVISOR) as u64
}

/// Declare and start the feed job. Refused (and reported) when its declaration would let the
/// warning come first: then nothing feeds and the watchdog is the containment.
pub(crate) fn start_feed_job() -> Result<(), ()> {
    let warning = warning_us();
    let period = warning / PERIOD_SHARE_DIVISOR;
    let deadline = warning / DEADLINE_SHARE_DIVISOR;
    if !feed_fits_window(
        period as u64,
        FEED_JITTER_US as u64,
        deadline as u64,
        FEED_BUDGET_US as u64,
        warning as u64,
    ) {
        error!("feed declaration outside the watchdog window");
        return Err(());
    }
    // SAFETY: once, before the feed job's first release.
    if unsafe { owo_feed_job_start(period, deadline, FEED_BUDGET_US) } != 0 {
        return Err(());
    }
    Ok(())
}

/// Change the feed job's period and start-by (a scheduler change request, taking effect at its
/// next release). WDOG0's window is fixed once locked, so only the feed's timing can change, and
/// only inside it: a declaration that lets the warning come first is refused here.
#[allow(dead_code)]
pub(crate) fn change_feed(period_us: u32, deadline_us: u32) -> Result<(), ()> {
    if !feed_fits_window(
        period_us as u64,
        FEED_JITTER_US as u64,
        deadline_us as u64,
        FEED_BUDGET_US as u64,
        warning_us() as u64,
    ) {
        return Err(());
    }
    // SAFETY: a thread-level kernel request on the feed job's static event.
    match unsafe { owo_feed_job_change(period_us, deadline_us, FEED_BUDGET_US) } {
        0 => Ok(()),
        _ => Err(()),
    }
}

fn now_us() -> u64 {
    crate::now_us()
}
