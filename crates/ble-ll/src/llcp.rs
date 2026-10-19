//! LL Control PDUs (§2.4.2): parsing and the responses a minimal peripheral needs.
//!
//! Opcode numbers follow Vol 6 Part B §2.4.2 as listed in zephyr/subsys/bluetooth/controller/
//! ll_sw/pdu.h (enum PDU_DATA_LLCTRL_TYPE_*).

/// Opcodes.
pub mod op {
    /// LL_CONNECTION_UPDATE_IND
    pub const CONN_UPDATE_IND: u8 = 0x00;
    /// LL_CHANNEL_MAP_IND
    pub const CHAN_MAP_IND: u8 = 0x01;
    /// LL_TERMINATE_IND
    pub const TERMINATE_IND: u8 = 0x02;
    /// LL_ENC_REQ
    pub const ENC_REQ: u8 = 0x03;
    /// LL_UNKNOWN_RSP
    pub const UNKNOWN_RSP: u8 = 0x07;
    /// LL_FEATURE_REQ
    pub const FEATURE_REQ: u8 = 0x08;
    /// LL_FEATURE_RSP
    pub const FEATURE_RSP: u8 = 0x09;
    /// LL_VERSION_IND
    pub const VERSION_IND: u8 = 0x0C;
    /// LL_PERIPHERAL_FEATURE_REQ (LL_SLAVE_FEATURE_REQ)
    pub const PERIPH_FEATURE_REQ: u8 = 0x0E;
    /// LL_CONNECTION_PARAM_REQ
    pub const CONN_PARAM_REQ: u8 = 0x0F;
    /// LL_REJECT_EXT_IND
    pub const REJECT_EXT_IND: u8 = 0x11;
    /// LL_PING_REQ
    pub const PING_REQ: u8 = 0x12;
    /// LL_PING_RSP
    pub const PING_RSP: u8 = 0x13;
    /// LL_LENGTH_REQ
    pub const LENGTH_REQ: u8 = 0x14;
    /// LL_LENGTH_RSP
    pub const LENGTH_RSP: u8 = 0x15;
    /// LL_PHY_REQ
    pub const PHY_REQ: u8 = 0x16;
}

/// Feature bits advertised in FEATURE_RSP (§4.6). Only what this crate implements:
/// bit 2 = Extended Reject Indication, bit 3 = Peripheral-initiated Features Exchange.
pub const FEATURES: [u8; 8] = [0b0000_1100, 0, 0, 0, 0, 0, 0, 0];

/// Link layer version number advertised in LL_VERSION_IND (Bluetooth 5.2 = 0x0B).
pub const LL_VERSION: u8 = 0x0B;

/// Parsed control PDU (payload after the 2-byte data header).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Ctrl {
    /// New connection parameters at `instant`
    ConnUpdate {
        /// transmitWindowSize (1.25 ms)
        win_size: u8,
        /// transmitWindowOffset (1.25 ms)
        win_offset: u16,
        /// connInterval (1.25 ms)
        interval: u16,
        /// peripheral latency
        latency: u16,
        /// supervision timeout (10 ms)
        timeout: u16,
        /// connEventCounter at which the update applies
        instant: u16,
    },
    /// New channel map at `instant`
    ChanMap {
        /// ChM
        map: [u8; 5],
        /// instant
        instant: u16,
    },
    /// Terminate with error code
    Terminate(u8),
    /// Feature exchange request (either direction)
    FeatureReq([u8; 8]),
    /// Version indication
    VersionInd {
        /// LL version
        version: u8,
        /// company id
        company: u16,
        /// sub-version
        subversion: u16,
    },
    /// LL_PING_REQ
    PingReq,
    /// LL_LENGTH_REQ (octets/time values, ignored: we stay at 27/328)
    LengthReq,
    /// Anything else; the opcode is kept so UNKNOWN_RSP can be built
    Other(u8),
}

fn le16(b: &[u8]) -> u16 {
    u16::from_le_bytes([b[0], b[1]])
}

