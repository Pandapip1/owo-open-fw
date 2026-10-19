//! HCI framing (Vol 4 Part A UART transport indicators, Vol 4 Part E §5.4 packets) and the event
//! builders the host needs from the controller, plus L2CAP→LL fragmentation for ACL data.
//!
//! Constants cross-checked against zephyr/include/zephyr/bluetooth/hci_types.h. Only the packet
//! mechanics live here; the command set Zephyr's host requires at init is listed in
//! docs/radio-firmware-plan.md.

/// H:4 packet indicators (Vol 4 Part A §2).
pub mod h4 {
    /// Command
    pub const CMD: u8 = 0x01;
    /// ACL data
    pub const ACL: u8 = 0x02;
    /// Event
    pub const EVT: u8 = 0x04;
}

/// Event codes.
pub mod evt {
    /// Disconnection Complete
    pub const DISCONN_COMPLETE: u8 = 0x05;
    /// Command Complete
    pub const CMD_COMPLETE: u8 = 0x0E;
    /// Command Status
    pub const CMD_STATUS: u8 = 0x0F;
    /// Number Of Completed Packets
    pub const NUM_COMPLETED_PACKETS: u8 = 0x13;
    /// LE Meta
    pub const LE_META: u8 = 0x3E;
    /// LE Connection Complete subevent
    pub const LE_CONN_COMPLETE: u8 = 0x01;
}

/// Opcode from OGF/OCF (Vol 4 Part E §5.4.1).
pub const fn opcode(ogf: u8, ocf: u16) -> u16 {
    ocf | ((ogf as u16) << 10)
}

/// Opcodes the minimal advertising/connection path needs.
pub mod cmd {
    use super::opcode;
    /// HCI_Reset
    pub const RESET: u16 = opcode(0x03, 0x0003);
    /// HCI_Read_Local_Version_Information
    pub const READ_LOCAL_VERSION: u16 = opcode(0x04, 0x0001);
    /// HCI_Read_BD_ADDR
    pub const READ_BD_ADDR: u16 = opcode(0x04, 0x0009);
    /// HCI_LE_Set_Advertising_Parameters
    pub const LE_SET_ADV_PARAM: u16 = opcode(0x08, 0x0006);
    /// HCI_LE_Set_Advertising_Data
    pub const LE_SET_ADV_DATA: u16 = opcode(0x08, 0x0008);
    /// HCI_LE_Set_Scan_Response_Data
    pub const LE_SET_SCAN_RSP_DATA: u16 = opcode(0x08, 0x0009);
    /// HCI_LE_Set_Advertising_Enable
    pub const LE_SET_ADV_ENABLE: u16 = opcode(0x08, 0x000A);
    /// HCI_Disconnect
    pub const DISCONNECT: u16 = opcode(0x01, 0x0006);
}

/// Parsed command header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CommandHeader {
    /// Opcode
    pub opcode: u16,
    /// Parameter length
    pub len: u8,
}

/// Parse an HCI command packet without the H:4 indicator.
pub fn parse_command(p: &[u8]) -> Option<(CommandHeader, &[u8])> {
    if p.len() < 3 || p.len() < 3 + p[2] as usize {
        return None;
    }
    Some((CommandHeader { opcode: u16::from_le_bytes([p[0], p[1]]), len: p[2] }, &p[3..3 + p[2] as usize]))
}

/// Write an event packet (without H:4 indicator) into `out`; returns its length.
pub fn event(out: &mut [u8], code: u8, params: &[u8]) -> usize {
    out[0] = code;
    out[1] = params.len() as u8;
    out[2..2 + params.len()].copy_from_slice(params);
    2 + params.len()
}

/// Command Complete (Vol 4 Part E §7.7.14) with the given return parameters (status first).
pub fn command_complete(out: &mut [u8], opcode: u16, ret: &[u8]) -> usize {
    let mut p = [0u8; 40];
    p[0] = 1; // Num_HCI_Command_Packets
    p[1..3].copy_from_slice(&opcode.to_le_bytes());
    p[3..3 + ret.len()].copy_from_slice(ret);
    event(out, evt::CMD_COMPLETE, &p[..3 + ret.len()])
}

/// Command Status (§7.7.15).
pub fn command_status(out: &mut [u8], opcode: u16, status: u8) -> usize {
    let mut p = [0u8; 4];
    p[0] = status;
    p[1] = 1;
    p[2..4].copy_from_slice(&opcode.to_le_bytes());
    event(out, evt::CMD_STATUS, &p)
}

/// Disconnection Complete (§7.7.5).
pub fn disconnection_complete(out: &mut [u8], handle: u16, reason: u8) -> usize {
    let mut p = [0u8; 4];
    p[0] = 0;
    p[1..3].copy_from_slice(&handle.to_le_bytes());
    p[3] = reason;
    event(out, evt::DISCONN_COMPLETE, &p)
}

/// LE Connection Complete (§7.7.65.1), role peripheral (1).
#[allow(clippy::too_many_arguments)]
pub fn le_connection_complete(
    out: &mut [u8],
    handle: u16,
    peer_random: bool,
    peer: &[u8; 6],
    interval: u16,
    latency: u16,
    timeout: u16,
    central_sca: u8,
) -> usize {
    let mut p = [0u8; 19];
    p[0] = evt::LE_CONN_COMPLETE;
    p[1] = 0; // status
    p[2..4].copy_from_slice(&handle.to_le_bytes());
    p[4] = 1; // role: peripheral
    p[5] = peer_random as u8;
    p[6..12].copy_from_slice(peer);
    p[12..14].copy_from_slice(&interval.to_le_bytes());
    p[14..16].copy_from_slice(&latency.to_le_bytes());
    p[16..18].copy_from_slice(&timeout.to_le_bytes());
    p[18] = central_sca;
    event(out, evt::LE_META, &p)
}

