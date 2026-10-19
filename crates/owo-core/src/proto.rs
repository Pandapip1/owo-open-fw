//! Wire formats. See `myowo-ble-protocol.md` for the evidence behind each field.

pub const NUM_CHANNELS: usize = 10;
pub const FRAME_LEN: usize = 20; // "Microsensacion" write, fw @0x26340
pub const CONFIG_LEN: usize = 10; // "Extra 1" write,        fw @0x25E00
pub const POWER_LEN: usize = 10; // "Power output config",  fw @0x26194
pub const WIDTHS_LEN: usize = 10; // "Positive pulse duration", fw @0x25F12
/// Values >= 0x7E are rejected, fw @0x26270..
pub const POWER_MAX: u8 = 0x7D;

/// Notification value sent on "Start / Stop" when a frame checksum fails (fw @0x263AC).
pub const NOTIFY_BAD_CHECKSUM: u8 = 0x86;

/// Commands written to "Start / Stop".
pub mod run {
    pub const STOP: u8 = 0x00;
    pub const START: u8 = 0x25;
    pub const RESTART: u8 = 0x26;
    /// Sets an internal state; unused by both apps.
    pub const STATE9: u8 = 0xAA;
    pub const ABORT: u8 = 0xFF;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// Wrong packet length.
    Len,
    /// Checksum mismatch.
    Csum,
    /// A value is outside the accepted range.
    Range,
}

pub fn xor(p: &[u8]) -> u8 {
    p.iter().fold(0, |a, b| a ^ b)
}

// ---- 20-byte order frame -----------------------------------------------

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Frame {
    /// Wire order, bytes 0..9.
    pub level: [u8; NUM_CHANNELS],
    /// Byte 10: (work+rest)*repeats, 100 ms units.
    pub total: u8,
    /// Byte 11: 100 ms units, meaning not traced in the firmware.
    pub exit_delay: u8,
    /// Byte 12.
    pub freq_hz: u8,
    /// Byte 13, 100 ms units.
    pub work: u8,
    /// Byte 14, 100 ms units.
    pub rest: u8,
    /// Byte 15, 100 ms units, clamped to `work`.
    pub ramp_up: u8,
    /// Byte 16, 100 ms units, clamped to `work`.
    pub ramp_down: u8,
    /// Byte 17 bit 0: flush everything queued before.
    pub first: bool,
}

impl Frame {
    pub const ZERO: Frame = Frame {
        level: [0; NUM_CHANNELS],
        total: 0,
        exit_delay: 0,
        freq_hz: 0,
        work: 0,
        rest: 0,
        ramp_up: 0,
        ramp_down: 0,
        first: false,
    };

    pub fn parse(buf: &[u8]) -> Result<Frame, Error> {
        if buf.len() != FRAME_LEN {
            return Err(Error::Len);
        }
        if xor(&buf[..19]) != buf[19] {
            return Err(Error::Csum);
        }
        let mut level = [0u8; NUM_CHANNELS];
        level.copy_from_slice(&buf[..NUM_CHANNELS]);
        Ok(Frame {
            level,
            total: buf[10],
            exit_delay: buf[11],
            freq_hz: buf[12],
            work: buf[13],
            rest: buf[14],
            // fw @0x26442..0x26460: both ramps are clamped to the work time
            ramp_up: buf[15].min(buf[13]),
            ramp_down: buf[16].min(buf[13]),
            first: buf[17] & 1 != 0,
        })
    }

    pub fn encode(&self) -> [u8; FRAME_LEN] {
        let mut o = [0u8; FRAME_LEN];
        o[..NUM_CHANNELS].copy_from_slice(&self.level);
        o[10] = self.total;
        o[11] = self.exit_delay;
        o[12] = self.freq_hz;
        o[13] = self.work;
        o[14] = self.rest;
        o[15] = self.ramp_up;
        o[16] = self.ramp_down;
        o[17] = self.first as u8;
        o[19] = xor(&o[..19]);
        o
    }
}

