//! Rust smoke test for the EFR32 lower-link-layer integration.
#![no_std]

use zephyr::printkln;
use zephyr_panic as _;

unsafe extern "C" {
    fn bt_enable(cb: Option<unsafe extern "C" fn(i32)>) -> i32;
}

#[no_mangle]
extern "C" fn rust_main() {
    // SAFETY: null callback requests synchronous controller initialization.
    let err = unsafe { bt_enable(None) };

    printkln!("bt_enable: {}", err);
}
