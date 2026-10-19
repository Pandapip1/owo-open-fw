//! Generates bindings to the Silicon Labs emlib / clock manager pieces used by the xG22
//! backend (feature `xg22`). Same approach as the application's build.rs: reuse the include
//! paths and defines Zephyr hands to `zephyr-sys`.
//!
//! Static inline emlib functions cannot be bound; the backend writes the few registers it
//! needs through the generated register structs instead.

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=wrapper.h");
    #[cfg(feature = "xg22")]
    gen();
    #[cfg(not(feature = "xg22"))]
    {}
}

#[cfg(feature = "xg22")]
fn gen() {
    use std::env;
    use std::path::PathBuf;

    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let (Ok(zephyr_base), Ok(includes), Ok(defines)) = (
        env::var("ZEPHYR_BASE"),
        env::var("INCLUDE_DIRS"),
        env::var("INCLUDE_DEFINES"),
    ) else {
        std::fs::write(out.join("emlib_bindings.rs"), "").unwrap();
        println!("cargo:warning=INCLUDE_DIRS/INCLUDE_DEFINES/ZEPHYR_BASE unset: emlib bindings skipped");
        return;
    };
    let target = env::var("TARGET").expect("TARGET");

    let mut b = bindgen::Builder::default()
        .header("wrapper.h")
        .use_core()
        .derive_default(true)
        .derive_copy(true)
        .prepend_enum_name(false)
        .clang_arg("-DRUST_BINDGEN")
        .clang_arg(format!("-I{zephyr_base}/lib/libc/minimal/include"))
        .clang_arg(format!("--target={target}"));
    for e in includes.split([' ', ';']).filter(|s| !s.is_empty()) {
        b = b.clang_arg(format!("-I{e}"));
    }
    for e in defines.split([' ', ';']).filter(|s| !s.is_empty()) {
        b = b.clang_arg(format!("-D{e}"));
    }
    for f in [
        "GPIO_PinModeSet",
        "TIMER_Init",
        "TIMER_InitCC",
        "IADC_init",
        "IADC_initSingle",
        "IADC_pullSingleFifoResult",
        "IADC_calcSrcClkPrescale",
        "IADC_calcAdcClkPrescale",
        "IADC_updateConfigs",
        "sl_clock_manager_enable_bus_clock",
        "sl_clock_manager_get_clock_branch_frequency",
    ] {
        b = b.allowlist_function(f);
    }
    for t in [
        "GPIO_.*",
        "TIMER_.*",
        "IADC_.*",
        "sl_status_t",
        "sl_bus_clock_t",
        "sl_clock_branch_t",
    ] {
        b = b.allowlist_type(t);
    }
    for v in [
        "SL_BUS_CLOCK_.*_VALUE",
        "TIMER1_BASE",
        "IADC0_BASE",
        "GPIO_BASE",
    ] {
        b = b.allowlist_var(v);
    }
    b = b.allowlist_item("SL_CLOCK_BRANCH_.*");
    b.generate()
        .expect("bindgen failed on emlib headers")
        .write_to_file(out.join("emlib_bindings.rs"))
        .expect("write bindings");
}
