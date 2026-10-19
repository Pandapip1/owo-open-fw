//! End-to-end behaviour of the peripheral driver against a scripted central and a mock radio.
//! Timing numbers are checked against the formulas of Vol 6 Part B §4.2.2 / §4.5 (see conn.rs).

use ble_ll::conn::{EVENT_JITTER_US, UNIT_US};
use ble_ll::pdu::{AdvHeader, AdvType, DataHeader};
use ble_ll::peripheral::{Config, Notify, Peripheral, ADV_ACCESS_ADDRESS};
use ble_ll::radio::{Radio, RadioEvent, RxPacket, MAX_PDU, T_IFS_US};
use std::collections::VecDeque;

#[derive(Debug, Clone, PartialEq)]
enum Cmd {
    Configure { ch: u8, aa: u32, crc: u32 },
    Rx { at: u32, window: u32 },
    Tx { at: u32, pdu: Vec<u8> },
    TxAfterRx(Vec<u8>),
    Stop,
}

#[derive(Default)]
struct Mock {
    now: u32,
    cmds: Vec<Cmd>,
    events: VecDeque<RadioEvent>,
}

impl Radio for Mock {
    fn now_us(&self) -> u32 {
        self.now
    }
    fn configure(&mut self, ch: u8, aa: u32, crc: u32) {
        self.cmds.push(Cmd::Configure { ch, aa, crc });
    }
    fn start_rx(&mut self, at: u32, window: u32) {
        self.cmds.push(Cmd::Rx { at, window });
    }
    fn start_tx(&mut self, at: u32, pdu: &[u8]) {
        self.cmds.push(Cmd::Tx { at, pdu: pdu.to_vec() });
    }
    fn tx_after_rx(&mut self, pdu: &[u8]) {
        self.cmds.push(Cmd::TxAfterRx(pdu.to_vec()));
    }
    fn poll(&mut self) -> Option<RadioEvent> {
        self.events.pop_front()
    }
    fn stop(&mut self) {
        self.cmds.push(Cmd::Stop);
    }
}

const ADDR: [u8; 6] = [0xC6, 0x6C, 0x2A, 0x4F, 0x31, 0x01];

fn cfg() -> Config {
    let mut adv = [0u8; 31];
    adv[..3].copy_from_slice(&[2, 1, 6]);
    let mut scan = [0u8; 31];
    scan[..5].copy_from_slice(&[4, 9, b'O', b'W', b'O']);
    Config {
        addr: ADDR,
        addr_random: true,
        adv_interval_us: 100_000,
        adv_data: adv,
        adv_len: 3,
        scan_data: scan,
        scan_len: 5,
        local_ppm: 50,
        company: 0xFFFF,
        csa2: false,
    }
}

fn rx(pdu: &[u8], crc_ok: bool, start: u32) -> RadioEvent {
    let mut buf = [0u8; MAX_PDU];
    buf[..pdu.len()].copy_from_slice(pdu);
    RadioEvent::Rx(RxPacket { buf, len: pdu.len(), crc_ok, start_us: start, end_us: start + 40 + 8 * pdu.len() as u32, rssi: -50 })
}

fn connect_ind(interval: u16, win_off: u16, latency: u16) -> Vec<u8> {
    let mut p = vec![0u8; 36];
    p[0] = 0x05;
    p[1] = 34;
    p[2..8].copy_from_slice(&[1, 2, 3, 4, 5, 6]);
    p[8..14].copy_from_slice(&ADDR);
    p[14..18].copy_from_slice(&0x5065_4321u32.to_le_bytes());
    p[18..21].copy_from_slice(&[0x56, 0x34, 0x12]);
    p[21] = 2;
    p[22..24].copy_from_slice(&win_off.to_le_bytes());
    p[24..26].copy_from_slice(&interval.to_le_bytes());
    p[26..28].copy_from_slice(&latency.to_le_bytes());
    p[28..30].copy_from_slice(&200u16.to_le_bytes());
    p[30..35].copy_from_slice(&[0xFF, 0xFF, 0xFF, 0xFF, 0x1F]);
    p[35] = 7 | (1 << 5); // hop 7, SCA 1 (250 ppm)
    p
}

