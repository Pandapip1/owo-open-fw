//! The only interface between the link layer and a radio. Implemented by the EFR32BG22 radio
//! driver, by the QEMU radio model's test harness and by host test doubles.
//!
//! Timestamps are microseconds of a free-running, wrapping 32-bit timebase. A packet timestamp is
//! the instant its access address *starts* on air (what BLE calls the anchor for a central's
//! packet). Packet buffers contain the PDU (header + payload) only: preamble, access address,
//! CRC and whitening belong to the radio.

/// Largest PDU (2-byte header + 255 payload).
pub const MAX_PDU: usize = 257;

/// A received packet.
#[derive(Clone, Debug)]
pub struct RxPacket {
    /// PDU bytes (header + payload), `len` valid
    pub buf: [u8; MAX_PDU],
    /// Valid length of `buf`
    pub len: usize,
    /// CRC passed
    pub crc_ok: bool,
    /// Start of the access address on air
    pub start_us: u32,
    /// End of the last bit (CRC) on air
    pub end_us: u32,
    /// Signal strength in dBm
    pub rssi: i8,
}

impl RxPacket {
    /// The PDU bytes.
    pub fn pdu(&self) -> &[u8] {
        &self.buf[..self.len]
    }
}

/// What the radio reports.
#[allow(clippy::large_enum_variant)] // no allocation: the packet buffer is held by value
#[derive(Clone, Debug)]
pub enum RadioEvent {
    /// A packet arrived (check `crc_ok`)
    Rx(RxPacket),
    /// The receive window closed without an access address match
    RxTimeout,
    /// A transmission finished; `end_us` is when its last bit left the antenna
    TxDone {
        /// End of the packet
        end_us: u32,
    },
}

/// Radio commands. All times are absolute in the radio timebase.
pub trait Radio {
    /// Current time.
    fn now_us(&self) -> u32;
    /// Select channel index (0..=39), access address and CRC preset for the next operation.
    fn configure(&mut self, channel_index: u8, access_address: u32, crc_init: u32);
    /// Listen from `at_us` for `window_us` (the window covers the time an access address may
    /// begin).
    fn start_rx(&mut self, at_us: u32, window_us: u32);
    /// Transmit `pdu` starting at `at_us`.
    fn start_tx(&mut self, at_us: u32, pdu: &[u8]);
    /// Transmit `pdu` exactly T_IFS (150 µs, Vol 6 Part B §4.1.1) after the end of the packet
    /// just received. Must be issued from the receive event handler; the implementation must
    /// either meet the deadline or report `TxDone` never (the LL then treats the event as lost).
    fn tx_after_rx(&mut self, pdu: &[u8]);
    /// Next pending event, if any.
    fn poll(&mut self) -> Option<RadioEvent>;
    /// Abort any operation in progress.
    fn stop(&mut self);
}

/// Inter-frame space (Vol 6 Part B §4.1.1).
pub const T_IFS_US: u32 = 150;
