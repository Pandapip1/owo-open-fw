//! The few traits the per-part driver crates need from a platform. `zephyr-io` implements them
//! over Zephyr's GPIO and SPI APIs; the unit tests implement them with mocks.

#![no_std]

/// A negative Zephyr errno value (e.g. `-5` for EIO). Kept as a plain integer so the part
/// drivers do not depend on any platform crate.
pub type Errno = i32;

/// A digital output. `set(true)` drives the *logical* active level (the devicetree flags decide
/// which electrical level that is).
pub trait OutputPin {
    fn set(&mut self, active: bool) -> Result<(), Errno>;
}

/// A full-duplex SPI device. The chip select, mode and clock are configured by the platform
/// (devicetree); a part driver only decides what bytes to send.
pub trait SpiDevice {
    /// Clock `tx` out and capture the same number of bytes into `rx`.
    fn transfer(&mut self, tx: &[u8], rx: &mut [u8]) -> Result<(), Errno>;
    /// Clock `tx` out and discard the reply.
    fn write(&mut self, tx: &[u8]) -> Result<(), Errno>;
}