fn drain(p: &mut Peripheral<Mock>) -> Vec<Cmd> {
    p.poll();
    std::mem::take(&mut p.radio_mut().cmds)
}

fn push(p: &mut Peripheral<Mock>, ev: RadioEvent) {
    // the mock clock follows the event time
    let t = match &ev {
        RadioEvent::TxDone { end_us } => *end_us,
        RadioEvent::Rx(r) => r.end_us,
        RadioEvent::RxTimeout => p.radio_mut().now,
    };
    p.radio_mut().now = t;
    p.radio_mut().events.push_back(ev);
}

fn start_connection(p: &mut Peripheral<Mock>) -> u32 {
    p.start_advertising();
    let c = drain(p);
    // ADV_IND on channel 37, 400 µs guard after "now"
    assert_eq!(c[0], Cmd::Configure { ch: 37, aa: ADV_ACCESS_ADDRESS, crc: 0x55_5555 });
    let Cmd::Tx { pdu, .. } = &c[1] else { panic!("{c:?}") };
    let h = AdvHeader::parse(pdu).unwrap();
    assert_eq!((h.ty, h.length, h.tx_add), (AdvType::AdvInd, 9, true));
    assert_eq!(&pdu[2..8], &ADDR);
    push(p, RadioEvent::TxDone { end_us: 1_000 });
    let c = drain(p);
    assert_eq!(c[1], Cmd::Rx { at: 1_000 + T_IFS_US - EVENT_JITTER_US, window: 2 * EVENT_JITTER_US + 40 });
    // central sends CONNECT_IND (ends at 1_400 in the mock)
    let ci = connect_ind(24, 2, 0);
    let ev = rx(&ci, true, 1_150);
    let RadioEvent::Rx(r) = &ev else { unreachable!() };
    let end = r.end_us;
    push(p, ev);
    let c = drain(p);
    assert_eq!(p.take_notification().map(|n| matches!(n, Notify::Connected { interval: 24, .. })), Some(true));
    // first event: transmitWindowDelay 1.25 ms + offset 2 * 1.25 ms after CONNECT_IND end
    let start = end + 1250 + 2 * UNIT_US;
    let Cmd::Rx { at, window } = c.iter().find(|c| matches!(c, Cmd::Rx { .. })).unwrap().clone() else { unreachable!() };
    assert_eq!(at, start - EVENT_JITTER_US);
    assert_eq!(window, 2 * UNIT_US + 2 * EVENT_JITTER_US);
    start
}

#[test]
fn scan_request_gets_scan_response_after_ifs() {
    let mut p = Peripheral::new(Mock::default(), cfg());
    p.start_advertising();
    drain(&mut p);
    push(&mut p, RadioEvent::TxDone { end_us: 500 });
    drain(&mut p);
    let mut req = vec![0x03, 12];
    req.extend_from_slice(&[9, 9, 9, 9, 9, 9]);
    req.extend_from_slice(&ADDR);
    push(&mut p, rx(&req, true, 650));
    let c = drain(&mut p);
    let Cmd::TxAfterRx(r) = &c[0] else { panic!("{c:?}") };
    let h = AdvHeader::parse(r).unwrap();
    assert_eq!((h.ty, h.length), (AdvType::ScanRsp, 6 + 5));
    assert_eq!(&r[8..], &[4, 9, b'O', b'W', b'O']);
    // wrong AdvA in SCAN_REQ is ignored: next channel
    push(&mut p, RadioEvent::TxDone { end_us: 900 });
    let c = drain(&mut p);
    assert!(matches!(c[0], Cmd::Configure { ch: 38, .. }));
}

