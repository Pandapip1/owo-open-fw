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
    fn owo_sense_start(a: *const c_void, interval_us: u32) -> c_int;
    fn owo_sense_wait_first(timeout_us: u32) -> c_int;
    fn owo_sense_stop();
    fn owo_sense_latest(raw: *mut u16, stamp: *mut u64) -> c_int;
    fn owo_adc_raw_to_mv(a: *const c_void, raw: u16) -> i32;
    fn owo_counter_start(d: *const c_void) -> c_int;
    fn owo_counter_stop(d: *const c_void);
    fn owo_counter_mark(d: *const c_void) -> u32;
    fn owo_counter_since_us(d: *const c_void, mark: u32) -> u32;
    fn owo_timing_start();
    fn owo_timing_stamp() -> u64;
    fn owo_timing_to_us(cycles: u64) -> u64;
    fn owo_busy_wait_us(us: u32);
    fn owo_suit_post() -> c_int;
    fn owo_suit_handle(kind: c_int) -> *const c_void;
    fn owo_suit_param(kind: c_int) -> u32;
    fn owo_suit_switch_routes() -> *const u32;
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

/// A free-running 32-bit Counter device (<= 1 us per tick) for timing short intervals.
pub struct FineCounter(*const c_void);
// SAFETY: static device pointer.
unsafe impl Send for FineCounter {}
impl FineCounter {
    /// # Safety
    /// `p` must be a ready `struct device *` of a Counter.
    pub unsafe fn from_raw(p: *const c_void) -> Self {
        Self(p)
    }
    /// Start counting (fails with `-ENOTSUP` if the counter is not 32-bit / fine enough).
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

/// Free-running ADC sampler built on Zephyr's standard ADC API: one asynchronous sequence that
/// repeats from its own completion callback (interrupt context), which stores each conversion
/// with a timestamp from Zephyr's timing API in a depth-1 message queue. Reading never blocks.
pub struct Sampler(*const c_void);
// SAFETY: static spec; the shim's state is a kernel message queue.
unsafe impl Send for Sampler {}
impl Sampler {
    /// # Safety
    /// `p` must be a `struct adc_dt_spec *` with static lifetime, from the shim.
    pub unsafe fn from_raw(p: *const c_void) -> Self {
        Self(p)
    }
    /// Start the timing source and sampling every `interval_us` (thread context; may block
    /// briefly if a previous run is still finishing).
    pub fn start(&mut self, interval_us: u32) -> Result<(), Errno> {
        // SAFETY: plain call.
        unsafe { owo_timing_start() };
        // SAFETY: valid handle by construction.
        check(unsafe { owo_sense_start(self.0, interval_us) })
    }
    /// Wait in thread context until the ADC callback has published the first sample.
    /// Returns false when the Zephyr timeout expires.
    pub fn wait_first(&self, timeout_us: u32) -> bool {
        // SAFETY: blocks only the calling thread on the shim's static semaphore.
        unsafe { owo_sense_wait_first(timeout_us) == 0 }
    }
    /// Ask the sampler to stop; after its next conversion it finishes and releases the timing source.
    pub fn stop(&mut self) {
        // SAFETY: only sets a flag / releases the timer.
        unsafe { owo_sense_stop() }
    }
    /// The newest conversion `(raw, millivolts at the pin, timestamp in us)`, or `None` if none
    /// completed since `start`.
    pub fn latest(&self) -> Option<(u16, i32, u64)> {
        let (mut raw, mut stamp) = (0u16, 0u64);
        // SAFETY: both out-pointers are live locals written once.
        let r = unsafe { owo_sense_latest(&mut raw, &mut stamp) };
        if r != 0 {
            return None;
        }
        // SAFETY: valid handle; pure conversion.
        let mv = unsafe { owo_adc_raw_to_mv(self.0, raw) };
        // SAFETY: pure conversion.
        let us = unsafe { owo_timing_to_us(stamp) };
        Some((raw, mv, us))
    }
}

/// Monotonic microseconds from Zephyr's timing API (valid while the timing source is running,
/// i.e. between `Sampler::start` and `stop`; `Suit` keeps it running whenever the supply is on).
pub fn now_us() -> u64 {
    // SAFETY: plain read.
    unsafe { owo_timing_to_us(owo_timing_stamp()) }
}

pub fn busy_wait_us(us: u32) {
    // SAFETY: k_busy_wait is callable from any context.
    unsafe { owo_busy_wait_us(us) }
}
