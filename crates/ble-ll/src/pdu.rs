//! Link-layer PDUs: advertising channel (§2.3) and data channel (§2.4) headers, CONNECT_IND
//! contents (§2.3.3.1).

/// Advertising PDU types (§2.3, Table 2.3), legacy.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum AdvType {
    /// ADV_IND
    AdvInd = 0,
    /// ADV_DIRECT_IND
    AdvDirectInd = 1,
    /// ADV_NONCONN_IND
    AdvNonconnInd = 2,
    /// SCAN_REQ
    ScanReq = 3,
    /// SCAN_RSP
    ScanRsp = 4,
    /// CONNECT_IND
    ConnectInd = 5,
    /// ADV_SCAN_IND
    AdvScanInd = 6,
}

impl AdvType {
    /// Decode the 4-bit type field.
    pub fn from_bits(v: u8) -> Option<AdvType> {
        Some(match v & 0xF {
            0 => AdvType::AdvInd,
            1 => AdvType::AdvDirectInd,
            2 => AdvType::AdvNonconnInd,
            3 => AdvType::ScanReq,
            4 => AdvType::ScanRsp,
            5 => AdvType::ConnectInd,
            6 => AdvType::AdvScanInd,
            _ => return None,
        })
    }
}

/// Advertising PDU header (2 bytes).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AdvHeader {
    /// PDU type
    pub ty: AdvType,
    /// ChSel: the sender supports channel selection algorithm #2
    pub ch_sel: bool,
    /// TxAdd: transmitter address is random
    pub tx_add: bool,
    /// RxAdd: receiver address is random
    pub rx_add: bool,
    /// Payload length (6..=37 for legacy PDUs)
    pub length: u8,
}

impl AdvHeader {
    /// Encode.
    pub fn to_bytes(&self) -> [u8; 2] {
        [
            self.ty as u8 | (self.ch_sel as u8) << 5 | (self.tx_add as u8) << 6 | (self.rx_add as u8) << 7,
            self.length,
        ]
    }

    /// Decode; `None` for unknown types or a length that cannot be a legacy PDU.
    pub fn parse(b: &[u8]) -> Option<AdvHeader> {
        if b.len() < 2 || b[1] > 37 {
            return None;
        }
        Some(AdvHeader {
            ty: AdvType::from_bits(b[0])?,
            ch_sel: b[0] & 0x20 != 0,
            tx_add: b[0] & 0x40 != 0,
            rx_add: b[0] & 0x80 != 0,
            length: b[1],
        })
    }
}

/// Maximum advertising/scan-response data of a legacy PDU (37 - 6 address bytes).
pub const MAX_ADV_DATA: usize = 31;

/// Build ADV_IND / ADV_SCAN_IND / ADV_NONCONN_IND / SCAN_RSP: header + AdvA + data. Returns the
/// number of bytes written to `out` (at most 39).
pub fn build_adv(out: &mut [u8; 39], ty: AdvType, ch_sel: bool, addr_random: bool, adv_a: &[u8; 6], data: &[u8]) -> usize {
    let data = &data[..data.len().min(MAX_ADV_DATA)];
    let h = AdvHeader { ty, ch_sel, tx_add: addr_random, rx_add: false, length: (6 + data.len()) as u8 };
    out[..2].copy_from_slice(&h.to_bytes());
    out[2..8].copy_from_slice(adv_a);
    out[8..8 + data.len()].copy_from_slice(data);
    8 + data.len()
}

/// The LLData of CONNECT_IND (§2.3.3.1) plus the two addresses.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ConnectInd {
    /// InitA (initiator address)
    pub init_a: [u8; 6],
    /// AdvA (our address, must match)
    pub adv_a: [u8; 6],
    /// Access address of the new connection
    pub access_address: u32,
    /// CRC preset
    pub crc_init: u32,
    /// transmitWindowSize in 1.25 ms units
    pub win_size: u8,
    /// transmitWindowOffset in 1.25 ms units
    pub win_offset: u16,
    /// connInterval in 1.25 ms units
    pub interval: u16,
    /// connPeripheralLatency
    pub latency: u16,
    /// connSupervisionTimeout in 10 ms units
    pub timeout: u16,
    /// Channel map
    pub chm: [u8; 5],
    /// hopIncrement (5..=16)
    pub hop: u8,
    /// Central sleep clock accuracy code (0..=7)
    pub sca: u8,
    /// TxAdd / RxAdd bits of the header
    pub tx_add: bool,
    /// RxAdd
    pub rx_add: bool,
    /// ChSel: the initiator supports algorithm #2
    pub ch_sel: bool,
}

fn le16(b: &[u8]) -> u16 {
    u16::from_le_bytes([b[0], b[1]])
}

impl ConnectInd {
    /// Payload length of CONNECT_IND (§2.3.3.1): 6 + 6 + 22.
    pub const LEN: u8 = 34;

    /// Parse a full PDU (header + payload).
    pub fn parse(pdu: &[u8]) -> Option<ConnectInd> {
        let h = AdvHeader::parse(pdu)?;
        if h.ty != AdvType::ConnectInd || h.length != Self::LEN || pdu.len() < 2 + Self::LEN as usize {
            return None;
        }
        let p = &pdu[2..];
        let ll = &p[12..];
        let c = ConnectInd {
            init_a: p[0..6].try_into().ok()?,
            adv_a: p[6..12].try_into().ok()?,
            access_address: u32::from_le_bytes(ll[0..4].try_into().ok()?),
            crc_init: u32::from_le_bytes([ll[4], ll[5], ll[6], 0]),
            win_size: ll[7],
            win_offset: le16(&ll[8..]),
            interval: le16(&ll[10..]),
            latency: le16(&ll[12..]),
            timeout: le16(&ll[14..]),
            chm: ll[16..21].try_into().ok()?,
            hop: ll[21] & 0x1F,
            sca: ll[21] >> 5,
            tx_add: h.tx_add,
            rx_add: h.rx_add,
            ch_sel: h.ch_sel,
        };
        Some(c)
    }

