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
    /// The ADC refused a conversion, or one it accepted never completed: it cannot be used
    /// again (the conversion may still hold the converter).
    Adc,
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
    /// Enable/disable the high-voltage supply. Must be idempotent.
    fn supply(&mut self, on: bool) -> Result<(), HwError>;
    /// Opaque reading of a fine-grained free-running counter (resolution of 1 us or better), taken
    /// to time short intervals (pulse phase widths). Only valid for [`Hw::fine_since_us`].
    fn fine_mark(&mut self) -> u32;
    /// Microseconds elapsed since `mark`, correct across one wrap-around of the counter.
    fn fine_since_us(&mut self, mark: u32) -> u32;
    /// Start one conversion of the feedback (sense) input and return at once. The supervisor
    /// starts every conversion itself, at points of the pulse it chooses: nothing samples on its
    /// own, so no conversion's completion can interrupt the pulse anywhere else.
    /// Implementations refuse ([`HwError::Adc`]) while an earlier conversion has not completed.
    fn sense_start(&mut self) -> Result<(), HwError>;
    /// The conversion [`Hw::sense_start`] started: `None` while it runs, then once
    /// `Some(value)` in the commanded level's units.
    fn sense_poll(&mut self) -> Option<u16>;
    /// Start one conversion of the battery input; as [`Hw::sense_start`] (one conversion at a
    /// time, of either input).
    fn battery_start(&mut self) -> Result<(), HwError>;
    /// The conversion [`Hw::battery_start`] started: `None` while it runs, then once
    /// `Some(millivolts at the pack)`.
    fn battery_poll(&mut self) -> Option<u32>;
    /// Busy-wait. Interrupts stay enabled.
    fn delay_us(&mut self, us: u32);
}
