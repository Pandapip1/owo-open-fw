//! Rust smoke test for the EFR32 lower-link-layer integration.
#![no_std]

use core::ptr;

use zephyr::printkln;
use zephyr::raw::{bt_data, bt_le_adv_param, bt_le_adv_start};
use zephyr_panic as _;

unsafe extern "C" {
    fn bt_enable(cb: Option<unsafe extern "C" fn(i32)>) -> i32;
}

/* bluetooth.h: BT_LE_ADV_OPT_CONN = BIT(0) | BIT(1).  Keep this payload
 * deliberately small: this smoke exists to exercise Zephyr's real advertising
 * scheduling path, not to stand in for a received or transmitted air packet. */
const BT_LE_ADV_OPT_CONN: u32 = 3;
const BT_DATA_FLAGS: u8 = 0x01;
const BT_LE_AD_GENERAL: u8 = 0x02;
const BT_LE_AD_NO_BREDR: u8 = 0x04;
static AD_FLAGS: u8 = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;

#[no_mangle]
extern "C" fn rust_main() {
    // SAFETY: null callback requests synchronous controller initialization.
    let err = unsafe { bt_enable(None) };

    printkln!("bt_enable: {}", err);
    if err != 0 {
        return;
    }

    let ad = [bt_data {
        type_: BT_DATA_FLAGS,
        data_len: 1,
        data: &AD_FLAGS,
    }];
    let param = bt_le_adv_param {
        id: 0,
        sid: 0,
        secondary_max_skip: 0,
        options: BT_LE_ADV_OPT_CONN,
        // 100 ms and 105 ms in 0.625-ms units.  This is within the legacy
        // range and gives the scheduler several real advertising deadlines
        // during the bounded QEMU run.
        interval_min: 160,
        interval_max: 168,
        peer: ptr::null(),
        ..Default::default()
    };
    // SAFETY: Zephyr consumes the advertising data during this synchronous
    // API call; `ad`, `param`, and its static payload remain valid throughout.
    let adv_err = unsafe { bt_le_adv_start(&param, ad.as_ptr(), ad.len(), ptr::null(), 0) };
    // A zero here is the host/controller boundary: an actual Zephyr legacy
    // advertising workload has been accepted.  The QEMU harness uses this
    // marker plus RAC tracing to distinguish rejection from a queued workload
    // which never reaches radio MMIO.
    printkln!("adv_start: {}", adv_err);
}
