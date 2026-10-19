//! 12-bit, 4-channel SPI DAC with the 24-bit `CCCC AAAA DDDDDDDDDDDD xxxx` frame.
//!
//! The suit's DAC speaks exactly this frame (`3F hi lo` = write-and-update, all DACs, fw
//! @0x44c4a..), which matches the LTC262x family. The exact part is not confirmed; the logic
//! here only relies on the frame layout. The data sheet's SPI mode is 0 (clock idles low, data
//! sampled on the rising edge), which is why Zephyr's `dac_ad56xx` (mode 1) is not usable.
//!
//! * [`Ltc2634`] is the pure driver, generic over [`owo_hal`] traits and unit-tested on the host.
//! * With feature `zephyr-driver`, [`zephyr`] exports the three functions that
//!   `drivers/dac/dac_ltc2634.c` forwards Zephyr's DAC API to.

#![cfg_attr(not(test), no_std)]

use owo_hal::{Errno, OutputPin, SpiDevice};

pub const CMD_WRITE_UPDATE: u8 = 0x3;
pub const CMD_POWER_DOWN: u8 = 0x4;
/// Address that targets all DACs at once.
pub const ADDR_ALL: u8 = 0xF;
pub const CHANNELS: u8 = 4;
pub const MAX_CODE: u16 = 0x0FFF;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    BadChannel,
    BadValue,
    Spi(Errno),
    Reset(Errno),
}

/// Build a 24-bit frame. `code` is the 12-bit value, left-justified in the 16 data bits.
pub fn frame(cmd: u8, addr: u8, code: u16) -> [u8; 3] {
    let d = (code & MAX_CODE) << 4;
    [(cmd << 4) | (addr & 0xF), (d >> 8) as u8, d as u8]
}

/// Channel numbers: 0..=3, or [`ADDR_ALL`] for all outputs.
pub fn valid_channel(ch: u8) -> bool {
    ch < CHANNELS || ch == ADDR_ALL
}

pub struct Ltc2634<S: SpiDevice, R: OutputPin> {
    spi: S,
    reset: Option<R>,
}

impl<S: SpiDevice, R: OutputPin> Ltc2634<S, R> {
    /// `reset` is the active-high-means-asserted view of the part's /CLR pin: `set(true)` clears.
    pub fn new(spi: S, reset: Option<R>) -> Self {
        Self { spi, reset }
    }

    /// Pulse /CLR (if wired), which zeroes all outputs. The caller waits >= 1 us between the
    /// assert and release in `delay`.
    pub fn clear(&mut self, mut delay: impl FnMut()) -> Result<(), Error> {
        if let Some(r) = self.reset.as_mut() {
            r.set(true).map_err(Error::Reset)?;
            delay();
            r.set(false).map_err(Error::Reset)?;
        }
        Ok(())
    }

    pub fn write(&mut self, channel: u8, code: u16) -> Result<(), Error> {
        if !valid_channel(channel) {
            return Err(Error::BadChannel);
        }
        if code > MAX_CODE {
            return Err(Error::BadValue);
        }
        self.spi.write(&frame(CMD_WRITE_UPDATE, channel, code)).map_err(Error::Spi)
    }

    /// Power down the given channel (or all with [`ADDR_ALL`]).
    pub fn power_down(&mut self, channel: u8) -> Result<(), Error> {
        if !valid_channel(channel) {
            return Err(Error::BadChannel);
        }
        self.spi.write(&frame(CMD_POWER_DOWN, channel, 0)).map_err(Error::Spi)
    }
}

#[cfg(feature = "zephyr-driver")]
pub mod zephyr;

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::vec::Vec;

    #[derive(Default)]
    struct S(Vec<[u8; 3]>);
    impl SpiDevice for S {
        fn transfer(&mut self, _: &[u8], _: &mut [u8]) -> Result<(), Errno> {
            unreachable!()
        }
        fn write(&mut self, tx: &[u8]) -> Result<(), Errno> {
            self.0.push(tx.try_into().unwrap());
            Ok(())
        }
    }
    struct R(std::rc::Rc<core::cell::RefCell<Vec<bool>>>);
    impl OutputPin for R {
        fn set(&mut self, a: bool) -> Result<(), Errno> {
            self.0.borrow_mut().push(a);
            Ok(())
        }
    }

    #[test]
    fn stock_frames() {
        // fw: level 126 -> code 126*0x12FED/0x9C4 = 3921 -> `3F 3D 10`.
        let code = (126u32 * 0x12FED / 0x9C4) as u16;
        assert_eq!(code, 3921);
        assert_eq!(frame(CMD_WRITE_UPDATE, ADDR_ALL, code), [0x3F, 0xF5, 0x10]);
        assert_eq!(frame(CMD_WRITE_UPDATE, ADDR_ALL, 0), [0x3F, 0x00, 0x00]);
        assert_eq!(frame(CMD_POWER_DOWN, 0, 0), [0x40, 0, 0]);
    }

    #[test]
    fn write_validates() {
        let mut d = Ltc2634::<S, R>::new(S::default(), None);
        assert_eq!(d.write(4, 0), Err(Error::BadChannel));
        assert_eq!(d.write(0, 4096), Err(Error::BadValue));
        d.write(ADDR_ALL, 4095).unwrap();
        d.write(2, 1).unwrap();
        assert_eq!(d.spi.0, [[0x3F, 0xFF, 0xF0], [0x32, 0x00, 0x10]]);
    }

    #[test]
    fn clear_pulses_reset() {
        let log = Default::default();
        let mut d = Ltc2634::new(S::default(), Some(R(std::rc::Rc::clone(&log))));
        let mut n = 0;
        d.clear(|| n += 1).unwrap();
        assert_eq!((*log.borrow()).as_slice(), [true, false]);
        assert_eq!(n, 1);
    }
}
