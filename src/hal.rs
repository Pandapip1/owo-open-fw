//! Output selection: a log-only dry-run, or (opt-in at build time) the real output stage
//! behind the `owo-stim` safety supervisor.
//!
//! The real stage needs `CONFIG_OWO_STIM_UNVALIDATED_HARDWARE=y`. It has never run on
//! hardware; see `docs/stock-stim-path.md` and the bench checklist in README.md.

use log::{info, warn};
use owo_core::engine::Pulse;
use owo_core::proto::NUM_CHANNELS;

pub struct DryRun {
    pulses: [u32; NUM_CHANNELS],
}

impl DryRun {
    pub const fn new() -> DryRun {
        DryRun { pulses: [0; NUM_CHANNELS] }
    }

    #[cfg_attr(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE, allow(dead_code))]
    fn pulse(&mut self, p: &Pulse) {
        let n = &mut self.pulses[p.channel as usize];
        *n += 1;
        // One line per pulse would flood the log; show the first of each burst.
        if *n == 1 || n.is_multiple_of(100) {
            info!(
                "ch{} pulse #{} level={} a={} gap={} b={} t={} us",
                p.channel, n, p.level, p.width.a_us, p.width.gap_us, p.width.b_us, p.at_us
            );
        }
    }

    fn all_off(&mut self) {
        self.pulses = [0; NUM_CHANNELS];
        info!("all channels off");
    }
}

#[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
mod real {
    use super::*;
    use owo_stim::stim::{Config, Fault, Outcome, PulseCmd, Skip, Stimulator};
    use owo_suit::{LeaseError, Suit};
    use zephyr::kconfig as k;

    /// `Stimulator` + the suit's output stage. It is created lazily (first `init`) because
    /// it needs the kernel to be up.
    pub struct Real {
        hw: Option<Suit>,
        sup: Stimulator,
        poll_every_us: u64,
        /// When the stop input is read next while the output is armed.
        stop_input_at: u64,
    }

    /// What the stock firmware's width table unit means in microseconds is unconfirmed
    /// (its sequencer ticks at 50 us and divides the table value by 25, which would make
    /// 2x). Default 100 % (shorter, weaker); set after measuring on a scope.
    const WIDTH_PERCENT: u32 = k::CONFIG_OWO_STIM_WIDTH_SCALE_PERCENT as u32;
    const STOP_INPUT_POLL_US: u64 = k::CONFIG_OWO_STIM_STOP_INPUT_POLL_MS as u64 * 1000;

    fn config() -> Config {
        let mut c = Config::conservative();
        c.hard_max_level = k::CONFIG_OWO_STIM_HARD_MAX_LEVEL as u8;
        c.deadman_us = k::CONFIG_OWO_STIM_DEADMAN_MS as u64 * 1000;
        c.max_phase_us = k::CONFIG_OWO_STIM_MAX_PHASE_US as u32;
        c.soft_start_us = k::CONFIG_OWO_STIM_SOFT_START_MS as u64 * 1000;
        c.max_duty_permille = k::CONFIG_OWO_STIM_MAX_DUTY_PERMILLE as u32;
        c.fault_lockout = k::CONFIG_OWO_STIM_FAULT_LOCKOUT as u8;
        c.sense_guard_us = k::CONFIG_OWO_STIM_SENSE_GUARD_US as u32;
        c.sense_timeout_us = k::CONFIG_OWO_STIM_SENSE_TIMEOUT_US as u32;
        c.settle_us = k::CONFIG_OWO_STIM_SETTLE_US as u32;
        c.under_margin = k::CONFIG_OWO_STIM_UNDER_MARGIN as u16;
        c.battery_start_mv = k::CONFIG_OWO_STIM_BATTERY_START_MV as u32;
        c.battery_stop_mv = k::CONFIG_OWO_STIM_BATTERY_STOP_MV as u32;
        c.battery_interval_us = k::CONFIG_OWO_STIM_BATTERY_INTERVAL_MS as u64 * 1000;
        c
    }