// ---- 10-byte config packet ("Extra 1") -----------------------------------

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Config {
    /// 0 -> 1, 0xFF keeps the previous value.
    pub duration: u8,
    /// 0 -> 1, >100 -> 100.
    pub freq_hz: u8,
    /// 0 -> 1.
    pub work: u8,
    /// 0 -> 1.
    pub rest: u8,
    /// byte4 * 10, clamped to work * 1000; unit unverified.
    pub ramp_up_raw: u16,
    /// byte5 * 10, same clamp.
    pub ramp_down_raw: u16,
    /// Byte 6: 0,1 -> 14; 2 -> 21; else as given; use unknown.
    pub xgatt: u8,
    /// Byte 7: <=1 -> 2; use unknown.
    pub aux7: u8,
    /// Byte 8: capped at 10; use unknown.
    pub aux8: u8,
}

impl Config {
    /// `prev` supplies the duration kept when the packet carries 0xFF.
    pub fn parse(buf: &[u8], prev: Option<&Config>) -> Result<Config, Error> {
        if buf.len() != CONFIG_LEN {
            return Err(Error::Len);
        }
        if xor(&buf[..9]) != buf[9] {
            return Err(Error::Csum);
        }
        // fw @0x25E3C: 0 -> 1, 0xFF -> keep, else as given
        let duration = match buf[0] {
            0 => 1,
            0xFF => prev.map_or(1, |p| p.duration),
            d => d,
        };
        // fw @0x25E56: 0 -> 1, >=101 -> 100
        let freq_hz = match buf[1] {
            0 => 1,
            f if f >= 101 => 100,
            f => f,
        };
        let work = buf[2].max(1);
        let rest = buf[3].max(1);
        // fw @0x25E90..0x25EC6
        let lim = work as u32 * 1000;
        let ramp_up_raw = (buf[4] as u32 * 10).min(lim) as u16;
        let ramp_down_raw = (buf[5] as u32 * 10).min(lim) as u16;
        // fw @0x25EC8..
        let xgatt = match buf[6] {
            0 | 1 => 14,
            2 => 21,
            x => x,
        };
        Ok(Config {
            duration,
            freq_hz,
            work,
            rest,
            ramp_up_raw,
            ramp_down_raw,
            xgatt,
            aux7: if buf[7] > 1 { buf[7] } else { 2 },
            aux8: buf[8].min(10),
        })
    }

    #[allow(clippy::too_many_arguments)]
    pub fn encode(
        duration: u8,
        freq: u8,
        work: u8,
        rest: u8,
        ramp_up_100ms: u8,
        ramp_down_100ms: u8,
        xgatt: u8,
    ) -> [u8; CONFIG_LEN] {
        let mut o = [
            duration,
            freq,
            work,
            rest,
            ramp_up_100ms,
            ramp_down_100ms,
            xgatt,
            0,
            0,
            0,
        ];
        o[9] = xor(&o[..9]);
        o
    }
}

// ---- power ---------------------------------------------------------------

/// fw @0x261BC..0x26204: the checksum travels in the MSBs of bytes 0..7.
fn power_csum_from_msbs(b: &[u8]) -> u8 {
    (b[0] & 0x80)
        | ((b[1] >> 1) & 0x40)
        | ((b[2] >> 2) & 0x20)
        | ((b[3] >> 3) & 0x10)
        | ((b[4] >> 4) & 0x08)
        | ((b[5] >> 5) & 0x04)
        | ((b[6] >> 6) & 0x02)
        | (b[7] >> 7)
}

pub fn power_parse(buf: &[u8]) -> Result<[u8; NUM_CHANNELS], Error> {
    if buf.len() != POWER_LEN {
        return Err(Error::Len);
    }
    let mut v = [0u8; NUM_CHANNELS];
    let mut x = 0;
    for (o, b) in v.iter_mut().zip(buf) {
        *o = b & 0x7F;
        x ^= *o;
    }
    if x != power_csum_from_msbs(buf) {
        return Err(Error::Csum);
    }
    if v.iter().any(|&l| l > POWER_MAX) {
        return Err(Error::Range);
    }
    Ok(v)
}