/// Number Of Completed Packets for one handle (§7.7.19).
pub fn number_of_completed_packets(out: &mut [u8], handle: u16, count: u16) -> usize {
    let mut p = [0u8; 5];
    p[0] = 1;
    p[1..3].copy_from_slice(&handle.to_le_bytes());
    p[3..5].copy_from_slice(&count.to_le_bytes());
    event(out, evt::NUM_COMPLETED_PACKETS, &p)
}

/// ACL header (Vol 4 Part E §5.4.2): handle (12 bits), PB flag, BC flag, length.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AclHeader {
    /// Connection handle
    pub handle: u16,
    /// Packet boundary flag: 0b00 first non-flushable... Controller-to-host: 0b10 = first
    /// automatically flushable, 0b01 = continuing fragment. Host-to-controller: 0b00 first
    /// non-flushable, 0b01 continuing.
    pub pb: u8,
    /// Data length
    pub len: u16,
}

/// Parse an ACL packet (no H:4 indicator).
pub fn parse_acl(p: &[u8]) -> Option<(AclHeader, &[u8])> {
    if p.len() < 4 {
        return None;
    }
    let h = u16::from_le_bytes([p[0], p[1]]);
    let len = u16::from_le_bytes([p[2], p[3]]);
    if p.len() < 4 + len as usize {
        return None;
    }
    Some((AclHeader { handle: h & 0x0FFF, pb: ((h >> 12) & 3) as u8, len }, &p[4..4 + len as usize]))
}

/// Build an ACL packet header + payload into `out`; returns length.
pub fn build_acl(out: &mut [u8], handle: u16, pb: u8, payload: &[u8]) -> usize {
    let h = (handle & 0x0FFF) | ((pb as u16 & 3) << 12);
    out[..2].copy_from_slice(&h.to_le_bytes());
    out[2..4].copy_from_slice(&(payload.len() as u16).to_le_bytes());
    out[4..4 + payload.len()].copy_from_slice(payload);
    4 + payload.len()
}

/// Split an L2CAP PDU into LL data PDU fragments of at most `max` bytes. Calls `f(start, chunk)`
/// for each fragment (LLID 0b10 for the first, 0b01 for the rest, Vol 6 Part B §2.4.2).
pub fn fragment(l2cap: &[u8], max: usize, mut f: impl FnMut(bool, &[u8])) {
    let mut first = true;
    for chunk in l2cap.chunks(max) {
        f(first, chunk);
        first = false;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn opcodes_match_zephyr_hci_types() {
        // hci_types.h comments: RESET 0x0c03, READ_BD_ADDR 0x1009, LE_SET_ADV_PARAM 0x2006, ...
        assert_eq!(cmd::RESET, 0x0C03);
        assert_eq!(cmd::READ_LOCAL_VERSION, 0x1001);
        assert_eq!(cmd::READ_BD_ADDR, 0x1009);
        assert_eq!(cmd::LE_SET_ADV_PARAM, 0x2006);
        assert_eq!(cmd::LE_SET_ADV_DATA, 0x2008);
        assert_eq!(cmd::LE_SET_SCAN_RSP_DATA, 0x2009);
        assert_eq!(cmd::LE_SET_ADV_ENABLE, 0x200A);
        assert_eq!(cmd::DISCONNECT, 0x0406);
    }

    #[test]
    fn command_roundtrip_and_events() {
        let pkt = [0x03, 0x0C, 0x00];
        assert_eq!(parse_command(&pkt), Some((CommandHeader { opcode: cmd::RESET, len: 0 }, &[][..])));
        assert_eq!(parse_command(&[0x06, 0x20, 0x05, 1]), None);
        let mut o = [0u8; 64];
        let n = command_complete(&mut o, cmd::RESET, &[0]);
        assert_eq!(&o[..n], &[0x0E, 4, 1, 0x03, 0x0C, 0]);
        let n = command_status(&mut o, cmd::DISCONNECT, 0);
        assert_eq!(&o[..n], &[0x0F, 4, 0, 1, 0x06, 0x04]);
        let n = disconnection_complete(&mut o, 0x40, 0x13);
        assert_eq!(&o[..n], &[0x05, 4, 0, 0x40, 0, 0x13]);
        let n = number_of_completed_packets(&mut o, 0x40, 2);
        assert_eq!(&o[..n], &[0x13, 5, 1, 0x40, 0, 2, 0]);
        let n = le_connection_complete(&mut o, 0x40, false, &[1, 2, 3, 4, 5, 6], 24, 0, 72, 7);
        assert_eq!(o[0], 0x3E);
        assert_eq!(o[1] as usize, n - 2);
        assert_eq!(&o[2..9], &[1, 0, 0x40, 0, 1, 0, 1]);
    }

    #[test]
    fn acl_header_and_fragmentation() {
        let mut o = [0u8; 40];
        let n = build_acl(&mut o, 0x40, 2, &[9, 8, 7]);
        assert_eq!(&o[..n], &[0x40, 0x20, 3, 0, 9, 8, 7]);
        assert_eq!(parse_acl(&o[..n]), Some((AclHeader { handle: 0x40, pb: 2, len: 3 }, &[9u8, 8, 7][..])));
        let data: [u8; 60] = core::array::from_fn(|i| i as u8);
        let mut frags = [(false, 0usize); 4];
        let mut k = 0;
        fragment(&data, 27, |s, c| {
            frags[k] = (s, c.len());
            k += 1;
        });
        assert_eq!(&frags[..k], &[(true, 27), (false, 27), (false, 6)]);
    }
}
