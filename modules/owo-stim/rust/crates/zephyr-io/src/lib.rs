//! Thin Rust handles over Zephyr driver APIs that the `zephyr` crate does not wrap yet (SPI, DAC,
//! PWM, ADC), plus GPIO by `gpio_dt_spec`. Every handle is an opaque pointer to a statically
//! allocated devicetree spec or `struct device`, produced by the C shim (`shim/owo_shim.c`);
//! the shim does the actual Zephyr calls because most of them are static inline or syscall
//! wrappers that bindgen cannot see.
//!
//! All functions return `0` or a negative errno, like Zephyr.

#![no_std]

use core::ffi::{c_int, c_void};
use owo_hal::{Errno, GpioPort, OutputPin, SpiDevice, Switchable};

extern "C" {
    fn owo_gpio_configure(g: *const c_void, active: c_int) -> c_int;
    fn owo_gpio_set(g: *const c_void, active: c_int) -> c_int;
    fn owo_gpio_port_set_masked(port: *const c_void, mask: u32, value: u32) -> c_int;
    fn owo_regulator_set(reg: *const c_void, on: c_int) -> c_int;
    fn owo_spi_transceive(s: *const c_void, tx: *const u8, rx: *mut u8, len: usize) -> c_int;
    fn owo_dac_channel_setup(d: *const c_void, ch: u8, resolution: u8) -> c_int;
    fn owo_dac_write(d: *const c_void, ch: u8, value: u32) -> c_int;
    fn owo_pwm_set_percent(p: *const c_void, percent: u8) -> c_int;
    fn owo_adc_setup(a: *const c_void) -> c_int;
    fn owo_adc_start(a: *const c_void) -> c_int;
    fn owo_adc_poll(raw: *mut u16) -> c_int;
    fn owo_adc_raw_to_mv(a: *const c_void, raw: u16) -> i32;
    fn owo_counter_start(d: *const c_void) -> c_int;
    fn owo_counter_stop(d: *const c_void);
    fn owo_counter_mark(d: *const c_void) -> u32;
    fn owo_counter_since_us(d: *const c_void, mark: u32) -> u32;
    fn owo_busy_wait_us(us: u32);
    fn owo_suit_post() -> c_int;
    fn owo_suit_handle(kind: c_int) -> *const c_void;
    fn owo_suit_param(kind: c_int) -> u32;
    fn owo_suit_switch_routes() -> *const u32;
    fn owo_suit_stop_input_setup() -> c_int;
    fn owo_suit_stop_input_get() -> c_int;
}

/// Non-energising POST for every driver named by the `owo,suit-output` node.
/// A successful result means the forced shutdown initializer may safely use
/// each driver; it does not itself alter any output.
pub fn suit_post() -> Result<(), Errno> {
    // SAFETY: pure readiness checks on static devicetree devices.
    check(unsafe { owo_suit_post() })
}

fn check(r: c_int) -> Result<(), Errno> {
    if r < 0 {
        Err(r)
    } else {
        Ok(())
    }
}

/// Handle kinds understood by `owo_suit_handle` (see the shim). Supplies are `16 + index`.
#[derive(Clone, Copy)]
#[repr(i32)]
pub enum SuitHandle {
    BridgeHighA = 0,
    BridgeHighB = 1,
    BridgeLowA = 2,
    BridgeLowB = 3,
    Switch = 4,
    Dac = 5,
    BoostPwm = 6,
    SenseAdc = 7,
    PhaseTimer = 8,
    BatteryAdc = 9,
}

/// Board parameters from the `owo,suit-output` node (kinds understood by `owo_suit_param`).
#[derive(Clone, Copy)]
#[repr(i32)]
pub enum SuitParam {
    DacChannel = 0,
    DacResolution = 1,
    DacScaleNum = 2,
    DacScaleDen = 3,
    MaxLevel = 4,
    SenseUvPerLevel = 5,
    SenseOffset = 6,
    BoostDutyPercent = 7,
    SupplyCount = 8,
    SwitchRouteCount = 9,
    BatteryDividerNum = 10,
    BatteryDividerDen = 11,
    HasStopInput = 12,
}

/// The suit's devicetree node, if the board describes one. Null when the node is absent or its
/// device is not ready.
pub fn suit_handle(kind: SuitHandle) -> *const c_void {
    // SAFETY: pure lookup of a static handle; no side effects.
    unsafe { owo_suit_handle(kind as c_int) }
}

/// Handle of supply regulator `index`.
pub fn suit_supply_handle(index: u32) -> *const c_void {
    // SAFETY: pure lookup.
    unsafe { owo_suit_handle(16 + index as c_int) }
}

