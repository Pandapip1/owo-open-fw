//! Build script.
//!
//! 1. The pairing serial is baked in at build time (`OWO_SERIAL`): Zephyr's Rust
//!    support exposes only bool/int/hex Kconfig values, not strings.
//! 2. Bindings for the Bluetooth connection and GATT APIs. zephyr-lang-rust's
//!    `zephyr-sys` only wraps `<zephyr/bluetooth/bluetooth.h>`, so `bt_conn_*` and
//!    `bt_gatt_*` are missing; generate them here with the same include paths,
//!    defines and clang target that the Zephyr build hands to `zephyr-sys`.
//!    Types `zephyr-sys` already defines are blocklisted and imported from
//!    `zephyr::raw` instead, so both sets of bindings agree on them.
//!
//! Static inline functions (e.g. `bt_gatt_notify_uuid`) are deliberately not
//! bound: that needs a compiled C wrapper. Their non-inline equivalents are used.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=OWO_SERIAL");
    println!("cargo:rerun-if-changed=owo_wrapper.h");
    println!("cargo:rerun-if-changed=build.rs");

    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let (Ok(zephyr_base), Ok(includes), Ok(defines)) = (
        env::var("ZEPHYR_BASE"),
        env::var("INCLUDE_DIRS"),
        env::var("INCLUDE_DEFINES"),
    ) else {
        // Not under the Zephyr build (e.g. a bare `cargo check`): emit an empty module.
        std::fs::write(out.join("ble_bindings.rs"), "").unwrap();
        println!("cargo:warning=INCLUDE_DIRS/INCLUDE_DEFINES/ZEPHYR_BASE unset: BLE bindings skipped");
        return;
    };
    let target = env::var("TARGET").expect("TARGET");

    let mut b = bindgen::Builder::default()
        .header("owo_wrapper.h")
        .use_core()
        .derive_default(true)
        .derive_copy(false)
        .prepend_enum_name(false)
        .clang_arg("-DRUST_BINDGEN")
        .clang_arg(format!("-I{zephyr_base}/lib/libc/minimal/include"))
        .clang_arg(format!("--target={target}"))
        .raw_line("use zephyr::raw::*;");

    for e in includes.split([' ', ';']).filter(|s| !s.is_empty()) {
        b = b.clang_arg(format!("-I{e}"));
    }
    for e in defines.split([' ', ';']).filter(|s| !s.is_empty()) {
        b = b.clang_arg(format!("-D{e}"));
    }

    // Already defined by zephyr-sys (reachable from bluetooth.h / kernel.h).
    for t in ["bt_addr_le_t", "bt_addr_t", "bt_conn", "sys_snode_t", "bt_data", "bt_le_adv_param"] {
        b = b.blocklist_type(t);
    }

    for f in [
        "bt_conn_auth_cb_register",
        "bt_conn_cb_register",
        "bt_conn_set_security",
        "bt_gatt_service_register",
        "bt_gatt_attr_read",
        "bt_gatt_attr_read_service",
        "bt_gatt_attr_read_chrc",
        "bt_gatt_attr_read_ccc",
        "bt_gatt_attr_write_ccc",
        "bt_gatt_attr_read_cud",
        "bt_gatt_notify_cb",
    ] {
        b = b.allowlist_function(f);
    }
    for t in [
        "bt_gatt_attr",
        "bt_gatt_chrc",
        "bt_gatt_service",
        "bt_gatt_ccc_managed_user_data",
        "bt_gatt_notify_params",
        "bt_conn_cb",
        "bt_conn_auth_cb",
        "bt_uuid",
        "bt_uuid_16",
        "bt_uuid_128",
        "bt_gatt_perm",
        "bt_security_t",
    ] {
        b = b.allowlist_type(t);
    }
    for v in [
        "BT_UUID_TYPE_.*",
        "BT_GATT_CHRC_.*",
        "BT_ATT_ERR_.*",
        "BT_GATT_PERM_.*",
    ] {
        b = b.allowlist_var(v);
    }

    b.generate()
        .expect("bindgen failed on conn.h/gatt.h")
        .write_to_file(out.join("ble_bindings.rs"))
        .expect("write bindings");
}
