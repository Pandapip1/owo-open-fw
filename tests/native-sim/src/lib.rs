//! native_sim integration test of the output stage. See ../CMakeLists.txt.
#![no_std]
// Test code: every unsafe call is a plain read or write of the test peripherals in src/test_hw.c.
#![allow(clippy::undocumented_unsafe_blocks)]

use core::sync::atomic::{AtomicU32, Ordering};

use owo_stim::stim::{Config, Fault, Outcome, PulseCmd, Skip, Stimulator};
use owo_stim::hw::{Hw, HwError};
use owo_suit::Suit;
use zephyr::printkln;
use zephyr::time::SYS_FREQUENCY;
use zephyr_panic as _;

extern "C" {
    fn t_plant_start();
    fn t_bridge_state() -> u32;
    fn t_supply_on() -> u32;
    fn t_switch_word() -> u32;
    fn t_dac_code() -> u32;
    fn t_pwm_percent() -> u32;
    fn t_shoot() -> u32;
    fn t_live_open() -> u32;
    fn t_conversions() -> u32;
    fn t_max_closed_us() -> u32;
    fn t_trace_len() -> u32;
    fn t_trace(i: u32) -> u32;
    fn t_set_gain_percent(g: u32);
    fn t_set_battery_mv(mv: u32);
    fn t_set_stop_pressed(pressed: i32);
    fn t_stop_in_us(us: u32);
    fn t_set_corrupt_readback(on: i32);
    fn t_set_adc_hang(on: i32);
    fn t_reset_observations();
    fn t_exit(code: i32) -> !;
}

static FAILS: AtomicU32 = AtomicU32::new(0);
static STOP_NOTIFIED: AtomicU32 = AtomicU32::new(0);

fn fail() {
    FAILS.fetch_add(1, Ordering::SeqCst);
}

fn stop_listener() {
    STOP_NOTIFIED.fetch_add(1, Ordering::SeqCst);
}

static REVOKE_ON_STOP: AtomicU32 = AtomicU32::new(0);

/// A stop requested from an interrupt (src/test_hw.c timer); a revocation once armed for it.
#[no_mangle]
extern "C" fn t_stop_from_isr() {
    if REVOKE_ON_STOP.load(Ordering::SeqCst) != 0 {
        owo_suit::revoke();
    } else {
        owo_suit::request_stop();
    }
}

macro_rules! check {
    ($cond:expr, $($msg:tt)*) => {
        if $cond {
            printkln!("  ok   {}", core::format_args!($($msg)*));
        } else {
            printkln!("  FAIL {}", core::format_args!($($msg)*));
            fail();
        }
    };
}

fn now_us() -> u64 {
    // SAFETY: plain kernel query.
    let ticks = unsafe { zephyr::raw::k_uptime_ticks() } as u64;
    ticks * 1_000_000 / SYS_FREQUENCY as u64
}

fn config() -> Config {
    let mut c = Config::conservative();
    c.soft_start_us = 0;
    c.hard_max_level = 40;
    c
}

fn armed() -> Stimulator {
    // A fresh supervisor starts with an empty duty bucket; let it fill.
    zephyr::time::sleep(zephyr::time::Duration::millis_at_least(200));
    let mut s = Stimulator::new(config());
    s.feed(now_us(), 10_000_000);
    s
}

fn pulse(ch: u8, level: u8) -> PulseCmd {
    PulseCmd { channel: ch, level, width_us: 200, gap_us: 50 }
}

fn trace() -> ([u32; 32], usize) {
    let mut t = [0u32; 32];
    // SAFETY: plain reads of the test peripherals.
    let n = unsafe { t_trace_len() } as usize;
    for (i, v) in t.iter_mut().enumerate().take(n.min(32)) {
        *v = unsafe { t_trace(i as u32) };
    }
    (t, n.min(32))
}

fn saw_closed(t: &([u32; 32], usize)) -> bool {
    t.0[..t.1].iter().any(|&s| s == 1 || s == 2)
}

