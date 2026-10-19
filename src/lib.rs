//! OWO suit open firmware, proof of concept: Zephyr + Rust.
//!
//! BLE peripheral exposing the stock GATT database; stimulation is dry-run only.
#![no_std]

mod ble;
mod hal;

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

fn now_us() -> u64 {
    // SAFETY: plain kernel query.
    let ticks = unsafe { zephyr::raw::k_uptime_ticks() } as u64;
    ticks * 1_000_000 / SYS_FREQUENCY as u64
}

pub(crate) fn passkey() -> u32 {
    proto::pairing_code(SERIAL.as_bytes())
}

// ---- application hooks called from the GATT layer -------------------------

pub(crate) fn on_frame(buf: &[u8]) -> Result<(), proto::Error> {
    let f = Frame::parse(buf)?;
    let mut s = STATE.lock().unwrap();
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

pub(crate) fn stop_all() {
    let mut s = STATE.lock().unwrap();
    s.engine.stop();
    s.out.all_off();
}

// ---- entry point -----------------------------------------------------------

#[no_mangle]
extern "C" fn rust_main() {
    // SAFETY: runs once at startup before any other Rust thread exists.
    unsafe {
        zephyr::set_logger().unwrap();
    }
    STATE.lock().unwrap().out.init();

    if let Err(e) = ble::start() {
        error!("BLE start failed: {}", e);
        return;
    }
    info!("pairing passkey for serial '{}': {:06}", SERIAL, passkey());

    // Pump: tickless. Sleep exactly until the engine's next wake-up; with nothing
    // queued, sleep until a GATT write gives the semaphore. No periodic timer.
    loop {
        let (wake, poll) = {
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
            (wake, s.out.poll_interval_us())
        };
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
