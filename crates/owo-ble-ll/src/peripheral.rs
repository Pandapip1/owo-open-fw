//! Peripheral-role link layer driver: legacy connectable advertising on 37/38/39, scan response,
//! CONNECT_IND handling, then connection events with LL control handling and a single-PDU TX
//! pipeline for L2CAP data. Single connection. No encryption, no PHY/length procedures yet
//! (see docs/radio-firmware-plan.md).

use crate::conn::{err, Connection, EventPlan, EVENT_JITTER_US};
use crate::llcp::{self, Ctrl};
use crate::pdu::{self, AdvHeader, AdvType, ConnectInd, DataHeader};
use crate::radio::{Radio, RadioEvent, RxPacket, T_IFS_US};

/// Advertising access address and CRC preset (§2.1.2, §3.1.1).
pub const ADV_ACCESS_ADDRESS: u32 = 0x8E89_BED6;
/// Largest data PDU payload without the Data Length Extension.
pub const MAX_PAYLOAD: usize = 27;
/// Guard between channel switches in an advertising event, µs.
const CH_SWITCH_GUARD_US: u32 = 400;
/// Width of the SCAN_REQ/CONNECT_IND listen window, µs (preamble + AA detect margin).
const REQ_WINDOW_US: u32 = 2 * EVENT_JITTER_US + 40;

/// Static configuration.
#[derive(Clone, Copy, Debug)]
pub struct Config {
    /// Device address
    pub addr: [u8; 6],
    /// `addr` is a random address
    pub addr_random: bool,
    /// Advertising interval, µs (min 20 ms for connectable, §4.4.2.2)
    pub adv_interval_us: u32,
    /// Advertising data
    pub adv_data: [u8; 31],
    /// Valid bytes in `adv_data`
    pub adv_len: usize,
    /// Scan response data
    pub scan_data: [u8; 31],
    /// Valid bytes in `scan_data`
    pub scan_len: usize,
    /// Our sleep clock accuracy, ppm (board property)
    pub local_ppm: u32,
    /// Company identifier for LL_VERSION_IND
    pub company: u16,
    /// Allow channel selection algorithm #2
    pub csa2: bool,
}

/// Something for the host side (HCI) to hear about.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Notify {
    /// A connection was established
    Connected {
        /// Peer address
        peer: [u8; 6],
        /// Peer address is random
        peer_random: bool,
        /// interval (1.25 ms)
        interval: u16,
        /// latency
        latency: u16,
        /// supervision timeout (10 ms)
        timeout: u16,
    },
    /// The connection ended
    Disconnected {
        /// HCI reason code
        reason: u8,
    },
    /// L2CAP data from the central
    Data {
        /// LLID 2 (start) or 1 (continuation)
        start: bool,
        /// Valid bytes
        len: u8,
        /// Payload
        data: [u8; MAX_PAYLOAD],
    },
}

#[derive(Clone, Copy, Debug)]
struct TxPdu {
    llid: u8,
    len: u8,
    data: [u8; MAX_PAYLOAD],
}