pub fn suit_param(kind: SuitParam) -> u32 {
    // SAFETY: pure lookup of a devicetree constant.
    unsafe { owo_suit_param(kind as c_int) }
}

/// Configure the board's stop input (`stop-gpios`) and its change callback, which calls
/// `owo_suit_stop_input_changed`. `Err(-ENOTSUP)` when the board has none.
pub fn suit_stop_input_setup() -> Result<(), Errno> {
    // SAFETY: plain driver calls on a static devicetree spec.
    check(unsafe { owo_suit_stop_input_setup() })
}

/// Whether the stop input is active now (reads its controller).
pub fn suit_stop_input() -> Result<bool, Errno> {
    // SAFETY: as above.
    let r = unsafe { owo_suit_stop_input_get() };
    check(r).map(|_| r != 0)
}

/// Per-channel switch pin masks from devicetree (`switch-routes`); static for the program's life.
pub fn suit_switch_routes() -> &'static [u32] {
    let n = suit_param(SuitParam::SwitchRouteCount) as usize;
    // SAFETY: the shim returns a pointer to a static array of exactly `n` words (or null with n == 0).
    unsafe {
        let p = owo_suit_switch_routes();
        if p.is_null() {
            &[]
        } else {
            core::slice::from_raw_parts(p, n)
        }
    }
}

pub struct Gpio(*const c_void);
// SAFETY: the handle points at a static `gpio_dt_spec`; Zephyr's GPIO API is callable from any context.
unsafe impl Send for Gpio {}

impl Gpio {
    /// # Safety
    /// `p` must be a `struct gpio_dt_spec *` with static lifetime, from the shim.
    pub unsafe fn from_raw(p: *const c_void) -> Self {
        Self(p)
    }
    /// Configure as an output, starting at the given logical level.
    pub fn configure_output(&mut self, active: bool) -> Result<(), Errno> {
        // SAFETY: valid handle by construction.
        check(unsafe { owo_gpio_configure(self.0, active as c_int) })
    }
}
impl OutputPin for Gpio {
    fn set(&mut self, active: bool) -> Result<(), Errno> {
        // SAFETY: valid handle by construction.
        check(unsafe { owo_gpio_set(self.0, active as c_int) })
    }
}

pub struct Spi(*const c_void);
// SAFETY: static `spi_dt_spec`; the SPI API serialises internally.
unsafe impl Send for Spi {}
impl Spi {
    /// # Safety
    /// `p` must be a `struct spi_dt_spec *` with static lifetime, from the shim.
    pub unsafe fn from_raw(p: *const c_void) -> Self {
        Self(p)
    }
}
impl SpiDevice for Spi {
    fn transfer(&mut self, tx: &[u8], rx: &mut [u8]) -> Result<(), Errno> {
        if tx.len() != rx.len() {
            return Err(-22);
        }
        // SAFETY: pointers/lengths come from live slices of equal length.
        check(unsafe { owo_spi_transceive(self.0, tx.as_ptr(), rx.as_mut_ptr(), tx.len()) })
    }
    fn write(&mut self, tx: &[u8]) -> Result<(), Errno> {
        // SAFETY: tx is a live slice; a null rx tells the shim to discard the reply.
        check(unsafe { owo_spi_transceive(self.0, tx.as_ptr(), core::ptr::null_mut(), tx.len()) })
    }
}

/// A Zephyr DAC (`struct device *` implementing the DAC API).
pub struct Dac(*const c_void);
// SAFETY: static device pointer.
unsafe impl Send for Dac {}
impl Dac {
    /// # Safety
    /// `p` must be a ready `struct device *` of a DAC.
    pub unsafe fn from_raw(p: *const c_void) -> Self {
        Self(p)
    }
    pub fn channel_setup(&mut self, channel: u8, resolution: u8) -> Result<(), Errno> {
        // SAFETY: valid handle by construction.
        check(unsafe { owo_dac_channel_setup(self.0, channel, resolution) })
    }
    pub fn write(&mut self, channel: u8, value: u32) -> Result<(), Errno> {
        // SAFETY: valid handle by construction.
        check(unsafe { owo_dac_write(self.0, channel, value) })
    }
}

/// A PWM output (`struct pwm_dt_spec`).
pub struct Pwm(*const c_void);
// SAFETY: static spec.
unsafe impl Send for Pwm {}
impl Pwm {
    /// # Safety
    /// `p` must be a `struct pwm_dt_spec *` with static lifetime, from the shim.
    pub unsafe fn from_raw(p: *const c_void) -> Self {
        Self(p)
    }
    pub fn set_percent(&mut self, percent: u8) -> Result<(), Errno> {
        // SAFETY: valid handle by construction.
        check(unsafe { owo_pwm_set_percent(self.0, percent) })
    }
}

