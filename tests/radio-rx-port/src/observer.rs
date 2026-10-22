//! Scanner-only smoke: records what Zephyr's host scanner delivers.
use zephyr::printkln;
use zephyr::raw::{
    bt_le_scan_cb, bt_le_scan_cb_register, bt_le_scan_param, bt_le_scan_recv_info, bt_le_scan_start,
    net_buf_simple,
};

unsafe extern "C" {
    fn bt_enable(cb: Option<unsafe extern "C" fn(i32)>) -> i32;
}

#[cfg(not(CONFIG_RADIO_RX_PORT_ACTIVE_SCAN))]
const BT_LE_SCAN_TYPE_PASSIVE: u8 = 0;
#[cfg(CONFIG_RADIO_RX_PORT_ACTIVE_SCAN)]
const BT_LE_SCAN_TYPE_ACTIVE: u8 = 1;

#[cfg(CONFIG_RADIO_RX_PORT_ACTIVE_SCAN)]
const SCAN_TYPE: u8 = BT_LE_SCAN_TYPE_ACTIVE;
#[cfg(not(CONFIG_RADIO_RX_PORT_ACTIVE_SCAN))]
const SCAN_TYPE: u8 = BT_LE_SCAN_TYPE_PASSIVE;

/*
 * This callback is intentionally a probe for the QEMU-only descriptor seam.
 * It records the first bytes delivered by Zephyr's host scanner; it does not
 * inspect the radio FIFO or treat a raw BUFC completion as a host callback.
 */
unsafe extern "C" fn scan_recv(
    _info: *const bt_le_scan_recv_info,
    buf: *mut net_buf_simple,
) {
    if buf.is_null() {
        return;
    }

    // SAFETY: Zephyr supplies a live net_buf_simple for the duration of the
    // callback and never calls this function with a null buffer.
    let packet = unsafe { &*buf };
    let first = if packet.data.is_null() {
        0
    } else {
        // SAFETY: Zephyr's buffer contract makes data valid for len bytes.
        unsafe { *packet.data }
    };
    let second = if packet.data.is_null() || packet.len < 2 {
        0
    } else {
        // SAFETY: The length check above proves the second byte is present.
        unsafe { *packet.data.add(1) }
    };
    let third = if packet.data.is_null() || packet.len < 3 {
        0
    } else {
        // SAFETY: The length check above proves the third byte is present.
        unsafe { *packet.data.add(2) }
    };
    printkln!(
        "rx_callback: len={} first={} second={} third={}",
        packet.len,
        first,
        second,
        third
    );
}

static mut SCAN_CALLBACKS: bt_le_scan_cb = bt_le_scan_cb {
    recv: Some(scan_recv),
    timeout: None,
    node: zephyr::raw::_snode {
        next: core::ptr::null_mut(),
    },
};

#[no_mangle]
extern "C" fn rust_main() {
    // SAFETY: null callback requests synchronous controller initialization.
    let err = unsafe { bt_enable(None) };
    printkln!("bt_enable: {}", err);
    if err != 0 {
        return;
    }

    // SAFETY: this static callback remains registered for the life of the
    // image, and Zephyr invokes it only from thread context.
    let callback_err = unsafe { bt_le_scan_cb_register(&raw mut SCAN_CALLBACKS) };
    printkln!("scan_callback_register: {}", callback_err);
    if callback_err != 0 {
        return;
    }

    let scan = bt_le_scan_param {
        type_: SCAN_TYPE,
        options: 0,
        // Continuous scanning asks the real LLL to arm RX. In the opt-in
        // active-scanning profile, Zephyr stages SCAN_REQ after an eligible
        // advertisement; the RX-to-TX transition remains intentionally
        // unavailable until the QEMU-only HAL seam provides it. The callback
        // is reached only after the QEMU descriptor seam has normalized a
        // bounded PDU for Zephyr's host.
        interval: 160,
        window: 160,
        timeout: 0,
        interval_coded: 0,
        window_coded: 0,
    };
    // SAFETY: Zephyr copies this parameter structure while starting scanning.
    let scan_err = unsafe { bt_le_scan_start(&scan, None) };
    printkln!("scan_start: {} type={}", scan_err, SCAN_TYPE);
}