    impl Real {
        pub const fn new() -> Real {
            Real {
                hw: None,
                sup: Stimulator::new(Config::conservative()),
                // The pump itself is the sole scheduled safety owner. This
                // keeps output shutdown out of IRQ context and avoids a
                // second monitor thread competing for the output mutex.
                poll_every_us: k::CONFIG_OWO_STIM_MONITOR_INTERVAL_MS as u64 * 1_000,
                stop_input_at: 0,
            }
        }

        /// Establish and verify the driver's shutdown state before normal
        /// application bring-up.  Unlike `all_off`, startup must surface a
        /// rejected driver command instead of merely logging it.
        pub fn init(&mut self) -> Result<(), ()> {
            warn!("REAL stimulation backend enabled: UNVALIDATED on hardware; dummy load only");
            self.sup = Stimulator::new(config());
            let hw = Suit::post_and_shutdown().map_err(|e| {
                warn!("output-stage shutdown baseline failed ({:?})", e);
            })?;
            self.hw = Some(hw);
            owo_suit::set_stop_listener(crate::stop_requested);
            if owo_suit::stop_requested() {
                crate::stop_requested();
            }
            Ok(())
        }

        pub fn frame_received(&mut self, now_us: u64, covers_us: u64, first: bool) {
            if first {
                self.sup.restart_window(now_us);
            }
            self.sup.feed(now_us, covers_us);
        }

        /// Explicit user Start/Restart: clears fault latches and the lockout, unless the stop
        /// input (the power button) still asks for a stop.
        pub fn user_start(&mut self, now_us: u64) {
            if owo_suit::stop_input_demands_stop() {
                warn!("start refused: the power button is pressed (or cannot be read)");
                return;
            }
            self.sup.reset();
            self.sup.restart_window(now_us);
        }

        /// A stop was requested: stop now (ordered safe-off) and stay stopped until Start.
        pub fn lock_out(&mut self) {
            // Taken before the safe-off: a request arriving after it is seen on the next pass.
            owo_suit::take_stop_request();
            if let Some(hw) = self.hw.as_mut() {
                self.sup.lock_out(hw);
            }
            info!("output stopped on request; locked until Start");
        }

        pub fn pulse(&mut self, p: &Pulse, now_us: u64) {
            let Some(hw) = self.hw.as_mut() else { return };
            if owo_suit::stop_requested() {
                return;
            }
            // Hold a resource lease for the whole bounded pulse. A monitor
            // failure revokes this lease; `Stimulator::fire` observes that at
            // its checkpoints and executes the electrical safe-off path.
            let _lease = match owo_suit::acquire_lease() {
                Ok(lease) => lease,
                Err(LeaseError::Revoked) => {
                    warn!("ch{} skipped: output resource is failed", p.channel);
                    return;
                }
                Err(LeaseError::Busy) => {
                    warn!("ch{} skipped: output resource lease unavailable", p.channel);
                    return;
                }
            };
            let cmd = PulseCmd {
                channel: p.channel,
                level: p.level,
                width_us: p.width.a_us as u32 * WIDTH_PERCENT / 100,
                gap_us: p.width.gap_us as u32,
            };
            // While a phase runs the bridge is closed and its length is this thread's own
            // timing: another thread must not run inside it. The system work queue (a
            // cooperative priority above the pump) did, once per pulse train, and a phase then
            // overran its width by the work item and the interrupts around it (the supervisor's
            // overrun check ended the session). A pulse is bounded by the supervisor's phase and
            // gap limits, so holding the scheduler for it is bounded too; interrupts and the
            // meta-IRQ radio jobs still preempt it, as they must.
            // SAFETY: plain kernel calls, paired in this thread; `fire` blocks only on kernel
            // objects, which a scheduler-locked thread may do.
            unsafe { zephyr::raw::k_sched_lock() };
            let outcome = self.sup.fire(hw, now_us, &cmd);
            // SAFETY: as above.
            unsafe { zephyr::raw::k_sched_unlock() };
            #[cfg(CONFIG_OWO_SIM_INJECT)]
            crate::sim_inject::note_outcome(&outcome);
            match outcome {
                Outcome::Fired { .. } => {}
                Outcome::Skipped(Skip::DutyBudget) | Outcome::Skipped(Skip::Zero) => {}
                Outcome::Skipped(s) => log::debug!("ch{} skipped: {:?}", p.channel, s),
                Outcome::Faulted(Fault::UnderDelivery { measured, commanded }) => warn!(
                    "ch{}: sense {} far below command {} (open electrode, no contact or dead \
                     sense); output stopped until Start",
                    p.channel, measured, commanded
                ),
                Outcome::Faulted(Fault::Cancelled) => {
                    info!("ch{} pulse ended: stop requested", p.channel)
                }
                Outcome::Faulted(Fault::BatteryLow { mv }) => {
                    warn!("battery {} mV too low; output stopped until Start", mv)
                }
                Outcome::Faulted(f) => warn!("ch{} FAULT: {:?}; output off", p.channel, f),
            }
        }