impl TxPdu {
    const EMPTY: TxPdu = TxPdu { llid: 1, len: 0, data: [0; MAX_PAYLOAD] };
    fn ctrl(p: &[u8]) -> TxPdu {
        let mut d = [0; MAX_PAYLOAD];
        d[..p.len()].copy_from_slice(p);
        TxPdu { llid: 3, len: p.len() as u8, data: d }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum AdvStage {
    Tx,
    WaitReq,
    SentScanRsp,
}

#[allow(clippy::large_enum_variant)]
enum State {
    Idle,
    Adv { idx: u8, stage: AdvStage, event_start_us: u32 },
    AdvGap { next_us: u32 },
    Conn(ConnState),
}

struct ConnState {
    conn: Connection,
    plan: EventPlan,
    got_first: bool,
    peer_md: bool,
    our_md: bool,
    awaiting_tx_done: bool,
    terminate_after_tx: Option<u8>,
    inflight: Option<TxPdu>,
    version_sent: bool,
}

/// `queue_data` refused: busy, too long or not connected.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct QueueError;

/// The driver.
pub struct Peripheral<R: Radio> {
    radio: R,
    cfg: Config,
    state: State,
    ctrl_q: Option<TxPdu>,
    data_q: Option<TxPdu>,
    notes: [Option<Notify>; 4],
    rand: u16,
}

impl<R: Radio> Peripheral<R> {
    /// Create (not advertising yet).
    pub fn new(radio: R, cfg: Config) -> Self {
        Peripheral { radio, cfg, state: State::Idle, ctrl_q: None, data_q: None, notes: [None; 4], rand: 0x5A5A }
    }

    /// Access the radio (tests, drivers).
    pub fn radio_mut(&mut self) -> &mut R {
        &mut self.radio
    }

    /// Provide entropy for the advertising delay (§4.4.2.2.1, 0..=10 ms).
    pub fn seed(&mut self, r: u16) {
        self.rand = r;
    }

    /// Start advertising now.
    pub fn start_advertising(&mut self) {
        if matches!(self.state, State::Idle) {
            let now = self.radio.now_us();
            self.state = State::Adv { idx: 0, stage: AdvStage::Tx, event_start_us: now };
            self.send_adv(0);
        }
    }

    /// Stop advertising (no effect while connected).
    pub fn stop_advertising(&mut self) {
        if matches!(self.state, State::Adv { .. } | State::AdvGap { .. }) {
            self.radio.stop();
            self.state = State::Idle;
        }
    }

    /// Is a connection up?
    pub fn is_connected(&self) -> bool {
        matches!(self.state, State::Conn(_))
    }

    /// Queue one L2CAP fragment (`start` = first fragment of an L2CAP PDU). Fails if the previous
    /// fragment has not gone out yet or `data` exceeds [`MAX_PAYLOAD`].
    pub fn queue_data(&mut self, start: bool, data: &[u8]) -> Result<(), QueueError> {
        if self.data_q.is_some() || data.len() > MAX_PAYLOAD || !self.is_connected() {
            return Err(QueueError);
        }
        let mut d = [0; MAX_PAYLOAD];
        d[..data.len()].copy_from_slice(data);
        self.data_q = Some(TxPdu { llid: if start { 2 } else { 1 }, len: data.len() as u8, data: d });
        Ok(())
    }

    /// Ask to disconnect: sends LL_TERMINATE_IND with `reason`.
    pub fn disconnect(&mut self, reason: u8) {
        if let State::Conn(c) = &mut self.state {
            c.terminate_after_tx = Some(reason);
            let (p, n) = llcp::terminate_ind(reason);
            self.ctrl_q = Some(TxPdu::ctrl(&p[..n]));
        }
    }

    /// Next notification for the host.
    pub fn take_notification(&mut self) -> Option<Notify> {
        self.notes.iter_mut().find_map(|n| n.take())
    }

    fn notify(&mut self, n: Notify) {
        if let Some(slot) = self.notes.iter_mut().find(|s| s.is_none()) {
            *slot = Some(n);
        }
    }

    /// The time the driver wants `poll` called by even without a radio event (advertising gap).
    pub fn next_deadline_us(&self) -> Option<u32> {
        match self.state {
            State::AdvGap { next_us } => Some(next_us),
            _ => None,
        }
    }

    fn adv_pdu(&self, ty: AdvType) -> ([u8; 39], usize) {
        let mut out = [0u8; 39];
        let (data, len) = if ty == AdvType::ScanRsp {
            (&self.cfg.scan_data, self.cfg.scan_len)
        } else {
            (&self.cfg.adv_data, self.cfg.adv_len)
        };
        let n = pdu::build_adv(&mut out, ty, self.cfg.csa2, self.cfg.addr_random, &self.cfg.addr, &data[..len]);
        (out, n)
    }

    fn send_adv(&mut self, idx: u8) {
        let (p, n) = self.adv_pdu(AdvType::AdvInd);
        self.radio.configure(37 + idx, ADV_ACCESS_ADDRESS, crate::crc::ADV_CRC_INIT);
        let at = self.radio.now_us().wrapping_add(CH_SWITCH_GUARD_US);
        self.radio.start_tx(at, &p[..n]);
    }

    /// Process radio events and timers. Call whenever the radio signals or the deadline passes.
    pub fn poll(&mut self) {
        if let State::AdvGap { next_us } = self.state {
            if (self.radio.now_us().wrapping_sub(next_us) as i32) >= 0 {
                self.state = State::Adv { idx: 0, stage: AdvStage::Tx, event_start_us: next_us };
                self.send_adv(0);
            }
        }
        while let Some(ev) = self.radio.poll() {
            match core::mem::replace(&mut self.state, State::Idle) {
                State::Adv { idx, stage, event_start_us } => self.adv_event(ev, idx, stage, event_start_us),
                State::Conn(c) => self.conn_event(ev, c),
                other => self.state = other,
            }
        }
    }

    fn next_adv_channel(&mut self, idx: u8, event_start_us: u32) {
        if idx < 2 {
            self.state = State::Adv { idx: idx + 1, stage: AdvStage::Tx, event_start_us };
            self.send_adv(idx + 1);
        } else {
            self.rand = self.rand.wrapping_mul(25173).wrapping_add(13849);
            let delay = (self.rand % 10_001) as u32;
            let next = event_start_us.wrapping_add(self.cfg.adv_interval_us).wrapping_add(delay);
            self.state = State::AdvGap { next_us: next };
        }
    }

    fn adv_event(&mut self, ev: RadioEvent, idx: u8, stage: AdvStage, start: u32) {
        match (stage, ev) {
            (AdvStage::Tx, RadioEvent::TxDone { end_us }) => {
                self.radio.configure(37 + idx, ADV_ACCESS_ADDRESS, crate::crc::ADV_CRC_INIT);
                self.radio.start_rx(end_us.wrapping_add(T_IFS_US).wrapping_sub(EVENT_JITTER_US), REQ_WINDOW_US);
                self.state = State::Adv { idx, stage: AdvStage::WaitReq, event_start_us: start };
            }
            (AdvStage::WaitReq, RadioEvent::Rx(p)) => self.adv_rx(p, idx, start),
            (AdvStage::WaitReq, RadioEvent::RxTimeout) => self.next_adv_channel(idx, start),
            (AdvStage::SentScanRsp, RadioEvent::TxDone { .. }) => self.next_adv_channel(idx, start),
            (stage, _) => self.state = State::Adv { idx, stage, event_start_us: start },
        }
    }

    fn adv_rx(&mut self, p: RxPacket, idx: u8, start: u32) {
        let hdr = AdvHeader::parse(p.pdu());
        let mine = |b: &[u8]| b.len() >= 6 && b[..6] == self.cfg.addr;
        match hdr {
            Some(h) if p.crc_ok && h.ty == AdvType::ScanReq && h.length == 12 && p.len >= 14 && mine(&p.pdu()[8..14]) => {
                let (r, n) = self.adv_pdu(AdvType::ScanRsp);
                self.radio.tx_after_rx(&r[..n]);
                self.state = State::Adv { idx, stage: AdvStage::SentScanRsp, event_start_us: start };
            }
            Some(h) if p.crc_ok && h.ty == AdvType::ConnectInd => {
                match ConnectInd::parse(p.pdu()) {
                    Some(c) if mine(&c.adv_a) && c.params_valid() => self.connect(&c, p.end_us),
                    _ => self.next_adv_channel(idx, start),
                }
            }
            _ => self.next_adv_channel(idx, start),
        }
    }

    fn connect(&mut self, c: &ConnectInd, rx_end_us: u32) {
        let (conn, plan) = Connection::from_connect_ind(c, rx_end_us, self.cfg.local_ppm, self.cfg.csa2 && c.ch_sel);
        self.notify(Notify::Connected {
            peer: c.init_a,
            peer_random: c.tx_add,
            interval: c.interval,
            latency: c.latency,
            timeout: c.timeout,
        });
        self.radio.configure(plan.channel, conn.access_address, conn.crc_init);
        self.radio.start_rx(plan.listen_from_us, plan.window_us);
        self.state = State::Conn(ConnState {
            conn,
            plan,
            got_first: false,
            peer_md: false,
            our_md: false,
            awaiting_tx_done: false,
            terminate_after_tx: None,
            inflight: None,
            version_sent: false,
        });
    }

    fn end_connection(&mut self, reason: u8) {
        self.radio.stop();
        self.ctrl_q = None;
        self.data_q = None;
        self.notify(Notify::Disconnected { reason });
        self.state = State::Idle;
    }

    /// Close the current connection event and arm the next one.
    fn next_event(&mut self, mut c: ConnState) {
        let now = self.radio.now_us();
        if c.conn.supervision_expired(now) {
            let reason = if c.conn.is_established() { err::CONN_TIMEOUT } else { err::FAILED_TO_ESTABLISH };
            return self.end_connection(reason);
        }
        c.plan = c.conn.plan_next();
        c.got_first = false;
        c.peer_md = false;
        c.our_md = false;
        c.awaiting_tx_done = false;
        self.radio.configure(c.plan.channel, c.conn.access_address, c.conn.crc_init);
        self.radio.start_rx(c.plan.listen_from_us, c.plan.window_us);
        self.state = State::Conn(c);
    }

    fn handle_ctrl(&mut self, c: &mut ConnState, payload: &[u8]) {
        let reply = |s: &mut Self, p: &[u8]| s.ctrl_q = Some(TxPdu::ctrl(p));
        match llcp::parse(payload) {
            Some(Ctrl::FeatureReq(_)) => {
                let (p, n) = llcp::feature_rsp();
                reply(self, &p[..n]);
            }
            Some(Ctrl::VersionInd { .. }) => {
                if !c.version_sent {
                    c.version_sent = true;
                    let (p, n) = llcp::version_ind(self.cfg.company, 0);
                    reply(self, &p[..n]);
                }
            }
            Some(Ctrl::PingReq) => {
                let (p, n) = llcp::ping_rsp();
                reply(self, &p[..n]);
            }
            Some(Ctrl::Terminate(code)) => c.terminate_after_tx = Some(code),
            Some(u @ (Ctrl::ConnUpdate { .. } | Ctrl::ChanMap { .. })) => {
                if let Err(code) = c.conn.on_ctrl(&u) {
                    let (p, n) = llcp::terminate_ind(code);
                    reply(self, &p[..n]);
                    c.terminate_after_tx = Some(code);
                }
            }
            Some(Ctrl::LengthReq) | Some(Ctrl::Other(_)) | None => {
                let op = payload.first().copied().unwrap_or(0);
                let (p, n) = llcp::unknown_rsp(op);
                reply(self, &p[..n]);
            }
        }
    }

    fn conn_event(&mut self, ev: RadioEvent, mut c: ConnState) {
        match ev {
            RadioEvent::RxTimeout => {
                if c.got_first {
                    self.next_event(c); // event over (peer did not continue)
                } else {
                    self.next_event(c); // missed event: widening grows from the old anchor
                }
            }
            RadioEvent::Rx(p) => {
                let hdr = DataHeader::parse(p.pdu());
                let ok = p.crc_ok && matches!(hdr, Some(h) if h.length as usize + 2 == p.len && h.length as usize <= MAX_PAYLOAD && h.llid != 0);
                if !ok {
                    // §4.5.? a peripheral does not answer a packet with a bad CRC: the event ends.
                    return self.next_event(c);
                }
                let h = hdr.unwrap();
                if !c.got_first {
                    c.got_first = true;
                    c.conn.set_anchor(c.plan.counter, p.start_us);
                }
                let out = c.conn.on_header(h.sn, h.nesn);
                if out.acked {
                    c.inflight = None;
                }
                if out.new_data && h.length > 0 {
                    let payload = &p.pdu()[2..];
                    if h.llid == 3 {
                        self.handle_ctrl(&mut c, payload);
                    } else {
                        let mut d = [0; MAX_PAYLOAD];
                        d[..payload.len()].copy_from_slice(payload);
                        self.notify(Notify::Data { start: h.llid == 2, len: h.length, data: d });
                    }
                }
                if c.inflight.is_none() {
                    c.inflight = Some(self.ctrl_q.take().or_else(|| self.data_q.take()).unwrap_or(TxPdu::EMPTY));
                }
                let tx = c.inflight.unwrap();
                c.our_md = self.ctrl_q.is_some() || self.data_q.is_some();
                c.peer_md = h.md;
                let hdr = DataHeader { llid: tx.llid, nesn: c.conn.nesn(), sn: c.conn.sn(), md: c.our_md, length: tx.len };
                let mut buf = [0u8; 2 + MAX_PAYLOAD];
                buf[..2].copy_from_slice(&hdr.to_bytes());
                buf[2..2 + tx.len as usize].copy_from_slice(&tx.data[..tx.len as usize]);
                self.radio.tx_after_rx(&buf[..2 + tx.len as usize]);
                c.awaiting_tx_done = true;
                self.state = State::Conn(c);
            }
            RadioEvent::TxDone { end_us } => {
                c.awaiting_tx_done = false;
                if let Some(reason) = c.terminate_after_tx {
                    return self.end_connection(reason);
                }
                if c.peer_md || c.our_md {
                    self.radio.configure(c.plan.channel, c.conn.access_address, c.conn.crc_init);
                    self.radio.start_rx(end_us.wrapping_add(T_IFS_US).wrapping_sub(EVENT_JITTER_US), 2 * EVENT_JITTER_US + 40);
                    self.state = State::Conn(c);
                } else {
                    self.next_event(c);
                }
            }
        }
    }
}
