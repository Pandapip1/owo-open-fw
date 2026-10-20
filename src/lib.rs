//! OWO suit open firmware, proof of concept: Zephyr + Rust.
//!
//! BLE peripheral exposing the stock GATT database; stimulation is dry-run only.
#![no_std]

mod ble;
mod hal;
mod ota;
mod wdt;

use core::sync::atomic::{AtomicBool, Ordering};
use log::{error, info};
use owo_core::engine::{Engine, Next, LEVEL_CAP};
use owo_core::proto::{self, run, Config, Frame};
use zephyr::sync::Mutex;
use zephyr::sys::sync::Semaphore;
use zephyr::time::{Duration, Forever, SYS_FREQUENCY};
use zephyr_panic as _;

/// Serial string hashed into the pairing passkey (build with `OWO_SERIAL=<yours>`).
/// Stock units derive the BLE passkey from it; which exact string the official app
/// hashes is not confirmed, so set it to your unit's serial and compare.
const SERIAL: &str = match option_env!("OWO_SERIAL") {
    Some(s) => s,
    None => "000000000000",
};

/// Everything the GATT callbacks and the pump share.
pub(crate) struct State {
    engine: Engine,
    cfg: Config,
    out: hal::Output,
    /// Held exactly while the stimulation supervisor is armed.  Its Drop
    /// implementation is the sole normal path that stops the hardware WDT.
    stim_watchdog: Option<wdt::Lease>,
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

// The C monitor thread is created statically and may run before `rust_main` has
// initialized the output backend. It must be completely inert until then.
static STIM_MONITOR_READY: AtomicBool = AtomicBool::new(false);

// Once reset preparation starts, no callback may re-arm the supervisor or restart
// advertising while the pump is tearing the system down.
static RESETTING: AtomicBool = AtomicBool::new(false);

pub(crate) fn resetting() -> bool {
    RESETTING.load(Ordering::Acquire)
}

/*
 * This is the admission and feed condition for a live stimulation session.
 * Its lifecycle owns the stricter output invariant: the monitor drops the
 * lease immediately after its ordered safe-off path.  Do not take STATE here:
 * a frame acquires its lease while holding that mutex, and the predicate must
 * be usable as an acquire-time race gate.
 */
fn stim_watchdog_safe() -> bool {
    !resetting()
}

fn now_us() -> u64 {
    // SAFETY: plain kernel query.
    let ticks = unsafe { zephyr::raw::k_uptime_ticks() } as u64;
    ticks * 1_000_000 / SYS_FREQUENCY as u64
}

/// Entry point for the persistent Zephyr safety-monitor job in `owo_shim.c`.
///
/// This deliberately shares the application mutex with the pump: `Output`, the
/// supervisor state, and the hardware handles have one owner at a time.  If a
/// pulse is in progress, Zephyr priority inheritance lets that bounded critical
/// section finish; the pulse's own feedback/freshness/overrun checks continue
/// to protect the live bridge.  The job then applies the dead-man safe-off path
/// without waiting for radio, BLE, or the pump's next wake-up.
#[no_mangle]
pub extern "C" fn owo_stim_monitor_poll() {
    #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
    {
        if !STIM_MONITOR_READY.load(Ordering::Acquire) {
            return;
        }
        let mut s = STATE.lock().unwrap();
        s.out.poll(now_us());
        // A dead-man expiry disarms only after Output has run its ordered
        // safe-off path. Releasing the session lease here permits EM4 again.
        let watchdog = (!s.out.monitoring_required())
            .then(|| s.stim_watchdog.take())
            .flatten();
        drop(s);
        drop(watchdog);
    }
}

pub(crate) fn passkey() -> u32 {
    proto::pairing_code(SERIAL.as_bytes())
}

// ---- application hooks called from the GATT layer -------------------------

pub(crate) fn on_frame(buf: &[u8]) -> Result<(), proto::Error> {
    if resetting() {
        return Err(proto::Error::Range);
    }
    let f = Frame::parse(buf)?;
    let mut s = STATE.lock().unwrap();
    // Do not arm an electrically live session unless its reset-bounded
    // monitor lease exists first.  Dry-run builds deliberately have no WDT.
    #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
    if s.stim_watchdog.is_none() {
        s.stim_watchdog = Some(wdt::Lease::acquire(Some(stim_watchdog_safe)).map_err(|_| {
            error!("refusing stimulation frame: watchdog unavailable");
            proto::Error::Range
        })?);
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
    if resetting() {
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
    drop(s);
    drop(watchdog);
}

/// Establish the application safety baseline after every reset.
///
/// This is deliberately separate from an operational Stop: a reset can leave external output
/// components powered while the MCU is only just becoming able to control them.  `init` puts the
/// backend's POST/forced initialization puts its pins and peripherals in their known state, and
/// the immediately following `all_off` runs the normal, ordered bridge/DAC/routing/supply
/// shutdown as well. Keep this before BLE startup (and before the watchdog is armed) so no
/// safety-critical service can accept work first.
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
/// teardown may sleep, so neither a GATT callback nor the safety-monitor thread
/// is a suitable context. A monitor poll already in progress shares `STATE` and
/// must finish its bounded critical section before this obtains the lock.
pub(crate) fn prepare_reset() {
    RESETTING.store(true, Ordering::Release);
    STIM_MONITOR_READY.store(false, Ordering::Release);
    stop_all();
}

// ---- entry point -----------------------------------------------------------

#[no_mangle]
extern "C" fn rust_main() {
    // SAFETY: runs once at startup before any other Rust thread exists.
    unsafe {
        zephyr::set_logger().unwrap();
    }
    // C reset safe-off has already run from SYS_INIT. Rust's readiness check
    // and forced safe-off must complete before enabling work.
    if early_boot_baseline().is_err() {
        /*
         * A watchdog reset cannot run teardown code.  The C SYS_INIT
         * best-effort baseline has already attempted every electrical
         * safe-off action; do not bring up BLE or normal work unless the
         * reconstructed drivers also acknowledged their shutdown state.
         */
        error!("output shutdown baseline unverified; bring-up halted");
        loop {
            zephyr::time::sleep(Duration::millis_at_least(1_000));
        }
    }
    // The statically-created monitor may now take the same mutex as the pump.
    STIM_MONITOR_READY.store(true, Ordering::Release);

    if let Err(e) = ble::start() {
        error!("BLE start failed: {}", e);
        return;
    }
    info!("pairing passkey for serial '{}': {:06}", SERIAL, passkey());

    // Pump: tickless. Sleep exactly until the engine's next wake-up; with nothing
    // queued, sleep until a GATT write gives the semaphore. Active stimulation
    // additionally caps that sleep at the watchdog feed interval (src/wdt.rs).
    loop {
        if !wdt::feed() {
            // A monitored resource whose predicate fails is failed, not
            // rebooted. Stop every output now; its lifecycle remains failed
            // until an explicit, separately-authorized recovery path exists.
            #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
            owo_suit::revoke();
            stop_all();
        }
        ota::reboot_if_requested();
        let (wake, poll, watchdog) = {
            let mut s = STATE.lock().unwrap();
            let wake = loop {
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
            let watchdog = (!s.out.monitoring_required())
                .then(|| s.stim_watchdog.take())
                .flatten();
            (wake, s.out.poll_interval_us(), watchdog)
        };
        // The supervisor has already completed its ordered safe-off path.
        // Releasing this resource can now turn off the watchdog.
        drop(watchdog);
        // Sleep until the next pulse, but never longer than the supervision interval while
        // the output is armed (the dead-man must be able to fire without any BLE traffic).
        let until = match (wake, poll) {
            (Some(t), Some(p)) => Some(t.min(now_us() + p)),
            (Some(t), None) => Some(t),
            (None, Some(p)) => Some(now_us() + p),
            (None, None) => None,
        };
        // The watchdog counts while the CPU idles in EM1, so never sleep past its feed interval.
        let until = wdt::cap_sleep(until, now_us());
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
