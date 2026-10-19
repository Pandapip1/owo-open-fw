//! Data whitening (Vol 6 Part B §3.2).
//!
//! 7-bit LFSR, polynomial x^7+x^4+1. Position 0 is set to 1 and positions 1..6 to the channel
//! index (position 1 = MSB of the 6-bit index). The bit leaving position 6 is XORed onto the data
//! (header, payload and CRC, each byte least significant bit first) and fed back into position 0
//! and, XORed, into position 4. Whitening is its own inverse.

/// Whitening state for RF channel *index* `ch` (0..=39; the "channel index" of §1.4.1 /
/// Vol 6 Part B §1.4.1, not the RF channel number).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Whitener {
    s: [bool; 7],
}

impl Whitener {
    /// Initialise for a channel index.
    pub fn new(ch: u8) -> Self {
        let mut s = [false; 7];
        s[0] = true;
        for i in 0..6 {
            s[1 + i] = (ch >> (5 - i)) & 1 != 0;
        }
        Whitener { s }
    }

    /// Next whitening bit; advances the LFSR.
    fn step(&mut self) -> bool {
        let out = self.s[6];
        self.s[6] = self.s[5];
        self.s[5] = self.s[4];
        self.s[4] = self.s[3] ^ out;
        self.s[3] = self.s[2];
        self.s[2] = self.s[1];
        self.s[1] = self.s[0];
        self.s[0] = out;
        out
    }

    /// XOR the whitening sequence onto `data` in place.
    pub fn apply(&mut self, data: &mut [u8]) {
        for b in data {
            for bit in 0..8 {
                if self.step() {
                    *b ^= 1 << bit;
                }
            }
        }
    }
}

/// Whiten (or de-whiten) `data` for channel index `ch`.
pub fn whiten(ch: u8, data: &mut [u8]) {
    Whitener::new(ch).apply(data);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn involution_on_every_channel() {
        let orig: [u8; 20] = core::array::from_fn(|i| (i as u8).wrapping_mul(37).wrapping_add(5));
        for ch in 0..40u8 {
            let mut d = orig;
            whiten(ch, &mut d);
            assert_ne!(d, orig, "channel {ch}");
            whiten(ch, &mut d);
            assert_eq!(d, orig, "channel {ch}");
        }
    }

    #[test]
    fn period_is_127_bits() {
        let mut w = Whitener::new(37);
        let first = w.s;
        let mut n = 0;
        loop {
            w.step();
            n += 1;
            if w.s == first {
                break;
            }
            assert!(n <= 127);
        }
        assert_eq!(n, 127, "x^7+x^4+1 is primitive");
    }

    #[test]
    fn channels_differ() {
        let mut a = [0u8; 8];
        let mut b = [0u8; 8];
        whiten(37, &mut a);
        whiten(38, &mut b);
        assert_ne!(a, b);
    }
}
