//! OWO suit open firmware, proof of concept: Zephyr + Rust.
//!
//! BLE peripheral exposing the stock GATT database; stimulation is dry-run only.
#![no_std]

mod ble;
mod hal;
mod ota;
#[cfg(CONFIG_OWO_SIM_INJECT)]
mod sim_inject;
mod wdt;

use core::sync::atomic::{AtomicBool, Ordering};
use log::{error, info};
use owo_core::engine::{Engine, Next, LEVEL_CAP};
use owo_core::proto::{self, run, Config, Frame};
use zephyr::sync::Mutex;
use zephyr::sys::sync::Semaphore;
use zephyr::time::{Duration, Forever, SYS_FREQUENCY};
use zephyr_panic as _;

/// Fallback serial (build with `OWO_SERIAL=<yours>`), used when the board describes no
/// manufacturing record or the record holds no valid serial (see [`serial`]).
const BUILD_SERIAL: &str = match option_env!("OWO_SERIAL") {
    Some(s) => s,
    None => "000000000000",
};

/// A serial is twelve characters: what the unit's record holds and src/serial.c writes.
pub(crate) const SERIAL_LEN: usize = 12;
const _: () = assert!(BUILD_SERIAL.len() <= SERIAL_LEN, "OWO_SERIAL is at most 12 characters");

extern "C" {
    fn owo_serial_read(out: *mut u8) -> core::ffi::c_int;
    #[cfg(CONFIG_PM)]
    fn owo_deep_sleep_inhibit(inhibit: bool);
}

/// Keeps the part out of EM2 while it exists (src/deep_sleep.c): EM2 stops the output stage's
/// clocks and the supervisor's cadence, so an armed session holds one.
struct DeepSleepInhibit;

impl DeepSleepInhibit {
    fn new() -> Self {
        // SAFETY: a lock count in the power manager; taken once per value.
        #[cfg(CONFIG_PM)]
        unsafe {
            owo_deep_sleep_inhibit(true)
        };
        Self
    }
}

impl Drop for DeepSleepInhibit {
    fn drop(&mut self) {
        // SAFETY: releases the count `new` took.
        #[cfg(CONFIG_PM)]
        unsafe {
            owo_deep_sleep_inhibit(false)
        };
    }
}

pub(crate) struct Serial {
    buf: [u8; SERIAL_LEN],
    len: usize,
}

impl Serial {
    pub(crate) fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.len]
    }
}

/// Serial string hashed into the pairing passkey and named in the advertised name ("OWO" and the
/// serial): the unit's own record when its board describes one holding twelve digits (USERDATA on
/// the suit, src/serial.c), else `OWO_SERIAL`. Stock units derive the BLE passkey from it; which
/// exact string the official app hashes is not confirmed, so compare with your unit.
pub(crate) fn serial() -> Serial {
    let mut s = Serial { buf: [0; SERIAL_LEN], len: SERIAL_LEN };
    // SAFETY: the buffer has room for the twelve bytes the function writes.
    if unsafe { owo_serial_read(s.buf.as_mut_ptr()) } != 0 {
        s.buf[..BUILD_SERIAL.len()].copy_from_slice(BUILD_SERIAL.as_bytes());
        s.len = BUILD_SERIAL.len();
    }
    s
}

/// Everything the GATT callbacks and the pump share.
pub(crate) struct State {
    engine: Engine,
    cfg: Config,
    out: hal::Output,
    /// Held exactly while the stimulation supervisor is armed: its safety predicate must
    /// hold for every watchdog feed (src/wdt.rs).
    stim_watchdog: Option<wdt::Lease>,
    /// Held with `stim_watchdog` while an output session is armed (a dry run, with nothing
    /// to supervise, lets go at the pump's next pass).
    deep_sleep: Option<DeepSleepInhibit>,
    /// Last-written values, returned on read (the stock firmware keeps them in RAM).
    pub v_widths: [u8; proto::WIDTHS_LEN],
    pub v_frame: [u8; proto::FRAME_LEN],
    pub v_power: [u8; proto::POWER_LEN],
    pub v_extra1: [u8; proto::CONFIG_LEN],
    pub v_run: u8,
    pub v_test: [u8; proto::FRAME_LEN],
    pub v_extra2: [u8; 10],
}

