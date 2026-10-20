use super::*;
use crate::hw::{Bridge, Hw, HwError, Sample};

#[derive(Clone, Debug, PartialEq)]
enum Ev {
    Bridge(Bridge),
    Dac(u8),
    Select(Option<u8>),
    Supply(bool),
}

/// Virtual-time model of the output stage: the clock advances only through `delay_us`, and a
/// free-running sampler produces a conversion every `period` us while the supply is on. A
/// conversion reports the commanded level if the bridge was closed when it completed.
struct Mock {
    ev: Vec<Ev>,
    clock: u64,
    /// (time, bridge) history, to evaluate what a past conversion saw.
    bridge_log: Vec<(u64, Bridge)>,
    sampler_from: Option<u64>,
    period: u64,
    /// After this clock value the sampler stops producing conversions (ADC/interrupt stalled).
    sampler_dies_at: Option<u64>,
    /// Never produce a first conversion.
    no_samples: bool,
    first_waits: u32,
    first_wait_timeout: Option<u32>,
    sense_gain: f32,
    /// Conversions completing at or after this clock value read this instead.
    over_from: Option<(u64, u16)>,
    /// Added to the next delay while the bridge is closed (a preempting interrupt).
    stall_energised: u32,
    fail_select: bool,
    fail_bridge_on: Option<Bridge>,
    cancel_at: Option<u64>,
    cur_dac: u8,
    energised_us: u64,
}

impl Mock {
    fn new() -> Mock {
        Mock {
            ev: vec![],
            clock: 0,
            bridge_log: vec![(0, Bridge::Off)],
            sampler_from: None,
            period: 60,
            sampler_dies_at: None,
            no_samples: false,
            first_waits: 0,
            first_wait_timeout: None,
            sense_gain: 1.0,
            over_from: None,
            stall_energised: 0,
            fail_select: false,
            fail_bridge_on: None,
            cancel_at: None,
            cur_dac: 0,
            energised_us: 0,
        }
    }
    fn bridge_trace(&self) -> Vec<Bridge> {
        self.ev
            .iter()
            .filter_map(|e| if let Ev::Bridge(b) = e { Some(*b) } else { None })
            .collect()
    }
    fn bridge_at(&self, t: u64) -> Bridge {
        self.bridge_log.iter().rev().find(|(at, _)| *at <= t).map(|(_, b)| *b).unwrap()
    }
    fn set_bridge(&mut self, b: Bridge) {
        self.bridge_log.push((self.clock, b));
    }
    fn conversion_at(&self, t: u64) -> u16 {
        if let Some((from, v)) = self.over_from {
            if t >= from {
                return v;
            }
        }
        if self.bridge_at(t) == Bridge::Off {
            0
        } else {
            (self.cur_dac as f32 * self.sense_gain) as u16
        }
    }
}

impl Hw for Mock {
    fn cancelled(&self) -> bool {
        self.cancel_at.is_some_and(|at| self.clock >= at)
    }
    fn bridge(&mut self, s: Bridge) -> Result<(), HwError> {
        if self.fail_bridge_on == Some(s) {
            // A real driver must leave the bridge off when it reports an error.
            self.ev.push(Ev::Bridge(Bridge::Off));
            self.set_bridge(Bridge::Off);
            return Err(HwError::Bus);
        }
        self.ev.push(Ev::Bridge(s));
        self.set_bridge(s);
        Ok(())
    }
    fn dac_level(&mut self, l: u8) -> Result<(), HwError> {
        self.cur_dac = l;
        self.ev.push(Ev::Dac(l));
        Ok(())
    }
    fn select(&mut self, ch: Option<u8>) -> Result<(), HwError> {
        self.ev.push(Ev::Select(ch));
        if self.fail_select && ch.is_some() {
            return Err(HwError::SwitchVerify);
        }
        Ok(())
    }
    fn supply(&mut self, on: bool) -> Result<(), HwError> {
        self.ev.push(Ev::Supply(on));
        self.sampler_from = if on { Some(self.clock) } else { None };
        Ok(())
    }
    fn now_us(&mut self) -> u64 {
        self.clock
    }
    fn fine_mark(&mut self) -> u32 {
        self.clock as u32
    }
    fn fine_since_us(&mut self, mark: u32) -> u32 {
        (self.clock as u32).wrapping_sub(mark)
    }
    fn sense_latest(&mut self) -> Option<Sample> {
        let from = self.sampler_from?;
        if self.no_samples {
            return None;
        }
        let upto = self.sampler_dies_at.map_or(self.clock, |d| d.min(self.clock));
        if upto < from + self.period {
            return None;
        }
        let at = from + (upto - from) / self.period * self.period;
        Some(Sample { value: self.conversion_at(at), at_us: at })
    }
    fn wait_first_sample(&mut self, timeout_us: u32) -> bool {
        self.first_waits += 1;
        self.first_wait_timeout = Some(timeout_us);
        let Some(from) = self.sampler_from else { return false };
        if self.no_samples {
            self.clock += timeout_us as u64;
            return false;
        }
        let first = from + self.period;
        if self.sampler_dies_at.is_some_and(|dies| dies <= first) || first > self.clock + timeout_us as u64 {
            self.clock += timeout_us as u64;
            return false;
        }
        self.clock = self.clock.max(first);
        true
    }
    fn delay_us(&mut self, us: u32) {
        let live = self.bridge_at(self.clock) != Bridge::Off;
        let step = us as u64 + if live { core::mem::take(&mut self.stall_energised) as u64 } else { 0 };
        if live {
            self.energised_us += step;
        }
        self.clock += step;
    }
}

