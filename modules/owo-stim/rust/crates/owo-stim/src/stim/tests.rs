use super::*;
use crate::hw::{Bridge, Hw, HwError};

#[derive(Clone, Debug, PartialEq)]
enum Ev {
    Bridge(Bridge),
    Dac(u8),
    Select(Option<u8>),
    Supply(bool),
    /// A sense conversion started (bridge state at its start).
    Sense(Bridge),
    Battery,
}

/// Virtual-time model of the output stage: the clock advances only through `delay_us`. A sense
/// conversion samples the output when it starts and completes `conv_us` later; it reads the
/// commanded level (times `sense_gain`) while the bridge is closed, 0 while it is off.
struct Mock {
    ev: Vec<Ev>,
    clock: u64,
    /// (time, bridge) history.
    bridge_log: Vec<(u64, Bridge)>,
    conv_us: u64,
    /// Start time of the running conversion and what it sampled.
    conv: Option<(u64, u16)>,
    /// Conversions started at or after this clock value never complete (ADC stalled).
    adc_dead_at: Option<u64>,
    /// (start time, phase start, width) of every conversion started while the bridge was closed.
    in_phase_starts: Vec<u64>,
    phase_start: Option<u64>,
    sense_gain: f32,
    /// The sense input reads this regardless of the output (stuck), from this clock value on.
    stuck: Option<(u64, u16)>,
    /// The output delivers this instead of the commanded level, from this long after a closure.
    over: Option<(u64, u16)>,
    /// Added to the next delay while the bridge is closed (a preempting interrupt).
    stall_energised: u32,
    /// An interrupt this long at the first delay after every bridge closure.
    stall_each_closure: u32,
    /// An interrupt this long right after the next sense poll that finds nothing.
    stall_after_poll: u32,
    fail_select: bool,
    fail_bridge_on: Option<Bridge>,
    cancel_at: Option<u64>,
    cur_dac: u8,
    energised_us: u64,
    battery: u32,
    battery_fails: bool,
    battery_at: Option<u64>,
}