pub(crate) static STATE: Mutex<State> = Mutex::new(State {
    engine: Engine::new(),
    cfg: Config {
        duration: 1,
        freq_hz: 0,
        work: 0,
        rest: 0,
        ramp_up_raw: 0,
        ramp_down_raw: 0,
        xgatt: 0,
        aux7: 0,
        aux8: 0,
    },
    out: hal::Output::new(),
    stim_watchdog: None,
    deep_sleep: None,
    v_widths: [0; proto::WIDTHS_LEN],
    v_frame: [0; proto::FRAME_LEN],
    v_power: [0; proto::POWER_LEN],
    v_extra1: [0; proto::CONFIG_LEN],
    v_run: 0,
    v_test: [0; proto::FRAME_LEN],
    v_extra2: [0; 10],
});

/// Given whenever the engine may have new work; the pump sleeps on it.
static WAKE: Semaphore = Semaphore::new(0, 1);

// Once reset preparation starts, no callback may re-arm the supervisor or restart
// advertising while the pump is tearing the system down.
static RESETTING: AtomicBool = AtomicBool::new(false);

pub(crate) fn resetting() -> bool {
    RESETTING.load(Ordering::Acquire)
}

// A stop requested from outside the pump (power button, loss of the control link): the pump
// locks the output out on its next pass.
static STOP_PENDING: AtomicBool = AtomicBool::new(false);
// The control link is gone for good (the radio shut down): no frame or Start is accepted again.
static LINK_DOWN: AtomicBool = AtomicBool::new(false);

/// Wake the pump to lock the output out. Lock-free and non-blocking: callable from interrupts.
pub(crate) fn stop_requested() {
    STOP_PENDING.store(true, Ordering::Release);
    WAKE.give();
}

/// The radio shut down (modules/efr32-radio `radio_shutdown.h`): called once, in the context
/// that detected the failure, which may be an interrupt.
#[cfg(CONFIG_BT_LLL_VENDOR_EFR32)]
extern "C" fn control_link_lost() {
    LINK_DOWN.store(true, Ordering::Release);
    #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
    owo_suit::request_stop();
    stop_requested();
}

#[cfg(CONFIG_BT_LLL_VENDOR_EFR32)]
extern "C" {
    fn efr32_radio_shutdown_cb_set(cb: Option<extern "C" fn()>);
}

fn link_down() -> bool {
    LINK_DOWN.load(Ordering::Acquire)
}

/*
 * This is the admission and feed condition for a live stimulation session.
 * Its lifecycle owns the stricter output invariant: the monitor drops the
 * lease immediately after its ordered safe-off path.  Do not take STATE here:
 * a frame acquires its lease while holding that mutex, and the predicate must
 * be usable as an acquire-time race gate.
 */
#[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
fn stim_watchdog_safe() -> bool {
    !resetting()
}

pub(crate) fn now_us() -> u64 {
    // SAFETY: plain kernel query.
    let ticks = unsafe { zephyr::raw::k_uptime_ticks() } as u64;
    ticks * 1_000_000 / SYS_FREQUENCY as u64
}

pub(crate) fn passkey() -> u32 {
    proto::pairing_code(serial().as_bytes())
}

// ---- application hooks called from the GATT layer -------------------------

pub(crate) fn on_frame(buf: &[u8]) -> Result<(), proto::Error> {
    if resetting() || link_down() {
        return Err(proto::Error::Range);
    }
    let f = Frame::parse(buf)?;
    let mut s = STATE.lock().unwrap();
    // Do not arm an electrically live session unless its feed predicate is registered first.
    #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
    if s.stim_watchdog.is_none() {
        s.stim_watchdog = Some(wdt::Lease::acquire(Some(stim_watchdog_safe)).map_err(|_| {
            error!("refusing stimulation frame: watchdog unavailable");
            proto::Error::Range
        })?);
    }
    if s.deep_sleep.is_none() {
        s.deep_sleep = Some(DeepSleepInhibit::new());
    }
    let now = now_us();
    s.out.frame_received(now, f.total as u64 * 100_000, f.first);
    s.engine.push(&f, now).map_err(|_| proto::Error::Range)?; // queue full
    drop(s);
    WAKE.give();
    Ok(())
}