fn all_off() -> bool {
    // SAFETY: plain reads of the test peripherals.
    unsafe {
        t_bridge_state() == 0 && t_switch_word() == 0 && t_dac_code() == 0 && t_supply_on() == 0
            && t_pwm_percent() == 0
    }
}

#[no_mangle]
extern "C" fn rust_main() {
    // SAFETY: test peripherals, single test thread.
    unsafe { t_plant_start() };
    printkln!("owo-stim native_sim test");

    printkln!("init");
    let mut hw = match Suit::post_and_shutdown() {
        Ok(h) => h,
        Err(e) => {
            printkln!("  FAIL Suit::post_and_shutdown: {:?}", e);
            unsafe { t_exit(1) }
        }
    };
    owo_suit::set_stop_listener(stop_listener);
    check!(!owo_suit::stop_requested(), "no stop requested with the button released");
    // SAFETY: plain reads.
    unsafe {
        check!(t_bridge_state() == 0, "bridge off after init");
        check!(t_switch_word() == 0, "switch open after init");
        check!(t_pwm_percent() == 0 && t_supply_on() == 0, "supply and boost off after init");
    }

    printkln!("one conversion, bridge off");
    {
        let r = hw.sense_start();
        let mut v = None;
        for _ in 0..100 {
            v = hw.sense_poll();
            if v.is_some() {
                break;
            }
            hw.delay_us(10);
        }
        check!(r.is_ok() && v == Some(0), "started {:?}, read {:?}", r, v);
    }

    printkln!("normal biphasic pulse on channel 3, level 20");
    unsafe { t_reset_observations() };
    let mut sup = armed();
    let r = sup.fire(&mut hw, now_us(), &pulse(3, 20));
    let tr = trace();
    match r {
        Outcome::Fired { measured } => {
            check!((18..=22).contains(&measured), "measured level {} is near 20", measured);
        }
        other => check!(false, "fired, got {:?}", other),
    }
    let pos = tr.0[..tr.1].iter().position(|&s| s == 1);
    let neg = tr.0[..tr.1].iter().position(|&s| s == 2);
    check!(matches!((pos, neg), (Some(p), Some(n)) if p < n), "positive phase before negative: {:?}", &tr.0[..tr.1]);
    unsafe {
        check!(t_shoot() == 0, "no shoot-through seen by the plant");
        check!(t_live_open() == 0, "bridge never closed with the supply off");
        check!(t_max_closed_us() < 600, "longest closed run {} us", t_max_closed_us());
        check!(t_bridge_state() == 0, "bridge off after the pulse");
        check!(t_switch_word() == 0, "switch cleared after the pulse");
        check!(t_dac_code() == 0, "DAC back at zero after the pulse");
        check!(t_conversions() > 4, "zero check and in-phase conversions ({})", t_conversions());
    }

    printkln!("dead-man");
    sup.poll(&mut hw, now_us() + 20_000_000);
    check!(!sup.is_armed(), "disarmed after the dead-man time");
    check!(all_off(), "everything off after disarm");

    printkln!("over-delivery trips");
    unsafe { t_reset_observations() };
    let mut sup = armed();
    unsafe { t_set_gain_percent(300) };
    let r = sup.fire(&mut hw, now_us(), &pulse(4, 20));
    unsafe { t_set_gain_percent(100) };
    check!(matches!(r, Outcome::Faulted(Fault::Overdrive { .. })), "overdrive fault, got {:?}", r);
    check!(all_off(), "everything off after the fault");
    check!(sup.channel_latched(4), "channel 4 latched");
    unsafe { check!(t_shoot() == 0, "no shoot-through") };

    printkln!("hung ADC: bridge never closes");
    unsafe { t_reset_observations() };
    let mut sup = armed();
    unsafe { t_set_adc_hang(1) };
    let r = sup.fire(&mut hw, now_us(), &pulse(5, 20));
    let tr = trace();
    check!(matches!(r, Outcome::Faulted(Fault::SenseTimeout)), "sense timeout, got {:?}", r);
    check!(!saw_closed(&tr), "bridge never closed: {:?}", &tr.0[..tr.1]);
    unsafe { t_set_adc_hang(0) };
    check!(all_off(), "everything off after the fault");

    printkln!("switch read-back mismatch: bridge never closes");
    unsafe { t_reset_observations() };
    let mut sup = armed();
    unsafe { t_set_corrupt_readback(1) };
    let r = sup.fire(&mut hw, now_us(), &pulse(6, 20));
    unsafe { t_set_corrupt_readback(0) };
    let tr = trace();
    check!(
        matches!(r, Outcome::Faulted(Fault::Hw(HwError::SwitchVerify))),
        "switch verify fault, got {:?}",
        r
    );
    check!(!saw_closed(&tr), "bridge never closed: {:?}", &tr.0[..tr.1]);
    check!(all_off(), "everything off, switch cleared");

    printkln!("battery below the start threshold: no session");
    unsafe { t_reset_observations() };
    let mut sup = armed();
    unsafe { t_set_battery_mv(config().battery_start_mv - 1) };
    let r = sup.fire(&mut hw, now_us(), &pulse(7, 20));
    unsafe { t_set_battery_mv(7600) };
    let tr = trace();
    check!(matches!(r, Outcome::Faulted(Fault::BatteryLow { .. })), "battery low, got {:?}", r);
    check!(!saw_closed(&tr), "bridge never closed: {:?}", &tr.0[..tr.1]);
    check!(all_off(), "everything off");

    printkln!("power button pressed: stop requested, no pulse, locked until Start");
    unsafe { t_reset_observations() };
    let mut sup = armed();
    unsafe { t_set_stop_pressed(1) };
    check!(owo_suit::stop_requested(), "stop requested by the input's callback");
    check!(STOP_NOTIFIED.load(Ordering::SeqCst) == 1, "listener told once");
    check!(owo_suit::stop_input_demands_stop(), "input reads pressed");
    check!(owo_suit::take_stop_request(), "request taken");
    sup.lock_out(&mut hw);
    check!(sup.is_locked() && !sup.is_armed() && all_off(), "locked out, everything off");
    sup.feed(now_us(), 10_000_000);
    check!(
        sup.fire(&mut hw, now_us(), &pulse(8, 20)) == Outcome::Skipped(Skip::Locked),
        "a new frame does not restart it"
    );
    unsafe { t_set_stop_pressed(0) };
    check!(!owo_suit::stop_requested(), "release requests nothing");
    check!(!owo_suit::stop_input_demands_stop(), "input reads released");

    printkln!("stop requested from an interrupt inside a phase: the pulse completes balanced");
    unsafe { t_reset_observations() };
    let mut sup = armed();
    let long = PulseCmd { channel: 9, level: 20, width_us: 400, gap_us: 50 };
    unsafe { t_stop_in_us(150) };
    let r = sup.fire(&mut hw, now_us(), &long);
    let tr = trace();
    check!(matches!(r, Outcome::Fired { .. }), "fired, got {:?}", r);
    check!(tr.0[..tr.1].contains(&1) && tr.0[..tr.1].contains(&2), "both phases: {:?}", &tr.0[..tr.1]);
    check!(owo_suit::take_stop_request(), "request pending after the pulse, taken");
    sup.lock_out(&mut hw);
    check!(sup.is_locked() && all_off(), "locked out, everything off");

    printkln!("revoked resource inside a phase: the pulse ends at once");
    unsafe { t_reset_observations() };
    let mut sup = armed();
    unsafe { t_stop_in_us(150) };
    REVOKE_ON_STOP.store(1, Ordering::SeqCst);
    let r = sup.fire(&mut hw, now_us(), &long);
    let tr = trace();
    check!(matches!(r, Outcome::Faulted(Fault::Cancelled)), "cancelled, got {:?}", r);
    check!(saw_closed(&tr) && !tr.0[..tr.1].contains(&2), "ended in the positive phase: {:?}", &tr.0[..tr.1]);
    check!(all_off() && sup.is_locked(), "everything off, output locked");

    let fails = FAILS.load(Ordering::SeqCst);
    printkln!("{}", if fails == 0 { "ALL PASSED" } else { "FAILED" });
    unsafe { t_exit(if fails == 0 { 0 } else { 1 }) }
}
