//! Safety supervisor. Every decision about whether, how hard and for how long to drive the
//! output stage is made here, on top of the narrow [`Hw`] interface, and nowhere else.
//!
//! Interlocks implemented here that the stock firmware 7.9.07 does not have are marked
//! **[new]**; those it has (re-implemented) are marked **[stock]**. See
//! `docs/stock-stim-path.md` for the evidence.
//!
//! 1. **[new]** Hard ceiling on level, independent of protocol-layer limits.
//! 2. **[new]** Coverage window + dead-man: pulses are only emitted inside the time the received
//!    frames cover (plus `grace_us`), and the output disarms `deadman_us` after the window ends.
//! 3. **[new]** Soft start after arming.
//! 4. **[new]** Per-channel duty budget (leaky bucket).
//! 5. **[new]** Phase and gap hard limits applied at the lowest layer.
//! 6. **[new]** Charge balance by construction: both phases use the same level and the same
//!    width (the stock sequencer also ignores the second width, but the protocol accepts one).
//! 7. **[new]** Supply is off unless armed; boot state and every fault leave it off.
//! 8. **[new]** Fault lockout after N faults until [`Stimulator::reset`].
//! 9. **[stock]** Closed-loop over-delivery trip using the sense ADC, per-channel latch.
//! 10. **[stock]** Channel switch read-back verification.
//! 11. **[stock]** Break-before-make on the bridge, bridge off before the channel changes.
//! 12. **[new]** Supervisor-timed sensing: the supervisor starts every conversion itself, so
//!     nothing samples while it is not looking and no conversion's completion interrupt can
//!     land at the end of a phase (conversions are only started while `sense_guard_us` of the
//!     phase remains, after the first). Before each pulse, with the bridge off and the DAC at
//!     zero, a conversion must complete within `sense_timeout_us` and read zero (within
//!     `over_margin`): a dead or stuck-high sense path never gets the bridge closed. Every phase
//!     starts a conversion `settle_us` in; one that does not complete within `sense_timeout_us`
//!     is an ADC stall. A phase that an interrupt carried past its width before its conversion
//!     could start ends unmeasured; a pulse needs at least one measured phase.
//! 13. **[new]** Overrun check: a phase that took longer than `width + overrun_slack_us` (the
//!     thread was preempted) is a fault, which latches the channel.
//! 14. **[new]** Under-delivery stops the output: an in-phase conversion more than
//!     `under_margin` below the commanded level (open electrode, lost contact, sense stuck at
//!     zero, a rail that sagged out of compliance) locks the whole output until the user starts
//!     again. The stock firmware notes the same condition ("status 4") and keeps driving.
//!     The lock covers every channel because one reading cannot tell an open pad from a dead
//!     sense path, and a dead sense path blinds the over-delivery trip on every channel.
//! 15. **[new]** Battery: a session only starts with the pack at or above `battery_start_mv`,
//!     and stops below `battery_stop_mv` (read at least every `battery_interval_us` while
//!     pulses fire); either needs an explicit user start to try again.

use crate::hw::{Bridge, Hw, HwError};

pub const CHANNELS: usize = 10;

#[derive(Clone, Copy, Debug)]
pub struct Config {
    pub hard_max_level: u8,
    /// Disarm (supply off) this long after the last received frame's coverage ended.
    pub deadman_us: u64,
    /// Pulses are refused when the coverage window ended more than this long ago.
    pub grace_us: u64,
    pub max_phase_us: u32,
    pub max_gap_us: u32,
    pub soft_start_us: u64,
    pub max_duty_permille: u32,
    pub fault_lockout: u8,
    /// Trip when measured exceeds commanded by more than this (stock: 8). Also the largest
    /// reading accepted from the zero check before a pulse.
    pub over_margin: u16,
    /// Under-delivery: stop when an in-phase reading is more than this below the commanded
    /// level (stock open-circuit tolerance: 14, its default and this unit's stored value).
    pub under_margin: u16,
    /// The first conversion of a phase is started this long after the bridge closed (the
    /// output is still rising before). Phases no longer than this are not delivered: they could
    /// not be measured.
    pub settle_us: u32,
    /// Time from starting a conversion to its result being seen by the pulse loop (driver start
    /// path, conversion, completion interrupt). Further conversions in a phase are only started
    /// while at least this much of it remains, so their completion interrupts stay clear of the
    /// phase's end.
    pub sense_guard_us: u32,
    /// A conversion not done this long after its start means the ADC is stalled (the output
    /// locks). A phase still ends at its width meanwhile; this only bounds how long a missing
    /// result is waited for, and must exceed the longest interrupt latency the ADC's completion
    /// interrupt can see, so that latency is never mistaken for a stall.
    pub sense_timeout_us: u32,
    /// A phase may take this much longer than its width before it counts as an overrun.
    pub overrun_slack_us: u32,
    /// How often the pulse loop looks at the clock and the ADC while a phase is running.
    pub poll_step_us: u32,
    /// Burst allowance of the duty bucket, in on-time microseconds.
    pub duty_burst_us: u32,
    /// A session (the first pulse after arming) needs the pack at or above this.
    pub battery_start_mv: u32,
    /// A running session stops when the pack reads below this.
    pub battery_stop_mv: u32,
    /// While pulses fire, the pack is read again once the last reading is this old.
    pub battery_interval_us: u64,
}