pub(crate) fn on_config(buf: &[u8]) -> Result<(), proto::Error> {
    let mut s = STATE.lock().unwrap();
    // stored only; its effect on stock behaviour is not yet understood
    s.cfg = Config::parse(buf, Some(&s.cfg))?;
    Ok(())
}

pub(crate) fn on_power(buf: &[u8]) -> Result<(), proto::Error> {
    let mut lim = proto::power_parse(buf)?;
    for l in lim.iter_mut() {
        *l = (*l).min(LEVEL_CAP);
    }
    STATE.lock().unwrap().engine.set_limits(&lim);
    Ok(())
}

pub(crate) fn on_widths(buf: &[u8]) -> Result<(), proto::Error> {
    let w = proto::widths_parse(buf)?;
    STATE.lock().unwrap().engine.set_widths(&w);
    Ok(())
}

/// `Err(())` for an unknown command.
pub(crate) fn on_run(cmd: u8) -> Result<(), ()> {
    if resetting() || link_down() {
        return Err(());
    }
    match cmd {
        run::STOP | run::ABORT => stop_all(),
        // Stock apps send 0x25 on connect; the stock firmware treats it as "ready"
        // and (per the app) answers on Extra 2. Record shape is from the app
        // decode, not verified on air.
        run::START | run::RESTART => {
            STATE.lock().unwrap().out.user_start(now_us());
            ble::notify_extra2(&[0, 0, 0, 1, 0, 0, 0, 0, 2, 0])
        }
        run::STATE9 => {}
        _ => return Err(()),
    }
    Ok(())
}

/// The OTA control point was written with "start": everything off now, then the pump reboots
/// into the Silicon Labs bootloader (which receives the image).
pub(crate) fn on_ota_start() {
    stop_all();
    ota::request();
    WAKE.give();
}

pub(crate) fn stop_all() {
    let mut s = STATE.lock().unwrap();
    s.engine.stop();
    s.out.all_off();
    let watchdog = s.stim_watchdog.take();
    let deep_sleep = s.deep_sleep.take();
    drop(s);
    drop(watchdog);
    drop(deep_sleep);
}

/// Establish the application safety baseline after every reset.
///
/// This is deliberately separate from an operational Stop: a reset can leave external output
/// components powered while the MCU is only just becoming able to control them.  `init` puts the
/// backend's POST/forced initialization puts its pins and peripherals in their known state, and
/// the immediately following `all_off` runs the normal, ordered bridge/DAC/routing/supply
/// shutdown as well. Keep this before BLE startup so no safety-critical service can accept
/// work first. The watchdog has run since PRE_KERNEL_2 (src/watchdog.c).
fn early_boot_baseline() -> Result<(), ()> {
    let mut s = STATE.lock().unwrap();
    s.engine.stop();
    s.out.init()?;
    s.out.all_off();
    Ok(())
}

/// Quiesce the application before an intentional reset.
///
/// This is deliberately called only by the pump: regulator, GPIO, and Bluetooth
/// teardown may sleep, so neither a GATT callback nor an interrupt is a
/// suitable context. The pump owns the same `STATE` lock and completes
/// its bounded critical section before this obtains it.
pub(crate) fn prepare_reset() {
    RESETTING.store(true, Ordering::Release);
    stop_all();
}

// ---- entry point -----------------------------------------------------------