pub fn power_encode(levels: &[u8; NUM_CHANNELS]) -> Result<[u8; POWER_LEN], Error> {
    if levels.iter().any(|&l| l > POWER_MAX) {
        return Err(Error::Range);
    }
    let x = xor(levels);
    let mut o = *levels;
    for (i, b) in o.iter_mut().take(8).enumerate() {
        *b |= ((x >> (7 - i)) & 1) << 7;
    }
    Ok(o)
}

// ---- pulse widths --------------------------------------------------------

const WIDTH_TBL: [u16; 8] = [50, 50, 75, 100, 125, 150, 175, 200];
const GAP_TBL: [u16; 4] = [0, 50, 100, 0];

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Width {
    pub a_us: u16,
    pub gap_us: u16,
    pub b_us: u16,
}

impl Width {
    pub const ZERO: Width = Width { a_us: 0, gap_us: 0, b_us: 0 };

    pub const fn decode(b: u8) -> Width {
        Width {
            a_us: WIDTH_TBL[(b & 7) as usize],
            b_us: WIDTH_TBL[((b >> 3) & 7) as usize],
            gap_us: GAP_TBL[(b >> 6) as usize],
        }
    }
}

pub fn widths_parse(buf: &[u8]) -> Result<[Width; NUM_CHANNELS], Error> {
    if buf.len() != WIDTHS_LEN {
        return Err(Error::Len);
    }
    let mut w = [Width::default(); NUM_CHANNELS];
    for (o, b) in w.iter_mut().zip(buf) {
        *o = Width::decode(*b);
    }
    Ok(w)
}

// ---- pairing -------------------------------------------------------------

