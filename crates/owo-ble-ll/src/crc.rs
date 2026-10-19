//! CRC-24 (Vol 6 Part B §3.1.1).
//!
//! Polynomial x^24+x^10+x^9+x^6+x^4+x^3+x+1, 24-bit LFSR, data fed least significant bit first.
//! Preset: [`ADV_CRC_INIT`] on the advertising channels (§3.1.1), the `CRCInit` field of
//! CONNECT_IND (§2.3.3.1) on the data channels. On air the register is sent starting from
//! position 23, i.e. as three bytes `bitrev(state)` little endian; [`crc24`] returns exactly
//! those on-air bytes packed little endian (byte 0 in bits 7:0).
//!
//! Two implementations are kept: a literal bit-serial LFSR ([`crc24_reference`]) and the fast
//! reflected form ([`crc24`]); the tests require them to agree.

/// Advertising-channel CRC preset (§3.1.1).
pub const ADV_CRC_INIT: u32 = 0x55_5555;

/// Reflected form of the polynomial (taps x^10,x^9,x^6,x^4,x^3,x^1 and the x^24 feedback).
const POLY_REFLECTED: u32 = 0xDA_6000;

fn rev24(v: u32) -> u32 {
    v.reverse_bits() >> 8
}

/// Literal LFSR of the spec figure: position 0 holds the LSB of the preset.
pub fn crc24_reference(init: u32, data: &[u8]) -> u32 {
    let mut s = [false; 24];
    for (i, b) in s.iter_mut().enumerate() {
        *b = (init >> i) & 1 != 0;
    }
    for &byte in data {
        for bit in 0..8 {
            let d = (byte >> bit) & 1 != 0;
            let fb = d ^ s[23];
            for i in (1..24).rev() {
                s[i] = s[i - 1];
            }
            s[0] = fb;
            if fb {
                for &t in &[1usize, 3, 4, 6, 9, 10] {
                    s[t] = !s[t];
                }
            }
        }
    }
    // Transmitted from position 23 down to 0, each byte least significant bit first:
    // byte 0 holds positions 23..16 with position 23 in bit 0.
    let mut state = 0u32;
    for (i, &b) in s.iter().enumerate() {
        state |= (b as u32) << i;
    }
    rev24(state)
}

/// Fast CRC. Returns the three on-air bytes packed little endian.
pub fn crc24(init: u32, data: &[u8]) -> u32 {
    let mut state = rev24(init & 0xFF_FFFF);
    for &byte in data {
        let mut cur = byte as u32;
        for _ in 0..8 {
            let fb = (state ^ cur) & 1;
            cur >>= 1;
            state >>= 1;
            if fb != 0 {
                state ^= POLY_REFLECTED;
            }
        }
    }
    state
}

/// The three CRC bytes in on-air order.
pub fn crc24_bytes(init: u32, data: &[u8]) -> [u8; 3] {
    let c = crc24(init, data);
    [c as u8, (c >> 8) as u8, (c >> 16) as u8]
}

/// True when `pdu_and_crc` (PDU followed by its three CRC bytes) checks out.
pub fn check(init: u32, pdu_and_crc: &[u8]) -> bool {
    if pdu_and_crc.len() < 3 {
        return false;
    }
    let (pdu, crc) = pdu_and_crc.split_at(pdu_and_crc.len() - 3);
    crc24_bytes(init, pdu) == [crc[0], crc[1], crc[2]]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fast_equals_reference() {
        let mut x = 0x1234_5678u32;
        let mut buf = [0u8; 40];
        for init in [ADV_CRC_INIT, 0, 0xAB_CDEF, 0xFF_FFFF] {
            for len in 0..40 {
                for b in buf.iter_mut() {
                    x = x.wrapping_mul(1664525).wrapping_add(1013904223);
                    *b = (x >> 24) as u8;
                }
                assert_eq!(crc24(init, &buf[..len]), crc24_reference(init, &buf[..len]), "len {len}");
            }
        }
    }

    #[test]
    fn appended_crc_checks_and_detects_errors() {
        let pdu = [0x00, 0x09, 0xC6, 0x6C, 0x2A, 0x4F, 0x31, 0x01, 0x02, 0x01, 0x06];
        let mut frame = [0u8; 14];
        frame[..11].copy_from_slice(&pdu);
        frame[11..].copy_from_slice(&crc24_bytes(ADV_CRC_INIT, &pdu));
        assert!(check(ADV_CRC_INIT, &frame));
        for byte in 0..14 {
            for bit in 0..8 {
                let mut bad = frame;
                bad[byte] ^= 1 << bit;
                assert!(!check(ADV_CRC_INIT, &bad), "flip {byte}.{bit}");
            }
        }
        assert!(!check(0x12_3456, &frame));
    }

    #[test]
    fn empty_message_is_bitreversed_preset() {
        // With no data the register is unchanged, so the output is its transmission order.
        assert_eq!(crc24(ADV_CRC_INIT, &[]), rev24(ADV_CRC_INIT));
        assert_eq!(crc24(0x00_0001, &[]), 0x80_0000);
    }
}
