//! H-bridge with four gates, as found in the stock firmware (0x3ae02 and the sequencer).
//!
//! Gate order: `[high_a, high_b, low_a, low_b]`.
//! * Off      = low sides on (`low_a`, `low_b` active), high sides off.
//! * Positive = `high_a` + `low_b`.
//! * Negative = `high_b` + `low_a`.
//!
//! Every transition releases both high sides first, then sets the low sides, then closes the
//! new high side. Going directly between polarities is refused (the stock firmware has the same
//! guard at RAM 0x20001277) and leaves the bridge Off.

#![cfg_attr(not(test), no_std)]

use owo_hal::{Errno, OutputPin};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Off,
    Positive,
    Negative,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// Positive <-> Negative requested without passing through Off. The bridge was set Off.
    Shoot,
    Pin(Errno),
}

pub struct HBridge<P: OutputPin> {
    high_a: P,
    high_b: P,
    low_a: P,
    low_b: P,
    state: State,
}

impl<P: OutputPin> HBridge<P> {
    /// Takes the four gates and drives them to the Off state, high sides first.
    pub fn new(high_a: P, high_b: P, low_a: P, low_b: P) -> Result<Self, Error> {
        let mut b = Self { high_a, high_b, low_a, low_b, state: State::Off };
        b.force_off()?;
        Ok(b)
    }

    pub fn state(&self) -> State {
        self.state
    }

    fn force_off(&mut self) -> Result<(), Error> {
        // Attempt every gate even if one fails; report the first error.
        let r1 = self.high_a.set(false);
        let r2 = self.high_b.set(false);
        let r3 = self.low_a.set(true);
        let r4 = self.low_b.set(true);
        self.state = State::Off;
        [r1, r2, r3, r4].into_iter().find_map(|r| r.err()).map_or(Ok(()), |e| Err(Error::Pin(e)))
    }

    /// Always safe to call; used on every error path.
    pub fn off(&mut self) -> Result<(), Error> {
        self.force_off()
    }

    pub fn set(&mut self, to: State) -> Result<(), Error> {
        if to == self.state {
            return Ok(());
        }
        if self.state != State::Off && to != State::Off {
            self.force_off()?;
            return Err(Error::Shoot);
        }
        let r = (|| {
            self.high_a.set(false)?;
            self.high_b.set(false)?;
            match to {
                State::Off => {
                    self.low_a.set(true)?;
                    self.low_b.set(true)?;
                }
                State::Positive => {
                    self.low_a.set(false)?;
                    self.low_b.set(true)?;
                    self.high_a.set(true)?;
                }
                State::Negative => {
                    self.low_a.set(true)?;
                    self.low_b.set(false)?;
                    self.high_b.set(true)?;
                }
            }
            Ok(())
        })();
        match r {
            Ok(()) => {
                self.state = to;
                Ok(())
            }
            Err(e) => {
                let _ = self.force_off();
                Err(Error::Pin(e))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::rc::Rc;
    use std::vec::Vec;
    extern crate std;

    type Log = Rc<RefCell<Vec<(u8, bool)>>>;
    struct Pin(u8, Log, bool);
    impl OutputPin for Pin {
        fn set(&mut self, a: bool) -> Result<(), Errno> {
            if self.2 {
                return Err(-5);
            }
            self.1.borrow_mut().push((self.0, a));
            Ok(())
        }
    }
    fn mk(fail: Option<u8>) -> (HBridge<Pin>, Log) {
        let l: Log = Default::default();
        let p = |i| Pin(i, l.clone(), fail == Some(i));
        (HBridge::new(p(0), p(1), p(2), p(3)).unwrap(), l)
    }

    // Track gate levels while replaying the log; assert no state ever has both gates of one leg on
    // or both high sides on.
    fn replay_ok(log: &[(u8, bool)]) {
        let mut g = [false, false, true, true];
        for (i, v) in log {
            g[*i as usize] = *v;
            assert!(!(g[0] && g[1]), "both high sides on");
            assert!(!(g[0] && g[2]), "leg A shoot-through");
            assert!(!(g[1] && g[3]), "leg B shoot-through");
        }
    }

    #[test]
    fn off_first_then_positive_then_off() {
        let (mut b, l) = mk(None);
        l.borrow_mut().clear();
        b.set(State::Positive).unwrap();
        b.set(State::Off).unwrap();
        replay_ok(&l.borrow());
        assert_eq!(b.state(), State::Off);
    }

    #[test]
    fn direct_flip_is_refused_and_leaves_off() {
        let (mut b, l) = mk(None);
        b.set(State::Positive).unwrap();
        l.borrow_mut().clear();
        assert_eq!(b.set(State::Negative), Err(Error::Shoot));
        assert_eq!(b.state(), State::Off);
        replay_ok(&l.borrow());
    }

    #[test]
    fn pin_error_forces_off() {
        let l: Log = Default::default();
        let mk = |i, f| Pin(i, l.clone(), f);
        let mut b = HBridge::new(mk(0, false), mk(1, false), mk(2, false), mk(3, false)).unwrap();
        b.low_b.2 = false;
        b.high_a.2 = true;
        assert_eq!(b.set(State::Positive), Err(Error::Pin(-5)));
        assert_eq!(b.state(), State::Off);
    }
}
