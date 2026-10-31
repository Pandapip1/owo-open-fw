//! Central connection workload (src/central.c): scan for the connectable
//! advertiser, then create an LE link and report the host's connection
//! callbacks. The workload is C because the Rust bindings expose only
//! `<zephyr/bluetooth/bluetooth.h>`, which does not declare the connection API.

unsafe extern "C" {
    fn radio_rx_port_central_start();
}

#[no_mangle]
extern "C" fn rust_main() {
    // SAFETY: called once from the main thread; the C workload owns its state.
    unsafe { radio_rx_port_central_start() };
}
