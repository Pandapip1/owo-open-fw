//! native_sim integration test of the output stage. See ../CMakeLists.txt.
#![no_std]
// Test code: every unsafe call is a plain read or write of the test peripherals in src/test_hw.c.
#![allow(clippy::undocumented_unsafe_blocks)]

use owo_stim::stim::{Config, Fault, Outcome, PulseCmd, Stimulator};
use owo_stim::hw::HwError;
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
    fn t_set_corrupt_readback(on: i32);
    fn t_set_adc_hang(on: i32);
    fn t_reset_observations();
    fn t_exit(code: i32) -> !;
}

static FAILS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

fn fail() {
    FAILS.fetch_add(1, core::sync::atomic::Ordering::SeqCst);
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
    let mut hw = match Suit::new() {
        Ok(h) => h,
        Err(e) => {
            printkln!("  FAIL Suit::new: {:?}", e);
            unsafe { t_exit(1) }
        }
    };
    // SAFETY: plain reads.
    unsafe {
        check!(t_bridge_state() == 0, "bridge off after init");
        check!(t_switch_word() == 0, "switch open after init");
        check!(t_pwm_percent() == 0 && t_supply_on() == 0, "supply and boost off after init");
    }

    printkln!("sampler smoke");
    {
        use owo_stim::hw::Hw;
        let r = hw.supply(true);
        zephyr::time::sleep(zephyr::time::Duration::millis_at_least(5));
        let l = hw.sense_latest();
        printkln!("  supply(true)={:?} conversions={} latest={:?}", r, unsafe { t_conversions() }, l);
        let _ = hw.supply(false);
    }

    printkln!("normal biphasic pulse on channel 3, level 20");
    unsafe { t_reset_observations() };
    let mut sup = armed();
    let r = sup.fire(&mut hw, now_us(), &pulse(3, 20));
    let tr = trace();
    match r {
        Outcome::Fired { measured, open_circuit } => {
            check!(!open_circuit, "no open circuit reported");
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
        check!(t_conversions() > 4, "ADC kept converting while the stage was live ({})", t_conversions());
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

    let fails = FAILS.load(core::sync::atomic::Ordering::SeqCst);
    printkln!("{}", if fails == 0 { "ALL PASSED" } else { "FAILED" });
    unsafe { t_exit(if fails == 0 { 0 } else { 1 }) }
}
