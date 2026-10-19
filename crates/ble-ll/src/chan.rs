//! Channel selection: algorithm #1 (§4.5.8.2) and #2 (§4.5.8.3).
//!
//! Channel *indices* are used throughout: 0..=36 data, 37..=39 advertising (RF channels 0..=39 map
//! to indices as in §1.4.1: [`rf_channel_to_index`]).

/// Data-channel map: bit n of byte n/8 set = channel n is used (ChM field of CONNECT_IND).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ChannelMap(pub [u8; 5]);

impl ChannelMap {
    /// All 37 data channels.
    pub const ALL: ChannelMap = ChannelMap([0xFF, 0xFF, 0xFF, 0xFF, 0x1F]);

    /// Is `ch` (0..=36) used?
    pub fn is_used(&self, ch: u8) -> bool {
        ch < 37 && self.0[(ch >> 3) as usize] & (1 << (ch & 7)) != 0
    }

    /// Number of used channels (bits above 36 are ignored).
    pub fn count(&self) -> u8 {
        (0..37u8).filter(|&c| self.is_used(c)).count() as u8
    }

    /// The `index`-th used channel counting upward from 0 (the "remapping table" of §4.5.8.2).
    pub fn nth_used(&self, index: u8) -> Option<u8> {
        (0..37u8).filter(|&c| self.is_used(c)).nth(index as usize)
    }
}

/// RF channel number (0..=39, centre 2402 + 2n MHz) to channel index (§1.4.1 / Vol 6 Part B 1.4.1):
/// advertising RF 0,12,39 are indices 37,38,39; the rest are data channels 0..=36 in order.
pub fn rf_channel_to_index(rf: u8) -> u8 {
    match rf {
        0 => 37,
        1..=11 => rf - 1,
        12 => 38,
        13..=38 => rf - 2,
        _ => 39,
    }
}

/// Inverse of [`rf_channel_to_index`].
pub fn index_to_rf_channel(idx: u8) -> u8 {
    match idx {
        37 => 0,
        38 => 12,
        39 => 39,
        0..=10 => idx + 1,
        _ => idx + 2,
    }
}

/// Centre frequency in MHz of a channel index.
pub fn index_to_mhz(idx: u8) -> u16 {
    2402 + 2 * index_to_rf_channel(idx) as u16
}

/// Algorithm #1 (§4.5.8.2). `last_unmapped` is the previous event's *unmapped* channel (0 at
/// connection start); `skipped` is the number of connection events since the previous call minus
/// one (0 when called every event), so peripheral latency is handled by advancing the hop
/// accordingly. Returns `(new unmapped channel, used channel)`.
pub fn csa1(last_unmapped: u8, hop: u8, skipped: u16, map: &ChannelMap) -> (u8, u8) {
    let step = (hop as u32) * (1 + skipped as u32);
    let unmapped = ((last_unmapped as u32 + step) % 37) as u8;
    if map.is_used(unmapped) {
        (unmapped, unmapped)
    } else {
        let idx = unmapped % map.count().max(1);
        (unmapped, map.nth_used(idx).unwrap_or(unmapped))
    }
}

/// Channel identifier for algorithm #2 (§4.5.8.3.1): AA[31:16] xor AA[15:0].
pub fn channel_id(access_address: u32) -> u16 {
    (access_address >> 16) as u16 ^ access_address as u16
}

fn perm(v: u16) -> u16 {
    // Reverse the bits inside each byte (§4.5.8.3.2 permutation).
    let lo = (v as u8).reverse_bits() as u16;
    let hi = ((v >> 8) as u8).reverse_bits() as u16;
    (hi << 8) | lo
}

fn mam(a: u16, b: u16) -> u16 {
    // Multiply, add, modulo 2^16 (§4.5.8.3.2): (17 * a + b) mod 2^16.
    a.wrapping_mul(17).wrapping_add(b)
}

/// prn_e of §4.5.8.3.3.
pub fn prn_e(counter: u16, chan_id: u16) -> u16 {
    let mut x = counter ^ chan_id;
    for _ in 0..3 {
        x = mam(perm(x), chan_id);
    }
    x ^ chan_id
}