impl Mock {
    fn new() -> Mock {
        Mock {
            ev: vec![],
            clock: 0,
            bridge_log: vec![(0, Bridge::Off)],
            conv_us: 8,
            conv: None,
            adc_dead_at: None,
            in_phase_starts: vec![],
            phase_start: None,
            sense_gain: 1.0,
            stuck: None,
            over: None,
            stall_energised: 0,
            stall_after_poll: 0,
            stall_each_closure: 0,
            fail_select: false,
            fail_bridge_on: None,
            cancel_at: None,
            cur_dac: 0,
            energised_us: 0,
            battery: 7_600,
            battery_fails: false,
            battery_at: None,
        }
    }
    fn bridge_trace(&self) -> Vec<Bridge> {
        self.ev
            .iter()
            .filter_map(|e| if let Ev::Bridge(b) = e { Some(*b) } else { None })
            .collect()
    }
    fn bridge_now(&self) -> Bridge {
        self.bridge_log.last().unwrap().1
    }
    fn set_bridge(&mut self, b: Bridge) {
        self.phase_start = if b == Bridge::Off { None } else { Some(self.clock) };
        if b != Bridge::Off && self.stall_each_closure > 0 {
            self.stall_energised = self.stall_each_closure;
        }
        self.bridge_log.push((self.clock, b));
    }
    fn sample_now(&self) -> u16 {
        if let Some((from, v)) = self.stuck {
            if self.clock >= from {
                return v;
            }
        }
        let Some(closed) = self.phase_start else { return 0 };
        if let Some((after, v)) = self.over {
            if self.clock >= closed + after {
                return v;
            }
        }
        (self.cur_dac as f32 * self.sense_gain) as u16
    }
    fn count(&self, e: &Ev) -> usize {
        self.ev.iter().filter(|x| *x == e).count()
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
        Ok(())
    }
    fn fine_mark(&mut self) -> u32 {
        self.clock as u32
    }
    fn fine_since_us(&mut self, mark: u32) -> u32 {
        (self.clock as u32).wrapping_sub(mark)
    }
    fn sense_start(&mut self) -> Result<(), HwError> {
        if let Some((at, _)) = self.conv {
            if self.adc_dead_at.is_some_and(|d| at >= d) || self.clock < at + self.conv_us {
                return Err(HwError::Adc); // the converter is still busy
            }
        }
        self.ev.push(Ev::Sense(self.bridge_now()));
        if let Some(p) = self.phase_start {
            self.in_phase_starts.push(self.clock - p);
        }
        self.conv = Some((self.clock, self.sample_now()));
        Ok(())
    }
    fn sense_poll(&mut self) -> Option<u16> {
        let (at, v) = self.conv?;
        if self.adc_dead_at.is_some_and(|d| at >= d) || self.clock < at + self.conv_us {
            self.clock += core::mem::take(&mut self.stall_after_poll) as u64;
            return None;
        }
        self.conv = None;
        Some(v)
    }
    fn battery_start(&mut self) -> Result<(), HwError> {
        self.ev.push(Ev::Battery);
        self.battery_at = Some(self.clock);
        Ok(())
    }
    fn battery_poll(&mut self) -> Option<u32> {
        let at = self.battery_at?;
        if self.battery_fails || self.clock < at + self.conv_us {
            return None;
        }
        self.battery_at = None;
        Some(self.battery)
    }
    fn delay_us(&mut self, us: u32) {
        let live = self.bridge_now() != Bridge::Off;
        let step = us as u64
            + if live { core::mem::take(&mut self.stall_energised) as u64 } else { 0 };
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

fn no_closure(m: &Mock) -> bool {
    let t = m.bridge_trace();
    !t.contains(&Bridge::Positive) && !t.contains(&Bridge::Negative)
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
    assert_eq!(o, Outcome::Fired { measured: 20 });
    let t = m.bridge_trace();
    assert_eq!(t, vec![Bridge::Off, Bridge::Positive, Bridge::Off, Bridge::Negative, Bridge::Off]);
    for w in t.windows(2) {
        assert!(!(w[0] == Bridge::Positive && w[1] == Bridge::Negative));
        assert!(!(w[0] == Bridge::Negative && w[1] == Bridge::Positive));
    }
    // routing, the zero check and then the amplitude are set before the bridge closes, and
    // routing and amplitude are cleared afterwards
    let pos = m.ev.iter().position(|e| *e == Ev::Bridge(Bridge::Positive)).unwrap();
    let sel = m.ev.iter().position(|e| *e == Ev::Select(Some(3))).unwrap();
    let zero = m.ev.iter().position(|e| *e == Ev::Sense(Bridge::Off)).unwrap();
    let dac = m.ev.iter().position(|e| *e == Ev::Dac(20)).unwrap();
    assert!(sel < zero && zero < dac && dac < pos);
    assert_eq!(m.ev[m.ev.len() - 1], Ev::Select(None));
    assert_eq!(m.cur_dac, 0);
    // both phases measured
    assert!(m.count(&Ev::Sense(Bridge::Positive)) >= 1);
    assert!(m.count(&Ev::Sense(Bridge::Negative)) >= 1);
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
    assert!(matches!(s.fire(&mut m, T0, &c), Outcome::Fired { .. }));
    // 2 phases of max_phase (400): the last step of each phase lands on the width.
    assert_eq!(m.energised_us, 2 * 400);
}

#[test]
fn phase_ends_at_its_width_and_conversions_keep_clear_of_the_end() {
    let (mut s, mut m) = armed();
    let c = PulseCmd { channel: 1, level: 20, width_us: 300, gap_us: 0 };
    assert!(matches!(s.fire(&mut m, T0, &c), Outcome::Fired { .. }));
    assert_eq!(m.energised_us, 2 * 300);
    let g = cfg().sense_guard_us as u64;
    assert!(m.in_phase_starts.len() >= 4, "{:?}", m.in_phase_starts);
    for &at in &m.in_phase_starts {
        assert!(at >= cfg().settle_us as u64, "{at}");
        // after the first conversion of a phase, none starts with less than the guard left
        assert!(at + g <= 300 || at == cfg().settle_us as u64, "{at}");
    }
}

#[test]
fn phases_too_short_to_measure_are_not_delivered() {
    let (mut s, mut m) = armed();
    let c = PulseCmd { channel: 1, level: 20, width_us: cfg().settle_us, gap_us: 0 };
    assert_eq!(s.fire(&mut m, T0, &c), Outcome::Skipped(Skip::TooShort));
    assert!(no_closure(&m));
}

#[test]
fn dead_adc_never_closes_the_bridge_and_locks() {
    let (mut s, mut m) = armed();
    m.adc_dead_at = Some(0);
    let o = s.fire(&mut m, T0, &cmd(0, 20));
    assert_eq!(o, Outcome::Faulted(Fault::SenseTimeout));
    assert!(no_closure(&m));
    assert!(s.is_locked());
    assert!(m.ev.contains(&Ev::Supply(false)));
}

#[test]
fn adc_stalling_mid_pulse_ends_the_phase_at_its_width_and_locks() {
    let (mut s, mut m) = armed();
    // The zero check (and the battery read) pass; every conversion started from the positive
    // phase on never completes.
    m.adc_dead_at = Some(m.clock + 30);
    let o = s.fire(&mut m, T0, &cmd(0, 20));
    assert_eq!(o, Outcome::Faulted(Fault::SenseTimeout));
    assert_eq!(m.energised_us, 150);
    assert!(!m.bridge_trace().contains(&Bridge::Negative));
    assert_eq!(*m.bridge_trace().last().unwrap(), Bridge::Off);
    assert!(s.is_locked());
}

/// An interrupt longer than the timeout right after a poll found the conversion still running:
/// the result is there when the loop resumes, so it is not a stall.
#[test]
fn a_long_interrupt_during_a_wait_is_not_a_stall() {
    let (mut s, mut m) = armed();
    m.stall_after_poll = cfg().sense_timeout_us + 100;
    assert!(matches!(s.fire(&mut m, T0, &cmd(0, 20)), Outcome::Fired { .. }));
}

/// An interrupt carries the loop from before `settle_us` past the width (but within the overrun
/// slack): that phase ends unmeasured, the other one still measures the pulse.
#[test]
fn one_phase_preempted_past_its_width_is_measured_by_the_other() {
    let (mut s, mut m) = armed();
    m.stall_energised = 145; // first delay of the positive phase: 155 us >= 150
    assert_eq!(s.fire(&mut m, T0, &cmd(0, 20)), Outcome::Fired { measured: 20 });
    assert_eq!(m.count(&Ev::Sense(Bridge::Positive)), 0);
    assert!(m.count(&Ev::Sense(Bridge::Negative)) >= 1);
}

#[test]
fn a_pulse_with_no_measured_phase_is_a_channel_fault() {
    let (mut s, mut m) = armed();
    m.stall_each_closure = 145;
    assert_eq!(s.fire(&mut m, T0, &cmd(0, 20)), Outcome::Faulted(Fault::Unmeasured));
    assert!(s.channel_latched(0) && !s.is_locked());
    assert_eq!(*m.bridge_trace().last().unwrap(), Bridge::Off);
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
fn sense_stuck_high_never_closes_the_bridge_and_locks() {
    let (mut s, mut m) = armed();
    m.stuck = Some((0, 75));
    let o = s.fire(&mut m, T0, &cmd(2, 20));
    assert_eq!(o, Outcome::Faulted(Fault::SenseNotZero { measured: 75 }));
    assert!(no_closure(&m));
    assert!(s.is_locked());
}

#[test]
fn overdrive_trips_immediately_and_latches_channel() {
    let (mut s, mut m) = armed();
    m.over = Some((0, 60)); // the regulator delivers far above 20 + 8
    let o = s.fire(&mut m, T0, &cmd(2, 20));
    assert_eq!(o, Outcome::Faulted(Fault::Overdrive { measured: 60, commanded: 20 }));
    assert!(!m.bridge_trace().contains(&Bridge::Negative));
    assert_eq!(*m.bridge_trace().last().unwrap(), Bridge::Off);
    assert!(m.ev.contains(&Ev::Supply(false)));
    assert_eq!(m.cur_dac, 0);
    // the phase ended at the first in-phase conversion, not at its width
    assert!(m.energised_us < 150, "{}", m.energised_us);
    assert_eq!(s.fire(&mut m, T0 + 1_000_000, &cmd(2, 20)), Outcome::Skipped(Skip::ChannelLatched));
    s.reset();
    s.feed(T0 + 1_000_001, 60_000_000);
    m.over = None;
    assert!(matches!(s.fire(&mut m, T0 + 2_000_000, &cmd(2, 20)), Outcome::Fired { .. }));
}

#[test]
fn switch_verify_failure_never_closes_bridge() {
    let (mut s, mut m) = armed();
    m.fail_select = true;
    let o = s.fire(&mut m, T0, &cmd(4, 20));
    assert_eq!(o, Outcome::Faulted(Fault::Hw(HwError::SwitchVerify)));
    assert!(no_closure(&m));
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
    m.over = Some((0, 100));
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
fn open_circuit_stops_the_output() {
    let (mut s, mut m) = armed();
    m.sense_gain = 0.0; // no current flows: open pads, or a sense stuck at zero
    let o = s.fire(&mut m, T0, &cmd(0, 30));
    assert_eq!(o, Outcome::Faulted(Fault::UnderDelivery { measured: 0, commanded: 30 }));
    assert!(!m.bridge_trace().contains(&Bridge::Negative));
    assert_eq!(*m.bridge_trace().last().unwrap(), Bridge::Off);
    assert!(m.ev.contains(&Ev::Supply(false)));
    // every channel is stopped until the user starts again
    assert!(s.is_locked() && !s.is_armed());
    assert_eq!(s.fire(&mut m, T0 + 100_000, &cmd(1, 30)), Outcome::Skipped(Skip::Locked));
    s.reset();
    s.feed(T0 + 200_000, 60_000_000);
    m.sense_gain = 1.0;
    assert!(matches!(s.fire(&mut m, T0 + 300_000, &cmd(1, 30)), Outcome::Fired { .. }));
}

#[test]
fn sense_stuck_at_zero_stops_once_the_level_is_distinguishable() {
    let (mut s, mut m) = armed();
    m.stuck = Some((0, 0));
    let margin = cfg().under_margin as u8;
    // At or below the margin a zero reading is inside the tolerance band: indistinguishable.
    assert!(matches!(s.fire(&mut m, T0, &cmd(0, margin)), Outcome::Fired { measured: 0 }));
    let o = s.fire(&mut m, T0 + 100_000, &cmd(0, margin + 1));
    assert_eq!(o, Outcome::Faulted(Fault::UnderDelivery { measured: 0, commanded: margin + 1 }));
    assert!(s.is_locked());
}

#[test]
fn partial_contact_stops_but_a_reading_inside_the_band_does_not() {
    let (mut s, mut m) = armed();
    let margin = cfg().under_margin;
    // delivers exactly the margin below the command: inside the band
    m.over = Some((0, 30 - margin));
    assert!(matches!(s.fire(&mut m, T0, &cmd(0, 30)), Outcome::Fired { .. }));
    // one level less: out of compliance (high pad impedance), stop
    m.over = Some((0, 30 - margin - 1));
    let o = s.fire(&mut m, T0 + 100_000, &cmd(0, 30));
    assert_eq!(o, Outcome::Faulted(Fault::UnderDelivery { measured: 30 - margin - 1, commanded: 30 }));
    assert!(s.is_locked());
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
    m.over = Some((100, 70)); // fine at first, way over once the phase is under way
    let c = PulseCmd { channel: 1, level: 20, width_us: 300, gap_us: 50 };
    let o = s.fire(&mut m, T0, &c);
    assert_eq!(o, Outcome::Faulted(Fault::Overdrive { measured: 70, commanded: 20 }));
    assert!(!m.bridge_trace().contains(&Bridge::Negative));
    assert_eq!(*m.bridge_trace().last().unwrap(), Bridge::Off);
}

/// A conversion still running when the width ends sampled the output inside the phase: it is
/// waited for and judged after the bridge opened, not dropped.
#[test]
fn overdrive_completing_after_the_width_still_trips() {
    let (mut s, mut m) = armed();
    m.conv_us = 40; // the only conversion of a 50 us phase starts at 20 us and ends at 60 us
    m.over = Some((0, 60));
    let c = PulseCmd { channel: 1, level: 20, width_us: 50, gap_us: 50 };
    let o = s.fire(&mut m, T0, &c);
    assert_eq!(o, Outcome::Faulted(Fault::Overdrive { measured: 60, commanded: 20 }));
    assert!(!m.bridge_trace().contains(&Bridge::Negative));
    assert_eq!(m.energised_us, 50);
}

#[test]
fn revoked_resource_interrupts_phase_and_shuts_down() {
    let (mut s, mut m) = armed();
    m.cancel_at = Some(m.clock + 120);
    let o = s.fire(&mut m, T0, &cmd(1, 80));
    assert_eq!(o, Outcome::Faulted(Fault::Cancelled));
    assert_eq!(*m.bridge_trace().last().unwrap(), Bridge::Off);
    assert!(m.ev.contains(&Ev::Supply(false)));
}

#[test]
fn session_refused_below_the_start_threshold() {
    let (mut s, mut m) = armed();
    m.battery = cfg().battery_start_mv - 1;
    let o = s.fire(&mut m, T0, &cmd(0, 20));
    assert_eq!(o, Outcome::Faulted(Fault::BatteryLow { mv: cfg().battery_start_mv - 1 }));
    assert!(no_closure(&m) && !m.ev.contains(&Ev::Supply(true)));
    assert!(s.is_battery_low() && !s.is_armed());
    // frames do not re-arm it; only an explicit user start does
    s.feed(T0 + 10, 60_000_000);
    assert!(!s.is_armed());
    assert_eq!(s.fire(&mut m, T0 + 20, &cmd(0, 20)), Outcome::Skipped(Skip::BatteryLow));
    s.reset();
    s.feed(T0 + 30, 60_000_000);
    m.battery = cfg().battery_start_mv;
    assert!(matches!(s.fire(&mut m, T0 + 40, &cmd(0, 20)), Outcome::Fired { .. }));
}

#[test]
fn running_session_stops_below_the_stop_threshold_only() {
    let (mut s, mut m) = armed();
    let every = cfg().battery_interval_us;
    assert!(matches!(s.fire(&mut m, T0, &cmd(0, 20)), Outcome::Fired { .. }));
    assert_eq!(m.count(&Ev::Battery), 1);
    // between the two thresholds: a running session goes on (hysteresis)
    m.battery = cfg().battery_stop_mv;
    assert!(matches!(s.fire(&mut m, T0 + every / 2, &cmd(0, 20)), Outcome::Fired { .. }));
    assert_eq!(m.count(&Ev::Battery), 1, "read again before the interval");
    assert!(matches!(s.fire(&mut m, T0 + every, &cmd(0, 20)), Outcome::Fired { .. }));
    assert_eq!(m.count(&Ev::Battery), 2);
    m.battery = cfg().battery_stop_mv - 1;
    m.ev.clear();
    let o = s.fire(&mut m, T0 + 2 * every, &cmd(0, 20));
    assert_eq!(o, Outcome::Faulted(Fault::BatteryLow { mv: cfg().battery_stop_mv - 1 }));
    assert!(no_closure(&m));
    assert!(m.ev.contains(&Ev::Supply(false)));
    assert!(!s.is_armed() && s.is_battery_low());
}

#[test]
fn battery_read_failure_locks_the_output() {
    let (mut s, mut m) = armed();
    m.battery_fails = true; // the conversion never completes
    let o = s.fire(&mut m, T0, &cmd(0, 20));
    assert_eq!(o, Outcome::Faulted(Fault::SenseTimeout));
    assert!(no_closure(&m));
    assert!(s.is_locked());
}
