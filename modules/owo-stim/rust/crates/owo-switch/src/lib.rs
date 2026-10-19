//! The electrode channel switch: a chain of four shift-register bytes with a latch input, as used
//! by the stock firmware (`fw @0x444c8`, table at RAM 0x20000E6C recovered by emulation).
//!
//! Protocol: drop the latch line, shift four bytes, raise the latch. Then, *with the latch left
//! high*, clock four zero bytes and compare what comes back with what was written. Pulsing the
//! latch again after shifting zeros in would latch zeros and disconnect the channel.

#![cfg_attr(not(test), no_std)]

use owo_hal::{Errno, OutputPin, SpiDevice};

/// Route bytes for electrode channel 0..=10, as the stock firmware stores them.
pub const TABLE: [[u8; 4]; 11] = [
    [0x01, 0x00, 0x01, 0x00],
    [0x00, 0x10, 0x00, 0x10],
    [0x00, 0x40, 0x00, 0x40],
    [0x00, 0x01, 0x00, 0x01],
    [0x00, 0x04, 0x00, 0x04],
    [0x00, 0x80, 0x00, 0x80],
    [0x00, 0x02, 0x00, 0x02],
    [0x00, 0x08, 0x00, 0x08],
    [0x00, 0x20, 0x00, 0x20],
    [0x02, 0x00, 0x02, 0x00],
    [0x04, 0x00, 0x04, 0x00],
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    BadChannel,
    Spi(Errno),
    Latch(Errno),
    /// The read-back differed from what was written; the switch was cleared.
    Verify,
}

pub struct Switch<S: SpiDevice, L: OutputPin> {
    spi: S,
    latch: L,
}

impl<S: SpiDevice, L: OutputPin> Switch<S, L> {
    pub fn new(spi: S, latch: L) -> Self {
        Self { spi, latch }
    }

    fn load(&mut self, frame: [u8; 4]) -> Result<(), Error> {
        self.latch.set(false).map_err(Error::Latch)?;
        self.spi.write(&frame).map_err(Error::Spi)?;
        self.latch.set(true).map_err(Error::Latch)
    }

    /// Connect `channel` (or nothing) and verify by read-back.
    pub fn select(&mut self, channel: Option<u8>) -> Result<(), Error> {
        let Some(c) = channel else {
            return self.load([0; 4]);
        };
        let want = *TABLE.get(c as usize).ok_or(Error::BadChannel)?;
        self.load(want)?;
        let mut back = [0u8; 4];
        // Latch stays high here.
        let r = self.spi.transfer(&[0; 4], &mut back).map_err(Error::Spi);
        if r.is_ok() && back == want {
            return Ok(());
        }
        let _ = self.load([0; 4]);
        r.and(Err(Error::Verify))
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::cell::RefCell;
    use std::rc::Rc;
    use std::vec::Vec;

    #[derive(Debug, PartialEq, Clone)]
    enum Ev {
        Latch(bool),
        Write([u8; 4]),
        Xfer,
    }
    type Log = Rc<RefCell<Vec<Ev>>>;
    struct L(Log);
    impl OutputPin for L {
        fn set(&mut self, a: bool) -> Result<(), Errno> {
            self.0.borrow_mut().push(Ev::Latch(a));
            Ok(())
        }
    }
    struct S(Log, [u8; 4]);
    impl SpiDevice for S {
        fn transfer(&mut self, _: &[u8], rx: &mut [u8]) -> Result<(), Errno> {
            self.0.borrow_mut().push(Ev::Xfer);
            rx.copy_from_slice(&self.1);
            Ok(())
        }
        fn write(&mut self, tx: &[u8]) -> Result<(), Errno> {
            self.0.borrow_mut().push(Ev::Write(tx.try_into().unwrap()));
            Ok(())
        }
    }

    #[test]
    fn select_shifts_latches_then_reads_back_with_latch_high() {
        let log: Log = Default::default();
        let mut sw = Switch::new(S(log.clone(), TABLE[3]), L(log.clone()));
        sw.select(Some(3)).unwrap();
        assert_eq!(
            *log.borrow(),
            [Ev::Latch(false), Ev::Write(TABLE[3]), Ev::Latch(true), Ev::Xfer]
        );
    }

    #[test]
    fn mismatch_clears_switch() {
        let log: Log = Default::default();
        let mut sw = Switch::new(S(log.clone(), [0; 4]), L(log.clone()));
        assert_eq!(sw.select(Some(0)), Err(Error::Verify));
        assert_eq!(log.borrow().last(), Some(&Ev::Latch(true)));
        assert!(log.borrow().contains(&Ev::Write([0; 4])));
    }

    #[test]
    fn bad_channel_touches_nothing() {
        let log: Log = Default::default();
        let mut sw = Switch::new(S(log.clone(), [0; 4]), L(log.clone()));
        assert_eq!(sw.select(Some(11)), Err(Error::BadChannel));
        assert!(log.borrow().is_empty());
    }

    #[test]
    fn table_is_two_identical_halves() {
        for r in TABLE {
            assert_eq!(r[..2], r[2..]);
        }
    }
}
