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
//! 12. **[new]** Sense freshness: the feedback ADC free-runs in interrupt context and stamps every
//!     conversion. The bridge is only closed, and only stays closed, while the newest conversion
//!     is at most `sense_max_age_us` old; a stalled ADC or an interrupt that starves the pulse
//!     thread therefore ends the pulse. No interrupt lock is needed, so the ADC interrupt can run.
//! 13. **[new]** Overrun check: a phase that took longer than `width + overrun_slack_us` (the
//!     thread was preempted) is a fault, which latches the channel.

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
    /// Trip when measured exceeds commanded by more than this (stock: 8).
    pub over_margin: u16,
    /// Report open circuit when measured is below commanded by more than this.
    pub under_margin: u16,
    /// Samples that completed less than this long after the bridge closed are ignored for the
    /// over-delivery check (the output is still rising).
    pub settle_us: u32,
    /// Longest the newest conversion may be while the bridge is (about to be) closed.
    pub sense_max_age_us: u32,
    /// How long to wait for the first conversion after the supply comes on.
    pub sense_startup_us: u32,
    /// A phase may take this much longer than its width before it counts as an overrun.
    pub overrun_slack_us: u32,
    /// How often the pulse loop looks at the sampler while a phase is running.
    pub poll_step_us: u32,
    /// Burst allowance of the duty bucket, in on-time microseconds.
    pub duty_burst_us: u32,
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
            under_margin: 15,
            settle_us: 20,
            sense_max_age_us: 500,
            sense_startup_us: 3_000,
            overrun_slack_us: 150,
            poll_step_us: 10,
            duty_burst_us: 4_000,
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
    Hw(HwError),
    /// Measured output exceeded the commanded level by more than the margin.
    Overdrive { measured: u16, commanded: u8 },
    /// The newest conversion was older than `sense_max_age_us` while the output was live.
    StaleSense { age_us: u64 },
    /// No conversion arrived after the supply came on.
    SenseTimeout,
    /// A phase ran longer than its width plus the slack.
    PhaseOverrun { elapsed_us: u64 },
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
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    Fired { measured: u16, open_circuit: bool },
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

    /// Clear fault latches and the lockout. Call only on an explicit user Stop/Start.
    pub fn reset(&mut self) {
        self.locked = false;
        self.faults = 0;
        self.latched = [false; CHANNELS];
    }

    /// Note that a valid frame has just arrived that asks for `covers_us` of output after any
    /// frames already queued. Arms the output on the first one.
    pub fn feed(&mut self, now_us: u64, covers_us: u64) {
        self.valid_until = self.valid_until.max(now_us).saturating_add(covers_us);
        if !self.armed && !self.locked {
            self.armed = true;
            self.armed_at = now_us;
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
        if let Some(l) = self.latched.get_mut(ch) {
            *l = true;
        }
        self.faults = self.faults.saturating_add(1);
        if self.faults >= self.cfg.fault_lockout {
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

    /// Execute one biphasic pulse. Blocks for roughly `2*width + gap` plus bus time.
    pub fn fire<H: Hw>(&mut self, hw: &mut H, now_us: u64, cmd: &PulseCmd) -> Outcome {
        if self.locked {
            return Outcome::Skipped(Skip::Locked);
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
        let width = cmd.width_us.clamp(1, self.cfg.max_phase_us);
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
        if let Err(e) = hw.dac_level(level) {
            return self.fault(hw, ch, Fault::Hw(e));
        }

        // The sampler starts with the supply; wait (bounded) for its first conversion so the
        // bridge never closes without live feedback.
        if let Err(f) = self.await_first_sample(hw) {
            return self.fault(hw, ch, f);
        }

        let r: Result<Option<u16>, Fault> = (|| {
            let a = self.phase(hw, Bridge::Positive, level, width)?;
            if gap > 0 {
                hw.delay_us(gap);
            }
            let b = self.phase(hw, Bridge::Negative, level, width)?;
            Ok(b.or(a))
        })();

        match r {
            Ok(m) => {
                // Leave the stage de-energised between pulses.
                let _ = hw.dac_level(0);
                let _ = hw.select(None);
                let open_circuit =
                    level >= 10 && m.is_some_and(|m| m + self.cfg.under_margin < level as u16);
                Outcome::Fired { measured: m.unwrap_or(0), open_circuit }
            }
            Err(f) => self.fault(hw, ch, f),
        }
    }

    fn await_first_sample<H: Hw>(&self, hw: &mut H) -> Result<(), Fault> {
        let start = hw.now_us();
        loop {
            if hw.sense_latest().is_some() {
                return Ok(());
            }
            if hw.now_us().saturating_sub(start) > self.cfg.sense_startup_us as u64 {
                return Err(Fault::SenseTimeout);
            }
            hw.delay_us(self.cfg.poll_step_us);
        }
    }

    /// The newest conversion, or the fault that its absence/staleness is.
    fn fresh<H: Hw>(&self, hw: &mut H) -> Result<(crate::hw::Sample, u64), Fault> {
        let s = hw.sense_latest().ok_or(Fault::SenseTimeout)?;
        let age = hw.now_us().saturating_sub(s.at_us);
        if age > self.cfg.sense_max_age_us as u64 {
            return Err(Fault::StaleSense { age_us: age });
        }
        Ok((s, age))
    }

    /// One phase: close the bridge (only with fresh feedback), watch the output until the width
    /// has elapsed, open the bridge. Returns the newest in-phase reading, if any arrived.
    fn phase<H: Hw>(
        &self,
        hw: &mut H,
        pol: Bridge,
        level: u8,
        width: u32,
    ) -> Result<Option<u16>, Fault> {
        let limit = level as u16 + self.cfg.over_margin;
        self.fresh(hw)?;
        if let Err(e) = hw.bridge(pol) {
            let _ = hw.bridge(Bridge::Off);
            return Err(Fault::Hw(e));
        }
        // Widths come from the fine clock; sample ages from the (possibly coarser) timestamp clock.
        // Each is only ever used as a difference of its own readings.
        let f0 = hw.fine_mark();
        let settle = self.cfg.settle_us.min(width) as u64;
        let mut measured = None;
        loop {
            hw.delay_us(self.cfg.poll_step_us);
            let elapsed = hw.fine_since_us(f0);
            if elapsed >= width {
                break;
            }
            match self.fresh(hw) {
                Ok((s, age)) => {
                    // The conversion happened `age` ago: it is in the settled part of the phase
                    // if the phase had been running for at least `settle` by then.
                    if age + settle <= elapsed as u64 {
                        measured = Some(s.value);
                        if s.value > limit {
                            let _ = hw.bridge(Bridge::Off);
                            return Err(Fault::Overdrive { measured: s.value, commanded: level });
                        }
                    }
                }
                Err(f) => {
                    let _ = hw.bridge(Bridge::Off);
                    return Err(f);
                }
            }
        }
        if let Err(e) = hw.bridge(Bridge::Off) {
            return Err(Fault::Hw(e));
        }
        let elapsed = hw.fine_since_us(f0) as u64;
        if elapsed > width as u64 + self.cfg.overrun_slack_us as u64 {
            return Err(Fault::PhaseOverrun { elapsed_us: elapsed });
        }
        Ok(measured)
    }
}

#[cfg(test)]
mod tests;