    /// Validity ranges of §2.3.3.1 / Vol 6 Part B §4.5.1.
    pub fn params_valid(&self) -> bool {
        (5..=16).contains(&self.hop)
            && (6..=3200).contains(&self.interval)
            && self.latency <= 499
            && (10..=3200).contains(&self.timeout)
            // supervisionTimeout must exceed (1 + latency) * interval * 2 (§4.5.2, in 1.25 ms units)
            && (self.timeout as u32) * 8 > (1 + self.latency as u32) * self.interval as u32 * 2
            && self.win_size >= 1
            && self.win_size as u16 <= (self.interval - 1).min(8)
            && self.win_offset <= self.interval
            && self.chm.iter().map(|b| b.count_ones()).sum::<u32>() >= 2
    }
}

/// LLID values of the data channel header (§2.4).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Llid {
    /// Continuation fragment of an L2CAP message, or an empty PDU
    Continue = 1,
    /// Start (or complete) L2CAP message
    Start = 2,
    /// LL Control PDU
    Control = 3,
}

/// Data channel PDU header (§2.4, 2 bytes, no CTE).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DataHeader {
    /// LLID (0 is reserved)
    pub llid: u8,
    /// Next expected sequence number
    pub nesn: bool,
    /// Sequence number
    pub sn: bool,
    /// More data
    pub md: bool,
    /// Payload length
    pub length: u8,
}

impl DataHeader {
    /// Encode.
    pub fn to_bytes(&self) -> [u8; 2] {
        [self.llid & 3 | (self.nesn as u8) << 2 | (self.sn as u8) << 3 | (self.md as u8) << 4, self.length]
    }

    /// Decode.
    pub fn parse(b: &[u8]) -> Option<DataHeader> {
        if b.len() < 2 {
            return None;
        }
        Some(DataHeader { llid: b[0] & 3, nesn: b[0] & 4 != 0, sn: b[0] & 8 != 0, md: b[0] & 0x10 != 0, length: b[1] })
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    pub fn sample_connect_ind() -> [u8; 36] {
        let mut p = [0u8; 36];
        p[0] = 0x05 | 0x20 | 0x40; // CONNECT_IND, ChSel, TxAdd
        p[1] = 34;
        p[2..8].copy_from_slice(&[1, 2, 3, 4, 5, 6]); // InitA
        p[8..14].copy_from_slice(&[0xC6, 0x6C, 0x2A, 0x4F, 0x31, 0x01]); // AdvA
        p[14..18].copy_from_slice(&0x8E89_BED6u32.to_le_bytes()); // AA
        p[18..21].copy_from_slice(&[0x56, 0x34, 0x12]); // CRCInit 0x123456
        p[21] = 3; // WinSize
        p[22..24].copy_from_slice(&2u16.to_le_bytes()); // WinOffset
        p[24..26].copy_from_slice(&24u16.to_le_bytes()); // interval 30 ms
        p[26..28].copy_from_slice(&0u16.to_le_bytes()); // latency
        p[28..30].copy_from_slice(&72u16.to_le_bytes()); // timeout 720 ms
        p[30..35].copy_from_slice(&[0xFF, 0xFF, 0xFF, 0xFF, 0x1F]);
        p[35] = 5 | (7 << 5); // hop 5, SCA 7
        p
    }

    #[test]
    fn adv_header_roundtrip() {
        let h = AdvHeader { ty: AdvType::AdvInd, ch_sel: true, tx_add: true, rx_add: false, length: 12 };
        assert_eq!(h.to_bytes(), [0x60, 12]);
        assert_eq!(AdvHeader::parse(&h.to_bytes()), Some(h));
        assert_eq!(AdvHeader::parse(&[0x07, 6]), None); // type 7 is reserved
        assert_eq!(AdvHeader::parse(&[0x00, 38]), None);
    }

    #[test]
    fn build_adv_ind() {
        let mut out = [0u8; 39];
        let n = build_adv(&mut out, AdvType::AdvInd, false, true, &[1, 2, 3, 4, 5, 0xC6], &[2, 1, 6]);
        assert_eq!(&out[..n], &[0x40, 9, 1, 2, 3, 4, 5, 0xC6, 2, 1, 6]);
    }

    #[test]
    fn connect_ind_fields() {
        let c = ConnectInd::parse(&sample_connect_ind()).unwrap();
        assert_eq!(c.access_address, 0x8E89_BED6);
        assert_eq!(c.crc_init, 0x12_3456);
        assert_eq!((c.win_size, c.win_offset, c.interval, c.latency, c.timeout), (3, 2, 24, 0, 72));
        assert_eq!((c.hop, c.sca, c.ch_sel, c.tx_add), (5, 7, true, true));
        assert!(c.params_valid());
        let mut bad = c;
        bad.hop = 4;
        assert!(!bad.params_valid());
        bad = c;
        bad.timeout = 4;
        assert!(!bad.params_valid());
        assert_eq!(ConnectInd::parse(&sample_connect_ind()[..30]), None);
    }

    #[test]
    fn data_header_bits() {
        let h = DataHeader { llid: 2, nesn: true, sn: false, md: true, length: 27 };
        assert_eq!(h.to_bytes(), [0x16, 27]);
        assert_eq!(DataHeader::parse(&[0x16, 27]), Some(h));
    }
}