fn cfg() -> Config {
    Config { soft_start_us: 0, ..Config::conservative() }
}

fn cmd(ch: u8, level: u8) -> PulseCmd {
    PulseCmd { channel: ch, level, width_us: 150, gap_us: 50 }
}

const T0: u64 = 1_000_000;

fn armed() -> (Stimulator, Mock) {
    let mut s = Stimulator::new(cfg());
    s.feed(T0, 60_000_000);
    (s, Mock::new())
}

#[test]
fn disarmed_never_touches_hardware() {
    let mut s = Stimulator::new(cfg());
    let mut m = Mock::new();
    assert_eq!(s.fire(&mut m, T0, &cmd(0, 20)), Outcome::Skipped(Skip::Disarmed));
    assert!(m.ev.is_empty());
}

#[test]
fn biphasic_with_break_before_make() {
    let (mut s, mut m) = armed();
    let o = s.fire(&mut m, T0, &cmd(3, 20));
    assert!(matches!(o, Outcome::Fired { open_circuit: false, .. }), "{o:?}");
    let t = m.bridge_trace();
    assert_eq!(t, vec![Bridge::Off, Bridge::Positive, Bridge::Off, Bridge::Negative, Bridge::Off]);
    for w in t.windows(2) {
        assert!(!(w[0] == Bridge::Positive && w[1] == Bridge::Negative));
        assert!(!(w[0] == Bridge::Negative && w[1] == Bridge::Positive));
    }
    // routing and amplitude are set before the bridge closes, and cleared afterwards
    let pos = m.ev.iter().position(|e| *e == Ev::Bridge(Bridge::Positive)).unwrap();
    let sel = m.ev.iter().position(|e| *e == Ev::Select(Some(3))).unwrap();
    let dac = m.ev.iter().position(|e| *e == Ev::Dac(20)).unwrap();
    assert!(sel < pos && dac < pos);
    assert_eq!(m.ev[m.ev.len() - 1], Ev::Select(None));
    assert_eq!(m.cur_dac, 0);
}

#[test]
fn level_clamped_to_hard_ceiling() {
    let (mut s, mut m) = armed();
    s.fire(&mut m, T0, &cmd(0, 126));
    let max = m.ev.iter().filter_map(|e| if let Ev::Dac(l) = e { Some(*l) } else { None }).max();
    assert_eq!(max, Some(40));
}

#[test]
fn widths_and_gaps_clamped_and_energised_time_bounded() {
    let (mut s, mut m) = armed();
    let c = PulseCmd { channel: 1, level: 20, width_us: 5000, gap_us: 5000 };
    s.fire(&mut m, T0, &c);
    // 2 phases of max_phase (400), plus up to one poll step of quantisation each.
    assert!(m.energised_us >= 2 * 400, "{}", m.energised_us);
    assert!(m.energised_us <= 2 * (400 + 10), "{}", m.energised_us);
}

#[test]
fn bridge_never_closes_without_a_first_conversion() {
    let (mut s, mut m) = armed();
    m.no_samples = true;
    let o = s.fire(&mut m, T0, &cmd(0, 20));
    assert_eq!(o, Outcome::Faulted(Fault::SenseTimeout));
    assert_eq!(m.first_waits, 1);
    assert_eq!(m.first_wait_timeout, Some(3_000));
    let t = m.bridge_trace();
    assert!(!t.contains(&Bridge::Positive) && !t.contains(&Bridge::Negative));
    assert_eq!(m.clock, 3_000, "waited {} us", m.clock);
}

#[test]
fn stalled_sampler_ends_the_phase() {
    let mut s = Stimulator::new(Config { sense_max_age_us: 100, ..cfg() });
    s.feed(T0, 60_000_000);
    let mut m = Mock::new();
    m.sampler_dies_at = Some(100); // last conversion at ~60, then silence
    let o = s.fire(&mut m, T0, &cmd(0, 20));
    assert!(matches!(o, Outcome::Faulted(Fault::StaleSense { .. })), "{o:?}");
    assert_eq!(*m.bridge_trace().last().unwrap(), Bridge::Off);
    assert!(!m.bridge_trace().contains(&Bridge::Negative));
}