#[no_mangle]
extern "C" fn rust_main() {
    // SAFETY: runs once at startup before any other Rust thread exists.
    unsafe {
        zephyr::set_logger().unwrap();
    }
    // The feed job feeds from now on while the output stays quiescent (src/wdt.rs).
    if wdt::start_feed_job().is_err() {
        error!("watchdog feed job refused; the watchdog resets the part");
    }
    // C reset safe-off has already run from SYS_INIT. Rust's readiness check
    // and forced safe-off must complete before enabling work.
    if early_boot_baseline().is_err() {
        /*
         * A watchdog reset cannot run teardown code.  The C SYS_INIT
         * best-effort baseline has already attempted every electrical
         * safe-off action; do not bring up BLE or normal work unless the
         * reconstructed drivers also acknowledged their shutdown state.
         * The feed job keeps feeding while the output stays quiescent: a reset loop would
         * pass through the reset window (pins floating) again and again. Once the bridge is
         * on it withholds the feed, and the warning interrupt and the reset take over.
         */
        error!("output shutdown baseline unverified; bring-up halted");
        loop {
            zephyr::time::sleep(Forever);
        }
    }
    // Before the link can exist: a radio shutdown stops the output it controls.
    // SAFETY: registers a function that is safe in any context.
    #[cfg(CONFIG_BT_LLL_VENDOR_EFR32)]
    unsafe {
        efr32_radio_shutdown_cb_set(Some(control_link_lost))
    };
    if let Err(e) = ble::start() {
        error!("BLE start failed: {}", e);
        return;
    }
    let serial = serial();
    info!(
        "pairing passkey for serial '{}': {:06}",
        core::str::from_utf8(serial.as_bytes()).unwrap_or("?"),
        passkey()
    );

    // Pump: tickless. Sleep until the engine's next wake-up or a GATT write gives the
    // semaphore. It is the output-safety supervisor; the feed job checks its passes are on
    // time (src/wdt.rs).
    loop {
        ota::reboot_if_requested();
        let (wake, poll, watchdog) = {
            let mut s = STATE.lock().unwrap();
            let wake = loop {
                if STOP_PENDING.swap(false, Ordering::AcqRel) {
                    s.engine.stop();
                    s.out.lock_out();
                }
                let now = now_us();
                match s.engine.next(now) {
                    Next::Pulse(p) => s.out.pulse(&p, now),
                    Next::Wake(t) => break Some(t),
                    Next::Idle => {
                        s.out.idle();
                        break None;
                    }
                }
            };
            s.out.poll(now_us());
            let ended = !s.out.monitoring_required();
            let watchdog = ended.then(|| s.stim_watchdog.take()).flatten();
            let deep_sleep = ended.then(|| s.deep_sleep.take()).flatten();
            (wake, s.out.poll_interval_us(), (watchdog, deep_sleep))
        };
        // The supervisor has already completed its ordered safe-off path; its feed predicate
        // and its hold on EM1 go with the session.
        drop(watchdog);
        // The supervision pass is complete. While a session is supervised the feed accepts it
        // up to a poll interval plus the feed's own start-by (wdt::pass_slack_us()): the pump
        // is not a scheduler job here, so nothing tighter is admitted for its wake-up.
        let limit = poll.map(|p| p + wdt::pass_slack_us());
        if !wdt::pass_done(now_us(), limit) {
            // A monitored resource whose predicate fails is failed, not rebooted: stop every
            // output now; its lifecycle remains failed until an explicit, separately
            // authorized recovery. The watchdog is the containment if this does not complete.
            #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
            owo_suit::revoke();
            stop_all();
        }
        // Sleep until the next pulse, but never longer than the supervision interval while
        // the output is armed (the dead-man must be able to fire without any BLE traffic).
        let until = match (wake, poll) {
            (Some(t), Some(p)) => Some(t.min(now_us() + p)),
            (Some(t), None) => Some(t),
            (None, Some(p)) => Some(now_us() + p),
            (None, None) => None,
        };
        match until {
            None => {
                let _ = WAKE.take(Forever);
            }
            Some(t) => {
                let us = t.saturating_sub(now_us());
                let ticks = (us * SYS_FREQUENCY as u64).div_ceil(1_000_000);
                let _ = WAKE.take(Duration::from_ticks(ticks.max(1) as _));
            }
        }
    }
}
