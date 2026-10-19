//! The electrode channel switch: one switch pin per route, driven through a GPIO port. The pin
//! assignment per channel comes from the board (devicetree `switch-routes`), so this crate is only
//! the channel -> pin-mask mapping and the "clear on failure" rule. Latching, shifting and
//! read-back verification belong to the GPIO driver underneath (in Zephyr: `ti,sn74hc595` with
//! `verify-readback`); a verification failure surfaces as `EIO` from `set_masked`.

#![cfg_attr(not(test), no_std)]

use owo_hal::{Errno, GpioPort};

const EIO: Errno = -5;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    BadChannel,
    /// The port write failed.
    Port(Errno),
    /// The port reported that the outputs did not read back as written; the switch was cleared.
    Verify,
}

pub struct Switch<'a, P: GpioPort> {
    port: P,
    routes: &'a [u32],
    all: u32,
}

impl<'a, P: GpioPort> Switch<'a, P> {
    /// `routes[ch]` is the set of port pins that are on for channel `ch`.
    pub fn new(port: P, routes: &'a [u32]) -> Self {
        let all = routes.iter().fold(0, |a, r| a | r);
        Self { port, routes, all }
    }

    /// Connect `channel` (or nothing). On any failure the switch is cleared (best effort).
    pub fn select(&mut self, channel: Option<u8>) -> Result<(), Error> {
        let want = match channel {
            None => 0,
            Some(c) => *self.routes.get(c as usize).ok_or(Error::BadChannel)?,
        };
        match self.port.set_masked(self.all, want) {
            Ok(()) => Ok(()),
            Err(e) => {
                let _ = self.port.set_masked(self.all, 0);
                Err(if e == EIO { Error::Verify } else { Error::Port(e) })
            }
        }
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::cell::RefCell;
    use std::rc::Rc;
    use std::vec::Vec;

    type Log = Rc<RefCell<Vec<(u32, u32)>>>;
    struct P(Log, Option<Errno>);
    impl GpioPort for P {
        fn set_masked(&mut self, m: u32, v: u32) -> Result<(), Errno> {
            self.0.borrow_mut().push((m, v));
            match self.1 {
                Some(e) if v != 0 => Err(e),
                _ => Ok(()),
            }
        }
    }
    const R: [u32; 3] = [0x0100_0100, 0x0010_0010, 0x0040_0040];

    #[test]
    fn select_writes_route_under_full_mask() {
        let log: Log = Default::default();
        let mut sw = Switch::new(P(log.clone(), None), &R);
        sw.select(Some(1)).unwrap();
        sw.select(None).unwrap();
        assert_eq!(*log.borrow(), [(0x0150_0150, 0x0010_0010), (0x0150_0150, 0)]);
    }

    #[test]
    fn eio_is_verify_and_clears() {
        let log: Log = Default::default();
        let mut sw = Switch::new(P(log.clone(), Some(-5)), &R);
        assert_eq!(sw.select(Some(0)), Err(Error::Verify));
        assert_eq!(log.borrow().last(), Some(&(0x0150_0150, 0)));
    }

    #[test]
    fn other_errors_are_port_errors() {
        let log: Log = Default::default();
        let mut sw = Switch::new(P(log, Some(-16)), &R);
        assert_eq!(sw.select(Some(0)), Err(Error::Port(-16)));
    }

    #[test]
    fn bad_channel_touches_nothing() {
        let log: Log = Default::default();
        let mut sw = Switch::new(P(log.clone(), None), &R);
        assert_eq!(sw.select(Some(3)), Err(Error::BadChannel));
        assert!(log.borrow().is_empty());
    }
}