/// Algorithm #2 (§4.5.8.3.1): data channel for connection event `counter`.
pub fn csa2(counter: u16, chan_id: u16, map: &ChannelMap) -> u8 {
    let prn = prn_e(counter, chan_id);
    let unmapped = (prn % 37) as u8;
    if map.is_used(unmapped) {
        unmapped
    } else {
        let idx = ((map.count() as u32 * prn as u32) >> 16) as u8;
        map.nth_used(idx).unwrap_or(unmapped)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Vectors: Core v5.2 Vol 6 Part C §3.1 and §3.2 (LE channel selection algorithm #2 sample
    // data), as quoted in zephyr/subsys/bluetooth/controller/ll_sw/lll_chan.c lll_chan_sel_2_ut().
    const MAP1: ChannelMap = ChannelMap([0xFF, 0xFF, 0xFF, 0xFF, 0x1F]);
    const MAP2: ChannelMap = ChannelMap([0x00, 0x06, 0xE0, 0x00, 0x1E]);
    const ID: u16 = 0x305F;

    #[test]
    fn channel_id_of_sample_access_address() {
        // Same file: AA 0x8E89BED6 gives channel identifier 0x305F.
        assert_eq!(channel_id(0x8E89_BED6), ID);
    }

    #[test]
    fn csa2_sample_data_1_all_37_channels() {
        assert_eq!(prn_e(0, ID), 56857); // quoted as prn_e for event 0 in the same test
        assert_eq!(csa2(0, ID, &MAP1), 25);
        assert_eq!(csa2(1, ID, &MAP1), 20);
        assert_eq!(csa2(2, ID, &MAP1), 6);
        assert_eq!(csa2(3, ID, &MAP1), 21);
    }

    #[test]
    fn csa2_sample_data_2_nine_channels() {
        assert_eq!(MAP2.count(), 9);
        assert_eq!(csa2(6, ID, &MAP2), 23);
        assert_eq!(csa2(7, ID, &MAP2), 9);
        assert_eq!(csa2(8, ID, &MAP2), 34);
    }

    #[test]
    fn csa2_always_returns_used_channel() {
        for map in [MAP1, MAP2, ChannelMap([0x06, 0, 0, 0, 0])] {
            for c in 0..=u16::MAX {
                assert!(map.is_used(csa2(c, ID, &map)));
            }
        }
    }

    #[test]
    fn csa1_hops_and_remaps() {
        // hop 5 from 0: 5, 10, 15 ...
        let mut last = 0;
        for want in [5u8, 10, 15, 20, 25, 30, 35, 3] {
            let (u, used) = csa1(last, 5, 0, &ChannelMap::ALL);
            assert_eq!((u, used), (want, want));
            last = u;
        }
        // Unused unmapped channel is remapped by (unmapped mod numUsed) into the used list.
        let map = ChannelMap([0x06, 0, 0, 0, 0]); // channels 1 and 2
        let (u, used) = csa1(0, 5, 0, &map);
        assert_eq!(u, 5);
        assert_eq!(used, map.nth_used(5 % 2).unwrap());
        // Skipped events advance the hop by (1 + skipped).
        assert_eq!(csa1(0, 5, 2, &ChannelMap::ALL).0, 15);
    }

    #[test]
    fn csa1_visits_all_channels_for_every_hop() {
        for hop in 5..=16u8 {
            let mut seen = [false; 37];
            let mut last = 0;
            for _ in 0..37 {
                let (u, used) = csa1(last, hop, 0, &ChannelMap::ALL);
                seen[used as usize] = true;
                last = u;
            }
            assert!(seen.iter().all(|&s| s), "hop {hop}");
        }
    }

    #[test]
    fn rf_index_mapping() {
        for idx in 0..40u8 {
            assert_eq!(rf_channel_to_index(index_to_rf_channel(idx)), idx);
        }
        // Advertising channels 37/38/39 are 2402/2426/2480 MHz (§1.4.1).
        assert_eq!(index_to_mhz(37), 2402);
        assert_eq!(index_to_mhz(38), 2426);
        assert_eq!(index_to_mhz(39), 2480);
        assert_eq!(index_to_mhz(0), 2404);
        assert_eq!(index_to_mhz(10), 2424);
        assert_eq!(index_to_mhz(11), 2428);
        assert_eq!(index_to_mhz(36), 2478);
    }
}
