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

pub trait Hw {
    /// Set the bridge. Must return with all drive off if it returns an error.
    fn bridge(&mut self, s: Bridge) -> Result<(), HwError>;
    /// Set the amplitude control DAC. `level` is in the stock firmware's 0..=126 units.
    fn dac_level(&mut self, level: u8) -> Result<(), HwError>;
    /// Connect electrode channel `ch` (or none) and verify by read-back.
    fn select(&mut self, ch: Option<u8>) -> Result<(), HwError>;
    /// Measure the delivered output, in the same units as the commanded level.
    fn sense(&mut self) -> Result<u16, HwError>;
    /// Enable/disable the high-voltage supply. Must be idempotent.
    fn supply(&mut self, on: bool) -> Result<(), HwError>;
    /// Busy-wait. Used only inside [`Hw::critical`].
    fn delay_us(&mut self, us: u32);
    /// Run `f` with interrupts locked so a phase cannot be stretched by an ISR.
    fn critical<R>(&mut self, f: impl FnOnce(&mut Self) -> R) -> R;
}