impl Config {
    pub const fn conservative() -> Config {
        Config {
            hard_max_level: 40,
            deadman_us: 1_500_000,
            grace_us: 250_000,
            max_phase_us: 400,
            max_gap_us: 200,
            soft_start_us: 500_000,
            max_duty_permille: 40,
            fault_lockout: 3,
            over_margin: 8,
            under_margin: 14,
            settle_us: 20,
            sense_guard_us: 60,
            sense_timeout_us: 500,
            overrun_slack_us: 150,
            poll_step_us: 10,
            duty_burst_us: 4_000,
            battery_start_mv: 7_200,
            battery_stop_mv: 6_840,
            battery_interval_us: 1_000_000,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PulseCmd {
    pub channel: u8,
    pub level: u8,
    pub width_us: u32,
    pub gap_us: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Fault {
    /// The resource lease was revoked; all output must be shut down.
    Cancelled,
    Hw(HwError),
    /// Measured output exceeded the commanded level by more than the margin.
    Overdrive { measured: u16, commanded: u8 },
    /// Measured output was below the commanded level by more than the margin: open electrode,
    /// no contact, sense stuck at zero, or the rail out of compliance. Locks the output.
    UnderDelivery { measured: u16, commanded: u8 },
    /// With the bridge off and the DAC at zero the sense input read above `over_margin`: the
    /// sense path is stuck high or leaking. Locks the output.
    SenseNotZero { measured: u16 },
    /// A conversion did not complete within `sense_timeout_us`. Locks the output.
    SenseTimeout,
    /// A phase ran longer than its width plus the slack.
    PhaseOverrun { elapsed_us: u64 },
    /// Neither phase of the pulse could start a conversion before its width ended (the loop was
    /// preempted past it): the pulse went unsupervised. Latches the channel.
    Unmeasured,
    /// The pack read below the start (session start) or stop threshold. Stops the output.
    BatteryLow { mv: u32 },
}

impl Fault {
    /// Faults of the shared sense path or ADC: no channel can be supervised after them.
    fn locks_output(&self) -> bool {
        matches!(
            self,
            Fault::UnderDelivery { .. }
                | Fault::SenseNotZero { .. }
                | Fault::SenseTimeout
                | Fault::Hw(HwError::Adc)
        )
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Skip {
    Disarmed,
    Locked,
    ChannelLatched,
    /// No received frame covers this moment (engine ran past what was asked for).
    Expired,
    BadChannel,
    Zero,
    DutyBudget,
    /// A phase this short could not be measured (`settle_us`).
    TooShort,
    /// Stopped for a low battery; needs an explicit user start.
    BatteryLow,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// Delivered; `measured` is the last in-phase reading of the second phase.
    Fired { measured: u16 },
    Skipped(Skip),
    Faulted(Fault),
}

pub struct Stimulator {
    cfg: Config,
    armed: bool,
    armed_at: u64,
    valid_until: u64,
    locked: bool,
    faults: u8,
    latched: [bool; CHANNELS],
    credit_us: [u32; CHANNELS],
    credit_at: [u64; CHANNELS],
    supply_on: bool,
    /// Stopped for a low battery (cleared by [`Stimulator::reset`]).
    battery_low: bool,
    /// When the pack was last read, if it has been in this session.
    battery_at: Option<u64>,
}

impl Stimulator {
    pub const fn new(cfg: Config) -> Stimulator {
        Stimulator {
            cfg,
            armed: false,
            armed_at: 0,
            valid_until: 0,
            locked: false,
            faults: 0,
            latched: [false; CHANNELS],
            credit_us: [0; CHANNELS],
            credit_at: [0; CHANNELS],
            supply_on: false,
            battery_low: false,
            battery_at: None,
        }
    }

    pub fn config(&self) -> &Config {
        &self.cfg
    }
    pub fn is_armed(&self) -> bool {
        self.armed
    }
    pub fn is_locked(&self) -> bool {
        self.locked
    }
    pub fn is_battery_low(&self) -> bool {
        self.battery_low
    }
    pub fn channel_latched(&self, ch: usize) -> bool {
        self.latched.get(ch).copied().unwrap_or(true)
    }

    /// Force everything off. Safe to call at any time, repeatedly.
    pub fn disarm<H: Hw>(&mut self, hw: &mut H) {
        self.armed = false;
        self.safe_off(hw);
    }

    /// Output stage off but still armed (use when the pulse engine goes idle).
    pub fn release<H: Hw>(&mut self, hw: &mut H) {
        self.safe_off(hw);
    }

    /// Clear fault latches, the lockout and a low-battery stop. Call only on an explicit user
    /// Stop/Start. The battery is read again before the next session's first pulse.
    pub fn reset(&mut self) {
        self.locked = false;
        self.faults = 0;
        self.latched = [false; CHANNELS];
        self.battery_low = false;
    }

    /// Note that a valid frame has just arrived that asks for `covers_us` of output after any
    /// frames already queued. Arms the output on the first one.
    pub fn feed(&mut self, now_us: u64, covers_us: u64) {
        self.valid_until = self.valid_until.max(now_us).saturating_add(covers_us);
        if !self.armed && !self.locked && !self.battery_low {
            self.armed = true;
            self.armed_at = now_us;
            // A new session: its first pulse needs a reading against the start threshold.
            self.battery_at = None;
        }
    }

    /// A frame with the "first" flag replaced everything queued: the coverage window restarts.
    pub fn restart_window(&mut self, now_us: u64) {
        self.valid_until = now_us;
    }

    /// Call regularly (at least every few hundred ms). Applies the dead-man timeout.
    pub fn poll<H: Hw>(&mut self, hw: &mut H, now_us: u64) {
        if self.armed && now_us > self.valid_until.saturating_add(self.cfg.deadman_us) {
            self.disarm(hw);
        }
    }

    fn safe_off<H: Hw>(&mut self, hw: &mut H) {
        // Order matters: bridge first, then the amplitude, then routing, then the supply.
        let _ = hw.bridge(Bridge::Off);
        let _ = hw.dac_level(0);
        let _ = hw.select(None);
        let _ = hw.supply(false);
        self.supply_on = false;
    }

    fn fault<H: Hw>(&mut self, hw: &mut H, ch: usize, f: Fault) -> Outcome {
        self.safe_off(hw);
        if let Fault::BatteryLow { .. } = f {
            // Not a fault of the stage: stop, and stay stopped until the user starts again.
            self.battery_low = true;
            self.armed = false;
            return Outcome::Faulted(f);
        }
        if let Some(l) = self.latched.get_mut(ch) {
            *l = true;
        }
        self.faults = self.faults.saturating_add(1);
        if self.faults >= self.cfg.fault_lockout || f.locks_output() {
            self.locked = true;
            self.armed = false;
        }
        Outcome::Faulted(f)
    }

    fn effective_level(&self, level: u8, now_us: u64) -> u8 {
        let mut l = level.min(self.cfg.hard_max_level) as u64;
        let since = now_us.saturating_sub(self.armed_at);
        if self.cfg.soft_start_us > 0 && since < self.cfg.soft_start_us {
            l = l * since / self.cfg.soft_start_us;
        }
        l as u8
    }

    /// Read the pack if this session has no reading yet (against the start threshold) or the
    /// last one is `battery_interval_us` old (against the stop threshold).
    fn battery_check<H: Hw>(&mut self, hw: &mut H, now_us: u64) -> Result<(), Fault> {
        let floor = match self.battery_at {
            None => self.cfg.battery_start_mv,
            Some(at) if now_us.saturating_sub(at) >= self.cfg.battery_interval_us => {
                self.cfg.battery_stop_mv
            }
            Some(_) => return Ok(()),
        };
        hw.battery_start().map_err(Fault::Hw)?;
        let mv = self.wait_for(hw, H::battery_poll)?;
        self.battery_at = Some(now_us);
        if mv < floor {
            return Err(Fault::BatteryLow { mv });
        }
        Ok(())
    }

    /// Execute one biphasic pulse. Blocks for roughly `2*width + gap` plus bus time.
    pub fn fire<H: Hw>(&mut self, hw: &mut H, now_us: u64, cmd: &PulseCmd) -> Outcome {
        if self.locked {
            return Outcome::Skipped(Skip::Locked);
        }
        if self.battery_low {
            return Outcome::Skipped(Skip::BatteryLow);
        }
        if !self.armed {
            return Outcome::Skipped(Skip::Disarmed);
        }
        if now_us > self.valid_until.saturating_add(self.cfg.grace_us) {
            return Outcome::Skipped(Skip::Expired);
        }
        let ch = cmd.channel as usize;
        if ch >= CHANNELS {
            return Outcome::Skipped(Skip::BadChannel);
        }
        if self.latched[ch] {
            return Outcome::Skipped(Skip::ChannelLatched);
        }
        let level = self.effective_level(cmd.level, now_us);
        if level == 0 {
            return Outcome::Skipped(Skip::Zero);
        }
        let width = cmd.width_us.min(self.cfg.max_phase_us);
        if width <= self.cfg.settle_us {
            return Outcome::Skipped(Skip::TooShort);
        }
        let gap = cmd.gap_us.min(self.cfg.max_gap_us);

        // Duty budget: refill by elapsed * permille, spend 2*width per pulse.
        let cost = 2 * width;
        let dt = now_us.saturating_sub(self.credit_at[ch]);
        let refill = (dt.saturating_mul(self.cfg.max_duty_permille as u64) / 1000)
            .min(self.cfg.duty_burst_us as u64) as u32;
        let credit = self.credit_us[ch].saturating_add(refill).min(self.cfg.duty_burst_us);
        self.credit_at[ch] = now_us;
        if credit < cost {
            self.credit_us[ch] = credit;
            return Outcome::Skipped(Skip::DutyBudget);
        }
        self.credit_us[ch] = credit - cost;

        if hw.cancelled() {
            return self.fault(hw, ch, Fault::Cancelled);
        }
        if let Err(f) = self.battery_check(hw, now_us) {
            return self.fault(hw, ch, f);
        }
        // Setup (slow, interrupts enabled): supply, routing with read-back, amplitude.
        if !self.supply_on {
            if let Err(e) = hw.supply(true) {
                return self.fault(hw, ch, Fault::Hw(e));
            }
            self.supply_on = true;
        }
        if let Err(e) = hw.bridge(Bridge::Off) {
            return self.fault(hw, ch, Fault::Hw(e));
        }
        if let Err(e) = hw.select(Some(cmd.channel)) {
            return self.fault(hw, ch, Fault::Hw(e));
        }
        // Zero check, bridge off and DAC still at zero: the sense path must answer, and with
        // nothing driven it must read nothing. The bridge never closes on a dead or stuck sense.
        match self.convert(hw) {
            Ok(v) if v > self.cfg.over_margin => {
                return self.fault(hw, ch, Fault::SenseNotZero { measured: v })
            }
            Ok(_) => {}
            Err(f) => return self.fault(hw, ch, f),
        }
        if let Err(e) = hw.dac_level(level) {
            return self.fault(hw, ch, Fault::Hw(e));
        }

        let r: Result<u16, Fault> = (|| {
            let a = self.phase(hw, Bridge::Positive, level, width)?;
            if gap > 0 {
                hw.delay_us(gap);
                if hw.cancelled() { return Err(Fault::Cancelled); }
            }
            let b = self.phase(hw, Bridge::Negative, level, width)?;
            b.or(a).ok_or(Fault::Unmeasured)
        })();

        match r {
            Ok(m) => {
                // Leave the stage de-energised between pulses.
                let _ = hw.dac_level(0);
                let _ = hw.select(None);
                Outcome::Fired { measured: m }
            }
            Err(f) => self.fault(hw, ch, f),
        }
    }

    /// Start a sense conversion and wait (busy) for it, at most `sense_timeout_us`.
    fn convert<H: Hw>(&self, hw: &mut H) -> Result<u16, Fault> {
        hw.sense_start().map_err(Fault::Hw)?;
        self.wait_for(hw, H::sense_poll)
    }

    /// Wait (busy) for the running conversion, at most `sense_timeout_us` from now. The clock is
    /// read before the result is polled, so time spent in unrelated interrupts between the two
    /// can never turn a completed conversion into a timeout.
    fn wait_for<H: Hw, T>(&self, hw: &mut H, poll: fn(&mut H) -> Option<T>) -> Result<T, Fault> {
        let m = hw.fine_mark();
        loop {
            let waited = hw.fine_since_us(m);
            if let Some(v) = poll(hw) {
                return Ok(v);
            }
            if waited >= self.cfg.sense_timeout_us {
                return Err(Fault::SenseTimeout);
            }
            hw.delay_us(self.cfg.poll_step_us.min(self.cfg.sense_timeout_us - waited));
        }
    }

    /// Judge one in-phase reading against the commanded level.
    fn judge(&self, v: u16, level: u8) -> Result<(), Fault> {
        if v > level as u16 + self.cfg.over_margin {
            return Err(Fault::Overdrive { measured: v, commanded: level });
        }
        if v + self.cfg.under_margin < level as u16 {
            return Err(Fault::UnderDelivery { measured: v, commanded: level });
        }
        Ok(())
    }

    /// One phase: close the bridge, measure the output while the width elapses, open the bridge.
    /// Returns the last in-phase reading, `None` if the width had elapsed before the first
    /// conversion could start (the loop was preempted across `settle_us`).
    ///
    /// Timing: the phase's clock starts before the bridge closes, so the measured length is never
    /// shorter than the time the bridge was closed. The first conversion starts `settle_us` in;
    /// later ones only while `sense_guard_us` of the width remains, so when the width allows one
    /// conversion plus the guard the end of the phase never waits for a conversion's interrupt:
    /// the bridge opens within one poll step of the width, plus whatever unrelated interrupts
    /// take (the overrun check bounds those).
    fn phase<H: Hw>(
        &self,
        hw: &mut H,
        pol: Bridge,
        level: u8,
        width: u32,
    ) -> Result<Option<u16>, Fault> {
        if hw.cancelled() { return Err(Fault::Cancelled); }
        let f0 = hw.fine_mark();
        if let Err(e) = hw.bridge(pol) {
            let _ = hw.bridge(Bridge::Off);
            return Err(Fault::Hw(e));
        }
        let mut pending = false;
        let mut measured = None;
        let mut fail = None;
        loop {
            if pending {
                if let Some(v) = hw.sense_poll() {
                    pending = false;
                    measured = Some(v);
                    if let Err(f) = self.judge(v, level) {
                        fail = Some(f);
                        break;
                    }
                }
            }
            if hw.cancelled() {
                fail = Some(Fault::Cancelled);
                break;
            }
            let elapsed = hw.fine_since_us(f0);
            if elapsed >= width {
                break;
            }
            let remaining = width - elapsed;
            if !pending
                && elapsed >= self.cfg.settle_us
                && (measured.is_none() || remaining >= self.cfg.sense_guard_us)
            {
                if let Err(e) = hw.sense_start() {
                    fail = Some(Fault::Hw(e));
                    break;
                }
                pending = true;
                continue;
            }
            hw.delay_us(self.cfg.poll_step_us.min(remaining));
        }
        let off = hw.bridge(Bridge::Off);
        let elapsed = hw.fine_since_us(f0) as u64;
        // A conversion started inside the phase sampled the output there: wait for it and judge
        // it, even after a fault (the next conversion must find the converter free).
        if pending {
            match self.wait_for(hw, H::sense_poll) {
                Ok(v) => {
                    measured = Some(v);
                    if fail.is_none() {
                        fail = self.judge(v, level).err();
                    }
                }
                Err(f) => fail = fail.or(Some(f)),
            }
        }
        if let Some(f) = fail {
            return Err(f);
        }
        off.map_err(Fault::Hw)?;
        if elapsed > width as u64 + self.cfg.overrun_slack_us as u64 {
            return Err(Fault::PhaseOverrun { elapsed_us: elapsed });
        }
        Ok(measured)
    }
}

#[cfg(test)]
mod tests;