#[test]
fn preempted_phase_is_an_overrun_fault() {
    let (mut s, mut m) = armed();
    m.stall_energised = 400;
    let o = s.fire(&mut m, T0, &cmd(0, 20));
    assert!(matches!(o, Outcome::Faulted(Fault::PhaseOverrun { .. })), "{o:?}");
    assert!(s.channel_latched(0));
    assert_eq!(*m.bridge_trace().last().unwrap(), Bridge::Off);
}

#[test]
fn bridge_closes_only_after_a_fresh_conversion_exists() {
    let (mut s, mut m) = armed();
    s.fire(&mut m, T0, &cmd(0, 20));
    let log = &m.bridge_log;
    // first closure no earlier than the first conversion (60 us after the supply came on)
    let first = log.iter().find(|(_, b)| *b == Bridge::Positive).unwrap().0;
    assert!(first >= 60);
    assert_eq!(m.first_waits, 1);
    assert_eq!(m.first_wait_timeout, Some(3_000));
}

#[test]
fn overdrive_trips_immediately_and_latches_channel() {
    let (mut s, mut m) = armed();
    m.over_from = Some((0, 60)); // far above 20 + 8
    let o = s.fire(&mut m, T0, &cmd(2, 20));
    assert!(matches!(o, Outcome::Faulted(Fault::Overdrive { measured: 60, commanded: 20 })));
    assert!(!m.bridge_trace().contains(&Bridge::Negative));
    assert_eq!(*m.bridge_trace().last().unwrap(), Bridge::Off);
    assert!(m.ev.contains(&Ev::Supply(false)));
    assert_eq!(m.cur_dac, 0);
    assert_eq!(s.fire(&mut m, T0 + 1_000_000, &cmd(2, 20)), Outcome::Skipped(Skip::ChannelLatched));
    s.reset();
    s.feed(T0 + 1_000_001, 60_000_000);
    m.over_from = None;
    assert!(matches!(s.fire(&mut m, T0 + 2_000_000, &cmd(2, 20)), Outcome::Fired { .. }));
}

#[test]
fn switch_verify_failure_never_closes_bridge() {
    let (mut s, mut m) = armed();
    m.fail_select = true;
    let o = s.fire(&mut m, T0, &cmd(4, 20));
    assert_eq!(o, Outcome::Faulted(Fault::Hw(HwError::SwitchVerify)));
    let t = m.bridge_trace();
    assert!(!t.contains(&Bridge::Positive) && !t.contains(&Bridge::Negative));
}

#[test]
fn bridge_error_mid_pulse_ends_with_everything_off() {
    let (mut s, mut m) = armed();
    m.fail_bridge_on = Some(Bridge::Negative);
    let o = s.fire(&mut m, T0, &cmd(0, 20));
    assert!(matches!(o, Outcome::Faulted(Fault::Hw(HwError::Bus))));
    assert_eq!(*m.bridge_trace().last().unwrap(), Bridge::Off);
    assert!(m.ev.contains(&Ev::Supply(false)));
}

#[test]
fn lockout_after_repeated_faults_and_reset_clears() {
    let (mut s, mut m) = armed();
    m.over_from = Some((0, 100));
    for ch in 0..3u8 {
        let _ = s.fire(&mut m, T0 + ch as u64 * 1_000_000, &cmd(ch, 20));
    }
    assert!(s.is_locked());
    m.ev.clear();
    assert_eq!(s.fire(&mut m, T0 + 10_000_000, &cmd(5, 20)), Outcome::Skipped(Skip::Locked));
    assert!(m.ev.is_empty());
    s.feed(T0 + 10_000_001, 60_000_000); // feeding does not re-arm a locked output
    assert!(!s.is_armed());
    s.reset();
    s.feed(T0 + 10_000_002, 60_000_000);
    assert!(s.is_armed());
}

#[test]
fn deadman_disarms_after_coverage_ends_and_supply_goes_off() {
    let mut s = Stimulator::new(cfg());
    let mut m = Mock::new();
    s.feed(T0, 2_000_000); // covers T0..T0+2s
    s.fire(&mut m, T0, &cmd(0, 20));
    m.ev.clear();
    s.poll(&mut m, T0 + 3_000_000); // inside coverage + 1.5 s
    assert!(s.is_armed());
    assert!(m.ev.is_empty());
    s.poll(&mut m, T0 + 3_600_000); // past coverage + 1.5 s
    assert!(!s.is_armed());
    assert!(m.ev.contains(&Ev::Supply(false)));
    assert_eq!(s.fire(&mut m, T0 + 3_700_000, &cmd(0, 20)), Outcome::Skipped(Skip::Disarmed));
}

