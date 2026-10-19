//! Entry points for the C glue in `drivers/dac/dac_ltc2634.c`. The glue owns the Zephyr device
//! object and the devicetree config; everything the driver *does* is here.
//!
//! The functions are stateless: all the state a DAC channel needs lives in the hardware.

use crate::{Error, Ltc2634};
use core::ffi::c_void;
use zephyr_io::{Gpio, Spi};

/// Zephyr errno values used by the glue contract.
const EINVAL: i32 = -22;

fn errno(e: Error) -> i32 {
    match e {
        Error::BadChannel | Error::BadValue => EINVAL,
        Error::Spi(n) | Error::Reset(n) => n,
    }
}

/// Zephyr `dac_channel_setup` check: 12-bit, no internal channels, channel 0..=3 or 15.
#[no_mangle]
pub extern "C" fn ltc2634_rs_channel_setup(channel: u8, resolution: u8, internal: bool) -> i32 {
    if internal || resolution != 12 || !crate::valid_channel(channel) {
        EINVAL
    } else {
        0
    }
}

/// Device init: pulse /CLR if present so the outputs start at zero. `reset` may be null.
///
/// # Safety
/// `bus` must be a ready `struct spi_dt_spec *`; `reset` null or a `struct gpio_dt_spec *`
/// (pre-configured as an output by the glue).
#[no_mangle]
pub unsafe extern "C" fn ltc2634_rs_init(bus: *const c_void, reset: *const c_void) -> i32 {
    // SAFETY: caller contract above.
    let spi = unsafe { Spi::from_raw(bus) };
    // SAFETY: caller contract above.
    let reset = (!reset.is_null()).then(|| unsafe { Gpio::from_raw(reset) });
    let mut d = Ltc2634::new(spi, reset);
    match d.clear(|| zephyr_io::busy_wait_us(2)) {
        Ok(()) => 0,
        Err(e) => errno(e),
    }
}

/// Zephyr `dac_write_value`.
///
/// # Safety
/// `bus` must be a ready `struct spi_dt_spec *`.
#[no_mangle]
pub unsafe extern "C" fn ltc2634_rs_write_value(bus: *const c_void, channel: u8, value: u32) -> i32 {
    // SAFETY: caller contract above.
    let spi = unsafe { Spi::from_raw(bus) };
    let Ok(code) = u16::try_from(value) else {
        return EINVAL;
    };
    let mut d: Ltc2634<Spi, Gpio> = Ltc2634::new(spi, None);
    match d.write(channel, code) {
        Ok(()) => 0,
        Err(e) => errno(e),
    }
}