/// Fine timestamps (<= 1 us per cycle) for timing short intervals, read from Zephyr's system
/// clock.
pub struct FineCounter(*const c_void);
// SAFETY: static identity token from the shim.
unsafe impl Send for FineCounter {}
impl FineCounter {
    /// # Safety
    /// `p` must be the shim's `SuitHandle::PhaseTimer` handle.
    pub unsafe fn from_raw(p: *const c_void) -> Self {
        Self(p)
    }
    /// Check the system clock is fine enough (fails with `-ENOTSUP` above 1 us per cycle).
    pub fn start(&mut self) -> Result<(), Errno> {
        // SAFETY: valid handle by construction.
        check(unsafe { owo_counter_start(self.0) })
    }
    pub fn stop(&mut self) {
        // SAFETY: valid handle by construction.
        unsafe { owo_counter_stop(self.0) }
    }
    pub fn mark(&self) -> u32 {
        // SAFETY: valid handle by construction.
        unsafe { owo_counter_mark(self.0) }
    }
    pub fn since_us(&self, mark: u32) -> u32 {
        // SAFETY: valid handle by construction.
        unsafe { owo_counter_since_us(self.0, mark) }
    }
}

/// A GPIO controller device used as a parallel port.
pub struct Port(*const c_void);
// SAFETY: static device pointer; the GPIO API is callable from any thread context.
unsafe impl Send for Port {}
impl Port {
    /// # Safety
    /// `p` must be a ready `struct device *` of a GPIO controller.
    pub unsafe fn from_raw(p: *const c_void) -> Self {
        Self(p)
    }
}
impl GpioPort for Port {
    fn set_masked(&mut self, mask: u32, value: u32) -> Result<(), Errno> {
        // SAFETY: valid handle by construction.
        check(unsafe { owo_gpio_port_set_masked(self.0, mask, value) })
    }
}

/// A Zephyr regulator (`struct device *`).
pub struct Regulator(*const c_void);
// SAFETY: static device pointer.
unsafe impl Send for Regulator {}
impl Regulator {
    /// # Safety
    /// `p` must be a ready `struct device *` of a regulator.
    pub unsafe fn from_raw(p: *const c_void) -> Self {
        Self(p)
    }
}
impl Switchable for Regulator {
    fn set(&mut self, on: bool) -> Result<(), Errno> {
        // SAFETY: valid handle by construction.
        check(unsafe { owo_regulator_set(self.0, on as c_int) })
    }
}

/// One ADC channel (`struct adc_dt_spec`). Conversions run one at a time across all channels:
/// the caller starts one and polls for its result; the shim's completion callback (the ADC
/// interrupt) publishes it. Nothing converts unless asked.
pub struct AdcChannel(*const c_void);
// SAFETY: static spec; the shim's state is an atomic.
unsafe impl Send for AdcChannel {}
impl AdcChannel {
    /// # Safety
    /// `p` must be a `struct adc_dt_spec *` with static lifetime, from the shim.
    pub unsafe fn from_raw(p: *const c_void) -> Self {
        Self(p)
    }
    /// Configure the channel (once, before its first conversion).
    pub fn setup(&mut self) -> Result<(), Errno> {
        // SAFETY: valid handle by construction.
        check(unsafe { owo_adc_setup(self.0) })
    }
    /// Start one conversion (thread context). `-EBUSY` while an earlier conversion, of any
    /// channel, has not completed.
    pub fn start(&mut self) -> Result<(), Errno> {
        // SAFETY: valid handle by construction.
        check(unsafe { owo_adc_start(self.0) })
    }
    /// The raw result of the conversion started last, once (`None` while it runs).
    pub fn poll() -> Option<u16> {
        let mut raw = 0u16;
        // SAFETY: the out-pointer is a live local.
        (unsafe { owo_adc_poll(&mut raw) } == 1).then_some(raw)
    }
    /// Millivolts at the pin for a raw result of this channel (`-1` if it cannot be converted).
    pub fn raw_to_mv(&self, raw: u16) -> i32 {
        // SAFETY: valid handle; pure conversion.
        unsafe { owo_adc_raw_to_mv(self.0, raw) }
    }
}

pub fn busy_wait_us(us: u32) {
    // SAFETY: k_busy_wait is callable from any context.
    unsafe { owo_busy_wait_us(us) }
}