#[test]
fn no_pulses_outside_the_window_the_frames_cover() {
    let mut s = Stimulator::new(cfg());
    let mut m = Mock::new();
    s.feed(T0, 1_000_000); // covers T0..T0+1s, grace 250 ms
    assert!(matches!(s.fire(&mut m, T0 + 1_100_000, &cmd(0, 20)), Outcome::Fired { .. }));
    m.ev.clear();
    assert_eq!(s.fire(&mut m, T0 + 1_300_000, &cmd(0, 20)), Outcome::Skipped(Skip::Expired));
    assert!(m.ev.is_empty());
    // a new frame extends the window
    s.feed(T0 + 1_400_000, 1_000_000);
    assert!(matches!(s.fire(&mut m, T0 + 1_500_000, &cmd(0, 20)), Outcome::Fired { .. }));
}

#[test]
fn queued_frames_extend_coverage_back_to_back() {
    let mut s = Stimulator::new(cfg());
    s.feed(T0, 1_000_000);
    s.feed(T0 + 10, 1_000_000); // second frame plays after the first
    let mut m = Mock::new();
    assert!(matches!(s.fire(&mut m, T0 + 1_900_000, &cmd(0, 20)), Outcome::Fired { .. }));
}

#[test]
fn soft_start_ramps_from_zero() {
    let mut s = Stimulator::new(Config { soft_start_us: 500_000, ..Config::conservative() });
    let mut m = Mock::new();
    s.feed(T0, 60_000_000);
    assert_eq!(s.fire(&mut m, T0, &cmd(0, 40)), Outcome::Skipped(Skip::Zero));
    s.feed(T0 + 250_000, 0);
    s.fire(&mut m, T0 + 250_000, &cmd(0, 40));
    assert!(m.ev.contains(&Ev::Dac(20)), "{:?}", m.ev);
    s.feed(T0 + 600_000, 0);
    m.ev.clear();
    s.fire(&mut m, T0 + 600_000, &cmd(0, 40));
    assert!(m.ev.contains(&Ev::Dac(40)));
}

#[test]
fn duty_budget_limits_sustained_rate_per_channel() {
    let (mut s, mut m) = armed();
    let mut fired = 0u64;
    let mut skipped = 0u64;
    for i in 0..200u64 {
        let now = T0 + i * 500; // a request every 0.5 ms for 100 ms
        s.feed(now, 0);
        match s.fire(&mut m, now, &cmd(0, 20)) {
            Outcome::Fired { .. } => fired += 1,
            Outcome::Skipped(Skip::DutyBudget) => skipped += 1,
            o => panic!("{o:?}"),
        }
    }
    assert!(skipped > 100, "fired {fired} skipped {skipped}");
    let on_us = fired * 300;
    assert!(on_us <= 4_000 + 100_000 * 40 / 1000 + 300, "on {on_us} us");
}

#[test]
fn open_circuit_is_reported_not_faulted() {
    let (mut s, mut m) = armed();
    m.sense_gain = 0.0;
    let o = s.fire(&mut m, T0, &cmd(0, 30));
    assert_eq!(o, Outcome::Fired { measured: 0, open_circuit: true });
    assert!(!s.channel_latched(0));
}

#[test]
fn bad_channel_rejected() {
    let (mut s, mut m) = armed();
    assert_eq!(s.fire(&mut m, T0, &cmd(10, 20)), Outcome::Skipped(Skip::BadChannel));
}

#[test]
fn release_leaves_everything_off_but_armed() {
    let (mut s, mut m) = armed();
    s.fire(&mut m, T0, &cmd(0, 20));
    m.ev.clear();
    s.release(&mut m);
    assert!(s.is_armed());
    assert!(m.ev.contains(&Ev::Supply(false)));
}

#[test]
fn overdrive_appearing_mid_phase_trips_before_negative_phase() {
    let (mut s, mut m) = armed();
    m.over_from = Some((120, 70)); // fine at first, way over once the phase is under way
    let o = s.fire(&mut m, T0, &cmd(1, 20));
    assert!(matches!(o, Outcome::Faulted(Fault::Overdrive { measured: 70, .. })), "{o:?}");
    assert!(!m.bridge_trace().contains(&Bridge::Negative));
    assert_eq!(*m.bridge_trace().last().unwrap(), Bridge::Off);
}

#[test]
fn revoked_resource_interrupts_phase_and_shuts_down() {
    let (mut s, mut m) = armed();
    m.cancel_at = Some(120);
    let o = s.fire(&mut m, T0, &cmd(1, 80));
    assert_eq!(o, Outcome::Faulted(Fault::Cancelled));
    assert_eq!(*m.bridge_trace().last().unwrap(), Bridge::Off);
    assert!(m.ev.contains(&Ev::Supply(false)));
}