        pub fn poll(&mut self, now_us: u64) {
            if let Some(hw) = self.hw.as_mut() {
                self.sup.poll(hw, now_us);
            }
            // The expander's /INT edge is the prompt path; this read bounds a lost one and stops
            // the output when the expander (which also holds the supply enables) cannot be read.
            if self.sup.is_armed() && now_us >= self.stop_input_at {
                self.stop_input_at = now_us + STOP_INPUT_POLL_US;
                if owo_suit::stop_input_demands_stop() {
                    owo_suit::request_stop();
                    crate::stop_requested();
                }
            }
        }

        pub fn idle(&mut self) {
            if let Some(hw) = self.hw.as_mut() {
                self.sup.release(hw);
            }
        }

        pub fn all_off(&mut self) {
            if let Some(hw) = self.hw.as_mut() {
                self.sup.disarm(hw);
            }
            info!("all channels off");
        }

        pub fn poll_interval_us(&self) -> Option<u64> {
            self.sup.is_armed().then_some(self.poll_every_us)
        }

        pub fn monitoring_required(&self) -> bool {
            self.sup.is_armed()
        }
    }
}

/// The output the pump drives.
pub struct Output {
    dry: DryRun,
    #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
    real: real::Real,
}

impl Output {
    pub const fn new() -> Output {
        Output {
            dry: DryRun::new(),
            #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
            real: real::Real::new(),
        }
    }

    /// Establish the output driver's verified shutdown baseline.
    pub fn init(&mut self) -> Result<(), ()> {
        #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
        return self.real.init();
        #[cfg(not(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE))]
        {
            warn!("DRY-RUN stimulation backend: no electrode output exists in this build");
            Ok(())
        }
    }

    /// A valid frame arrived asking for `covers_us` of output (feeds the coverage window).
    pub fn frame_received(&mut self, _now_us: u64, _covers_us: u64, _first: bool) {
        #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
        self.real.frame_received(_now_us, _covers_us, _first);
    }

    /// The user explicitly started a session.
    pub fn user_start(&mut self, _now_us: u64) {
        #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
        self.real.user_start(_now_us);
    }

    /// Stop now on an external request and stay stopped until the user starts again.
    pub fn lock_out(&mut self) {
        #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
        self.real.lock_out();
        self.dry.all_off();
    }

    pub fn pulse(&mut self, p: &Pulse, _now_us: u64) {
        #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
        self.real.pulse(p, _now_us);
        #[cfg(not(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE))]
        self.dry.pulse(p);
    }

    /// Periodic supervision (dead-man). Cheap when idle.
    pub fn poll(&mut self, _now_us: u64) {
        #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
        self.real.poll(_now_us);
    }

    /// The pulse engine has nothing queued: de-energise the supply.
    pub fn idle(&mut self) {
        #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
        self.real.idle();
    }

    /// Must be safe to call at any time.
    pub fn all_off(&mut self) {
        #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
        self.real.all_off();
        self.dry.all_off();
    }

    /// If the pump must wake periodically to supervise, how often (us).
    pub fn poll_interval_us(&self) -> Option<u64> {
        #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
        return self.real.poll_interval_us();
        #[cfg(not(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE))]
        None
    }

    /// Whether an output session is still armed and therefore requires the
    /// watchdog-backed dead-man supervisor.
    pub fn monitoring_required(&self) -> bool {
        #[cfg(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)]
        return self.real.monitoring_required();
        #[cfg(not(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE))]
        false
    }
}
