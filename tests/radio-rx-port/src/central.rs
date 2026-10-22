//! Central connection workload: scan for the connectable advertiser, then
//! create an LE link and report the host's connected callback.
//!
//! This is the QEMU-only proof that the ported lower link layer runs the
//! connection-establishment exchange: the central's CONNECT_IND reaches the
//! advertiser, the advertiser's first connection-event packet comes back, and
//! Zephyr's host on the central reports an established link. It is not a claim
//! about RF airtime or silicon timing.
use core::mem::MaybeUninit;
use core::ptr;

use zephyr::printkln;
use zephyr::raw::{
    bt_addr_le_t, bt_conn, bt_conn_cb, bt_conn_cb_register, bt_conn_le_create,
    bt_conn_le_create_param, bt_le_conn_param, bt_le_scan_param, bt_le_scan_start,
    bt_le_scan_stop, net_buf_simple,
};

unsafe extern "C" {
    fn bt_enable(cb: Option<unsafe extern "C" fn(i32)>) -> i32;
}

const BT_LE_SCAN_TYPE_ACTIVE: u8 = 1;
const BT_ADDR_LE_ANONYMOUS: u8 = 0xFF;

/* Scan window 30 ms and interval 60 ms in 0.625 ms units, matching the
 * BT_GAP_SCAN_FAST_* constants Zephyr's own central sample uses. */
const SCAN_INTERVAL: u16 = 0x0060;
const SCAN_WINDOW: u16 = 0x0030;

/* Connection parameters: 7.5..50 ms interval, no peripheral latency, a 5 s
 * supervision timeout. The minimum interval is deliberate: a bounded QEMU run
 * then crosses many connection events after the link is up. */
const CONN_INTERVAL: u16 = 0x0006;
const CONN_INTERVAL_MAX: u16 = 0x0028;
const CONN_TIMEOUT: u16 = 500;

/* Bindgen emits bt_conn_cb's optional members only when their Kconfig guards
 * are set, so the callback block is zero-initialized and patched at run time
 * rather than written with a literal field list that would drift with the
 * profile. */
// SAFETY: every bt_conn_cb member is a nullable function pointer or a list
// node, so an all-zero block is a valid "no callbacks registered" state.
static mut CONN_CALLBACKS: bt_conn_cb = unsafe { MaybeUninit::<bt_conn_cb>::zeroed().assume_init() };

/* The peer captured from the first accepted advertisement report. Bindgen does
 * not derive Copy for bt_addr_le_t, so it is copied as bytes. The scan callback
 * is the only writer and never writes again once a create has been issued. */
// SAFETY: an all-zero bt_addr_le_t is a valid, unused address; it is fully
// overwritten by the scan callback before CREATE_ISSUED ever reads it.
static mut PEER: bt_addr_le_t = unsafe { MaybeUninit::<bt_addr_le_t>::zeroed().assume_init() };
static mut CREATE_ISSUED: bool = false;
static mut CONN: *mut bt_conn = ptr::null_mut();

unsafe extern "C" fn connected(_conn: *mut bt_conn, err: u8) {
    // Zero here means Zephyr's host accepted the controller's connection
    // complete event: the link is established on the central.
    printkln!("central_connected: {}", err);
}

/*
 * Zephyr hands the report's address only for the duration of this callback, so
 * the peer is copied before the create request is issued. Scanning stops first:
 * creating while scanning would race a second report over PEER, and the
 * controller's scan and initiate windows compete for the same radio.
 */
unsafe extern "C" fn scan_result(
    addr: *const bt_addr_le_t,
    _rssi: i8,
    _adv_type: u8,
    _buf: *mut net_buf_simple,
) {
    if addr.is_null() || CREATE_ISSUED {
        return;
    }
    // SAFETY: addr points at a bt_addr_le_t the host keeps valid for this call.
    let peer = unsafe { &*addr };
    if peer.type_ == BT_ADDR_LE_ANONYMOUS {
        return;
    }

    // SAFETY: PEER is read only after CREATE_ISSUED is set, this is the sole
    // writer, and the copy is the plain 7-byte address structure.
    unsafe {
        ptr::copy_nonoverlapping(
            peer as *const bt_addr_le_t as *const u8,
            &raw mut PEER as *mut u8,
            core::mem::size_of::<bt_addr_le_t>(),
        );
        CREATE_ISSUED = true;
    }

    // SAFETY: scanning is active on this single-threaded caller and stopping
    // it is the documented way to release the scan before creating.
    let stop_err = unsafe { bt_le_scan_stop() };
    printkln!("scan_stop: {}", stop_err);

    let create_param = bt_conn_le_create_param {
        options: 0,
        interval: SCAN_INTERVAL,
        window: SCAN_WINDOW,
        interval_coded: 0,
        window_coded: 0,
        timeout: 0,
    };
    let conn_param = bt_le_conn_param {
        interval_min: CONN_INTERVAL,
        interval_max: CONN_INTERVAL_MAX,
        latency: 0,
        timeout: CONN_TIMEOUT,
    };
    // SAFETY: PEER was initialized above and outlives the call, the parameter
    // structures are copied by the host, and CONN is a live out parameter.
    let err = unsafe {
        bt_conn_le_create(
            &raw const PEER,
            &raw const create_param,
            &raw const conn_param,
            &raw mut CONN,
        )
    };
    printkln!("conn_le_create: {}", err);
}

#[no_mangle]
extern "C" fn rust_main() {
    // SAFETY: null callback requests synchronous controller initialization.
    let err = unsafe { bt_enable(None) };
    printkln!("bt_enable: {}", err);
    if err != 0 {
        return;
    }

    // SAFETY: single-threaded init before any callback can run; the static
    // lives for the life of the image and Zephyr keeps the registered block.
    let cb_err = unsafe {
        let cb = &raw mut CONN_CALLBACKS;
        cb.write(bt_conn_cb::default());
        (*cb).connected = Some(connected);
        bt_conn_cb_register(cb)
    };
    printkln!("conn_cb_register: {}", cb_err);

    let scan = bt_le_scan_param {
        type_: BT_LE_SCAN_TYPE_ACTIVE,
        options: 0,
        interval: SCAN_INTERVAL,
        window: SCAN_WINDOW,
        timeout: 0,
        interval_coded: 0,
        window_coded: 0,
    };
    // SAFETY: the host copies the parameters and keeps the callback for the
    // duration of the scan.
    let scan_err = unsafe { bt_le_scan_start(&raw const scan, Some(scan_result)) };
    printkln!("scan_start: {}", scan_err);
}