/// Jenkins one-at-a-time hash mod 1,000,000: the suit's fixed BLE passkey.
pub fn pairing_code(serial: &[u8]) -> u32 {
    let mut h: u32 = 0;
    for &c in serial {
        h = h.wrapping_add(c as u32);
        h = h.wrapping_add(h << 10);
        h ^= h >> 6;
    }
    h = h.wrapping_add(h << 3);
    h ^= h >> 11;
    h = h.wrapping_add(h << 15);
    h % 1_000_000
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frame() {
        // Worked example from the protocol document: pectoral_r=60 (wire byte 4),
        // dorsal_l=40 (wire byte 8), 100 Hz, 5 x 100 ms, no ramps, first.
        let ex: [u8; 20] = [
            0, 0, 0, 0, 0x3C, 0, 0, 0, 0x28, 0, 5, 0, 100, 5, 0, 0, 0, 1, 0, 0x71,
        ];
        let f = Frame::parse(&ex).unwrap();
        assert!(f.level[4] == 60 && f.level[8] == 40 && f.freq_hz == 100);
        assert!(f.total == 5 && f.work == 5 && f.rest == 0 && f.first);
        assert_eq!(f.encode(), ex);

        let mut bad = ex;
        bad[19] ^= 1;
        assert_eq!(Frame::parse(&bad), Err(Error::Csum));
        assert_eq!(Frame::parse(&ex[..19]), Err(Error::Len));
        assert_eq!(Frame::parse(&[&ex[..], &[0]].concat()), Err(Error::Len));

        // ramps are clamped to the work time
        let g = Frame { freq_hz: 50, work: 3, rest: 1, total: 4, ramp_up: 25, ramp_down: 2, ..Default::default() };
        let f = Frame::parse(&g.encode()).unwrap();
        assert!(f.ramp_up == 3 && f.ramp_down == 2);
    }

    #[test]
    fn config() {
        let ex: [u8; 10] = [0xFE, 0x64, 0x05, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x97];
        let p = Config { duration: 7, ..Default::default() };
        let c = Config::parse(&ex, Some(&p)).unwrap();
        assert!(c.duration == 0xFE && c.freq_hz == 100 && c.work == 5 && c.rest == 1);
        assert!(c.xgatt == 8 && c.aux7 == 2 && c.aux8 == 0);
        assert_eq!(Config::encode(0xFE, 100, 5, 0, 0, 0, 8), ex);

        // duration 0xFF keeps the previous value; 0 becomes 1
        let b = Config::encode(0xFF, 200, 0, 0, 250, 250, 2);
        let c = Config::parse(&b, Some(&p)).unwrap();
        assert!(c.duration == 7 && c.freq_hz == 100 && c.work == 1 && c.rest == 1);
        assert_eq!(c.xgatt, 21);
        assert!(c.ramp_up_raw == 1000 && c.ramp_down_raw == 1000); // 2500 clamped to work*1000
        let mut b = Config::encode(0, 0, 5, 5, 3, 4, 0);
        let c = Config::parse(&b, None).unwrap();
        assert!(c.duration == 1 && c.freq_hz == 1 && c.xgatt == 14);
        assert!(c.ramp_up_raw == 30 && c.ramp_down_raw == 40);

        b[9] ^= 0x10;
        assert_eq!(Config::parse(&b, None), Err(Error::Csum));
        assert_eq!(Config::parse(&b[..9], None), Err(Error::Len));
    }

    #[test]
    fn power() {
        let mut inp = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
        let enc = power_encode(&inp).unwrap();
        assert_eq!(power_parse(&enc).unwrap(), inp);

        // checksum bits live in bytes 0..7 only: XOR of 1..10 = 11 = 0x0B
        assert!(enc[4] & 0x80 != 0 && enc[6] & 0x80 != 0 && enc[7] & 0x80 != 0);
        assert!(enc[0] & 0x80 == 0 && enc[8] & 0x80 == 0);

        let mut bad = enc;
        bad[3] ^= 1;
        assert_eq!(power_parse(&bad), Err(Error::Csum));
        inp[2] = 0x7E;
        assert_eq!(power_encode(&inp), Err(Error::Range));

        // a packet whose checksum is valid but one value is 0x7E must be rejected
        let mut raw = [0x7E, 0, 0, 0, 0, 0, 0, 0, 0, 0];
        for (i, b) in raw.iter_mut().take(8).enumerate() {
            *b |= ((0x7Eu8 >> (7 - i)) & 1) << 7;
        }
        assert_eq!(power_parse(&raw), Err(Error::Range));
    }

    #[test]
    fn widths() {
        let w = Width::decode(0x6D);
        assert!(w.a_us == 150 && w.b_us == 150 && w.gap_us == 50);
        let w = Width::decode(0x49);
        assert!(w.a_us == 50 && w.b_us == 50 && w.gap_us == 50);
        let w = Width::decode(0x00);
        assert!(w.a_us == 50 && w.b_us == 50 && w.gap_us == 0);
        let w = Width::decode(0xFF);
        assert!(w.a_us == 200 && w.b_us == 200 && w.gap_us == 0);
        let w = Width::decode(0x97); // 10 010 111: gap 100
        assert!(w.a_us == 200 && w.b_us == 75 && w.gap_us == 100);

        let tbl = [0x6D, 0x49, 0x49, 0x49, 0x49, 0x6D, 0x6D, 0x49, 0x6D, 0x49];
        let ws = widths_parse(&tbl).unwrap();
        assert!(ws[0].a_us == 150 && ws[1].a_us == 50 && ws[8].a_us == 150);
        assert_eq!(widths_parse(&tbl[..9]), Err(Error::Len));
    }

    #[test]
    fn pairing() {
        // app-side vectors from owo_ref.py, firmware default serial buffers
        assert_eq!(pairing_code(b"0000000000"), 981255);
        assert_eq!(pairing_code(b"1234567890"), 269566);
        assert_eq!(pairing_code(b"000000000000"), 426134);
        assert_eq!(pairing_code(b"999999999999"), 277805);
    }
}
