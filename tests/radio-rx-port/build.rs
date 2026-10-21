fn main() {
    println!("cargo::rustc-check-cfg=cfg(CONFIG_RADIO_RX_PORT_ACTIVE_SCAN)");
    zephyr_build::export_kconfig_bool_options();
}
