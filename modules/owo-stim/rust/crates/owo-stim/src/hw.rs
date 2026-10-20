//! Hardware interface used by the supervisor.

/// Output bridge state. Implementations must be break-before-make: they must never
/// go straight from `Positive` to `Negative` (or back) without passing through `Off`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Bridge {
    Off,
    Positive,
    Negative,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HwError {
    /// The channel switch did not read back what was written.
    SwitchVerify,
    /// A bus transaction failed.
    Bus,
    /// The requested transition would short the bridge (positive <-> negative).
    BridgeShoot,
}

/// The newest completed conversion of the feedback ADC.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Sample {
    /// In the same units as the commanded level.
    pub value: u16,
    /// When the ADC interrupt noted the completion, on the [`Hw::now_us`] clock.
    pub at_us: u64,
}

pub trait Hw {
    /// True when the resource lease was revoked. Implementations must make
    /// this lock-free: pulse execution checks it at bounded checkpoints.
    fn cancelled(&self) -> bool { false }
    /// Set the bridge. Must return with all drive off if it returns an error.
    fn bridge(&mut self, s: Bridge) -> Result<(), HwError>;
    /// Set the amplitude control DAC. `level` is in the stock firmware's 0..=126 units.
    fn dac_level(&mut self, level: u8) -> Result<(), HwError>;
    /// Connect electrode channel `ch` (or none) and verify by read-back.
    fn select(&mut self, ch: Option<u8>) -> Result<(), HwError>;
    /// Enable/disable the high-voltage supply. Must be idempotent. Turning it on also starts the
    /// free-running feedback sampler, turning it off stops it, so nothing runs while idle.
    fn supply(&mut self, on: bool) -> Result<(), HwError>;
    /// Monotonic microsecond clock.
    fn now_us(&mut self) -> u64;
    /// Opaque reading of a fine-grained free-running counter (resolution of 1 us or better), taken
    /// to time short intervals (pulse phase widths). Only valid for [`Hw::fine_since_us`].
    fn fine_mark(&mut self) -> u32;
    /// Microseconds elapsed since `mark`, correct across one wrap-around of the counter.
    fn fine_since_us(&mut self, mark: u32) -> u32;
    /// Non-blocking: the newest conversion the sampler has completed since the supply came on,
    /// or `None` if there has been none yet. The sampler runs in interrupt context and stamps
    /// each result with the clock, so the caller can judge how old it is.
    fn sense_latest(&mut self) -> Option<Sample>;
    /// Wait for the first feedback conversion after the supply was enabled. Implementations must
    /// block the calling thread until either the sample is published or `timeout_us` expires.
    /// The bridge is still open while this runs.
    fn wait_first_sample(&mut self, timeout_us: u32) -> bool;
    /// Busy-wait. Interrupts stay enabled.
    fn delay_us(&mut self, us: u32);
}