/// Parse a control PDU payload. `None` if it is too short for its opcode.
pub fn parse(p: &[u8]) -> Option<Ctrl> {
    let (&opcode, b) = p.split_first()?;
    Some(match opcode {
        op::CONN_UPDATE_IND if b.len() >= 11 => Ctrl::ConnUpdate {
            win_size: b[0],
            win_offset: le16(&b[1..]),
            interval: le16(&b[3..]),
            latency: le16(&b[5..]),
            timeout: le16(&b[7..]),
            instant: le16(&b[9..]),
        },
        op::CHAN_MAP_IND if b.len() >= 7 => Ctrl::ChanMap { map: b[..5].try_into().ok()?, instant: le16(&b[5..]) },
        op::TERMINATE_IND if !b.is_empty() => Ctrl::Terminate(b[0]),
        op::FEATURE_REQ | op::PERIPH_FEATURE_REQ if b.len() >= 8 => Ctrl::FeatureReq(b[..8].try_into().ok()?),
        op::VERSION_IND if b.len() >= 5 => Ctrl::VersionInd { version: b[0], company: le16(&b[1..]), subversion: le16(&b[3..]) },
        op::PING_REQ => Ctrl::PingReq,
        op::LENGTH_REQ if b.len() >= 8 => Ctrl::LengthReq,
        op::CONN_UPDATE_IND | op::CHAN_MAP_IND | op::TERMINATE_IND | op::FEATURE_REQ | op::PERIPH_FEATURE_REQ
        | op::VERSION_IND | op::LENGTH_REQ => return None,
        o => Ctrl::Other(o),
    })
}

/// LL_UNKNOWN_RSP for `unknown`.
pub fn unknown_rsp(unknown: u8) -> ([u8; 2], usize) {
    ([op::UNKNOWN_RSP, unknown], 2)
}

/// LL_FEATURE_RSP.
pub fn feature_rsp() -> ([u8; 9], usize) {
    let mut p = [0u8; 9];
    p[0] = op::FEATURE_RSP;
    p[1..].copy_from_slice(&FEATURES);
    (p, 9)
}

/// LL_VERSION_IND with the given company identifier and sub-version.
pub fn version_ind(company: u16, subversion: u16) -> ([u8; 6], usize) {
    let mut p = [0u8; 6];
    p[0] = op::VERSION_IND;
    p[1] = LL_VERSION;
    p[2..4].copy_from_slice(&company.to_le_bytes());
    p[4..6].copy_from_slice(&subversion.to_le_bytes());
    (p, 6)
}

/// LL_REJECT_EXT_IND.
pub fn reject_ext(rejected_opcode: u8, error: u8) -> ([u8; 3], usize) {
    ([op::REJECT_EXT_IND, rejected_opcode, error], 3)
}

/// LL_PING_RSP.
pub fn ping_rsp() -> ([u8; 1], usize) {
    ([op::PING_RSP], 1)
}

/// LL_TERMINATE_IND.
pub fn terminate_ind(error: u8) -> ([u8; 2], usize) {
    ([op::TERMINATE_IND, error], 2)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn opcodes_match_the_numbers_in_the_spec_table() {
        // Vol 6 Part B §2.4.2 Table 2.18 (as enumerated in zephyr ll_sw/pdu.h).
        assert_eq!((op::CONN_UPDATE_IND, op::CHAN_MAP_IND, op::TERMINATE_IND), (0, 1, 2));
        assert_eq!((op::UNKNOWN_RSP, op::FEATURE_REQ, op::FEATURE_RSP, op::VERSION_IND), (7, 8, 9, 0x0C));
        assert_eq!((op::CONN_PARAM_REQ, op::REJECT_EXT_IND, op::PING_REQ, op::LENGTH_REQ), (0x0F, 0x11, 0x12, 0x14));
    }

    #[test]
    fn parse_conn_update() {
        let p = [0, 2, 3, 0, 24, 0, 6, 0, 72, 0, 0x10, 0x00];
        assert_eq!(
            parse(&p),
            Some(Ctrl::ConnUpdate { win_size: 2, win_offset: 3, interval: 24, latency: 6, timeout: 72, instant: 0x10 })
        );
        assert_eq!(parse(&p[..11]), None);
    }

    #[test]
    fn parse_others() {
        assert_eq!(parse(&[1, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 5, 0]), Some(Ctrl::ChanMap { map: [0xFF, 0xFF, 0xFF, 0xFF, 0x1F], instant: 5 }));
        assert_eq!(parse(&[2, 0x13]), Some(Ctrl::Terminate(0x13)));
        assert_eq!(parse(&[0x12]), Some(Ctrl::PingReq));
        assert_eq!(parse(&[0x1A]), Some(Ctrl::Other(0x1A)));
        assert_eq!(parse(&[]), None);
        assert_eq!(unknown_rsp(0x1A).0, [7, 0x1A]);
        assert_eq!(feature_rsp().0[0], 9);
        assert_eq!(&version_ind(0xFFFF, 1).0, &[0x0C, 0x0B, 0xFF, 0xFF, 1, 0]);
    }
}