#[test]
fn advertising_event_visits_three_channels_then_waits_interval() {
    let mut p = Peripheral::new(Mock::default(), cfg());
    p.seed(0);
    p.start_advertising();
    drain(&mut p);
    for (i, ch) in [38u8, 39].iter().enumerate() {
        push(&mut p, RadioEvent::TxDone { end_us: 1000 * (i as u32 + 1) });
        drain(&mut p);
        push(&mut p, RadioEvent::RxTimeout);
        let c = drain(&mut p);
        assert!(matches!(c[0], Cmd::Configure { ch: x, .. } if x == *ch), "{c:?}");
    }
    push(&mut p, RadioEvent::TxDone { end_us: 3000 });
    drain(&mut p);
    push(&mut p, RadioEvent::RxTimeout);
    drain(&mut p);
    let d = p.next_deadline_us().unwrap();
    assert!((100_000..=110_000).contains(&d), "{d}");
}

#[test]
fn connection_event_exchange_acks_and_delivers_data() {
    let mut p = Peripheral::new(Mock::default(), cfg());
    let start = start_connection(&mut p);
    // Central's first packet: empty PDU, SN=0 NESN=0, arrives inside the window.
    push(&mut p, rx(&[0x01, 0], true, start + 5));
    let c = drain(&mut p);
    let Cmd::TxAfterRx(t) = &c[0] else { panic!("{c:?}") };
    let h = DataHeader::parse(t).unwrap();
    // We expect SN 1 next (nesn=1: ack), our own sn=0, empty
    assert_eq!((h.llid, h.length, h.nesn, h.sn, h.md), (1, 0, true, false, false));
    push(&mut p, RadioEvent::TxDone { end_us: start + 200 });
    let c = drain(&mut p);
    // next event 30 ms after the anchor, widened by (50 + 250) ppm * 30 ms = 9 µs, +16 µs jitter
    let ch1 = match c.iter().find(|c| matches!(c, Cmd::Configure { .. })).unwrap() {
        Cmd::Configure { ch, aa, crc } => {
            assert_eq!((*aa, *crc), (0x5065_4321, 0x12_3456));
            *ch
        }
        _ => unreachable!(),
    };
    assert!(ch1 < 37);
    let Cmd::Rx { at, window } = c.iter().find(|c| matches!(c, Cmd::Rx { .. })).unwrap().clone() else { unreachable!() };
    let anchor = start + 5;
    let widening = (300u32 * 30_000).div_ceil(1_000_000);
    assert_eq!(at, anchor + 30_000 - widening - EVENT_JITTER_US);
    assert_eq!(window, 2 * (widening + EVENT_JITTER_US));

    // Event 2: central sends an L2CAP start fragment with SN=1 NESN=1 (acks our empty PDU).
    p.queue_data(true, &[1, 2, 3]).unwrap();
    let a2 = anchor + 30_000 + 3;
    push(&mut p, rx(&[0x02 | 0x04 | 0x08, 4, 0xDE, 0xAD, 0xBE, 0xEF], true, a2));
    let c = drain(&mut p);
    let Cmd::TxAfterRx(t) = &c[0] else { panic!("{c:?}") };
    let h = DataHeader::parse(t).unwrap();
    assert_eq!((h.llid, h.length, h.sn, h.nesn), (2, 3, true, false)); // new data from us, we ack
    assert_eq!(&t[2..], &[1, 2, 3]);
    match p.take_notification() {
        Some(Notify::Data { start: true, len: 4, data }) => assert_eq!(&data[..4], &[0xDE, 0xAD, 0xBE, 0xEF]),
        n => panic!("{n:?}"),
    }
    push(&mut p, RadioEvent::TxDone { end_us: a2 + 200 });
    drain(&mut p);

    // Event 3: central repeats its previous packet (SN=1) and NAKs ours (NESN == our SN == 1):
    // we must retransmit the same PDU and not deliver the duplicate payload again.
    let a3 = a2 + 30_000;
    push(&mut p, rx(&[0x01 | 0x04 | 0x08, 0], true, a3));
    let c = drain(&mut p);
    let Cmd::TxAfterRx(t) = &c[0] else { panic!("{c:?}") };
    let h = DataHeader::parse(t).unwrap();
    assert_eq!((h.llid, h.sn, h.length), (2, true, 3), "retransmission of the same PDU");
    assert_eq!(&t[2..], &[1, 2, 3]);
    assert_eq!(p.take_notification(), None, "duplicate not delivered twice");
}

