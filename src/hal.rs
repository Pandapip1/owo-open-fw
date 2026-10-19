//! The ONLY place where electrical output could ever happen.
//!
//! This proof of concept ships a dry-run backend that only logs. The suit's
//! electrode hardware (pins, drivers, boost stage, current limits) has not been
//! identified, so no real backend exists. Do not write one until the board has
//! been identified and measured on a dummy load; see README.md.

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

    pub fn init(&self) {
        warn!("DRY-RUN stimulation backend: no electrode output exists in this build");
    }

    /// One bi-phasic pulse on one channel.
    pub fn pulse(&mut self, p: &Pulse) {
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

    /// Must be safe to call at any time.
    pub fn all_off(&mut self) {
        self.pulses = [0; NUM_CHANNELS];
        info!("all channels off");
    }
}
