//! Scanner-only smoke test for the EFR32 lower-link-layer integration.
#![no_std]

use zephyr::printkln;
use zephyr::raw::{bt_le_scan_param, bt_le_scan_start};
use zephyr_panic as _;

unsafe extern "C" {
    fn bt_enable(cb: Option<unsafe extern "C" fn(i32)>) -> i32;
}

const BT_LE_SCAN_TYPE_PASSIVE: u8 = 0;

#[no_mangle]
extern "C" fn rust_main() {
    // SAFETY: null callback requests synchronous controller initialization.
    let err = unsafe { bt_enable(None) };
    printkln!("bt_enable: {}", err);
    if err != 0 {
        return;
    }

    let scan = bt_le_scan_param {
        type_: BT_LE_SCAN_TYPE_PASSIVE,
        options: 0,
        // Continuous passive scanning asks the real LLL to arm RX. The null
        // callback intentionally avoids claiming raw BUFC bytes are a PDU.
        interval: 160,
        window: 160,
        timeout: 0,
        interval_coded: 0,
        window_coded: 0,
    };
    // SAFETY: Zephyr copies this parameter structure while starting scanning.
    let scan_err = unsafe { bt_le_scan_start(&scan, None) };
    printkln!("scan_start: {}", scan_err);
}