#[test]
fn crc_failure_is_not_answered_and_widening_grows() {
    let mut p = Peripheral::new(Mock::default(), cfg());
    let start = start_connection(&mut p);
    push(&mut p, rx(&[0x01, 0], true, start));
    drain(&mut p);
    push(&mut p, RadioEvent::TxDone { end_us: start + 200 });
    drain(&mut p);
    // event 1: bad CRC -> no TxAfterRx, next event planned 2 intervals after the anchor
    push(&mut p, rx(&[0x01, 0], false, start + 30_000));
    let c = drain(&mut p);
    assert!(!c.iter().any(|c| matches!(c, Cmd::TxAfterRx(_))));
    let Cmd::Rx { at, window } = c.iter().find(|c| matches!(c, Cmd::Rx { .. })).unwrap().clone() else { unreachable!() };
    let widening = (300u32 * 60_000).div_ceil(1_000_000);
    assert_eq!(at, start + 60_000 - widening - EVENT_JITTER_US);
    assert_eq!(window, 2 * (widening + EVENT_JITTER_US));
}

#[test]
fn terminate_ind_ends_connection_with_remote_reason() {
    let mut p = Peripheral::new(Mock::default(), cfg());
    let start = start_connection(&mut p);
    push(&mut p, rx(&[0x03, 2, 0x02, 0x13], true, start));
    let c = drain(&mut p);
    assert!(matches!(c[0], Cmd::TxAfterRx(_)));
    push(&mut p, RadioEvent::TxDone { end_us: start + 250 });
    drain(&mut p);
    let mut got = None;
    while let Some(n) = p.take_notification() {
        got = Some(n);
    }
    assert_eq!(got, Some(Notify::Disconnected { reason: 0x13 }));
    assert!(!p.is_connected());
}

#[test]
fn feature_request_gets_feature_response_and_unknown_gets_unknown_rsp() {
    let mut p = Peripheral::new(Mock::default(), cfg());
    let start = start_connection(&mut p);
    let mut f = vec![0x03, 9, 0x08];
    f.extend_from_slice(&[0xFF; 8]);
    push(&mut p, rx(&f, true, start));
    drain(&mut p);
    push(&mut p, RadioEvent::TxDone { end_us: start + 250 });
    drain(&mut p);
    // Next event: the central acks (NESN=1) and sends an unknown opcode (SN=1)
    push(&mut p, rx(&[0x03 | 0x04 | 0x08, 1, 0x1A], true, start + 30_000));
    let c = drain(&mut p);
    let Cmd::TxAfterRx(t) = &c[0] else { panic!("{c:?}") };
    // our first reply (FEATURE_RSP, sn=0) was acked, so now the queued reply is UNKNOWN_RSP
    assert_eq!(&t[2..], &[0x07, 0x1A]);
}

#[test]
fn supervision_timeout_disconnects() {
    let mut p = Peripheral::new(Mock::default(), cfg());
    let start = start_connection(&mut p);
    push(&mut p, rx(&[0x01, 0], true, start));
    drain(&mut p);
    push(&mut p, RadioEvent::TxDone { end_us: start + 200 });
    drain(&mut p);
    // 200 * 10 ms = 2 s; every following event times out
    for k in 1..=100u32 {
        p.radio_mut().now = start + k * 30_000;
        push(&mut p, RadioEvent::RxTimeout);
        drain(&mut p);
        if !p.is_connected() {
            break;
        }
    }
    let mut got = None;
    while let Some(n) = p.take_notification() {
        got = Some(n);
    }
    assert_eq!(got, Some(Notify::Disconnected { reason: 0x08 }));
}
