//! Thin Rust handles over Zephyr driver APIs that the `zephyr` crate does not wrap yet (SPI, DAC,
//! PWM, ADC), plus GPIO by `gpio_dt_spec`. Every handle is an opaque pointer to a statically
//! allocated devicetree spec or `struct device`, produced by the C shim (`shim/owo_shim.c`);
//! the shim does the actual Zephyr calls because most of them are static inline or syscall
//! wrappers that bindgen cannot see.
//!
//! All functions return `0` or a negative errno, like Zephyr.

#![no_std]

use core::ffi::{c_int, c_void};
use owo_hal::{Errno, OutputPin, SpiDevice};

extern "C" {
    fn owo_gpio_configure(g: *const c_void, active: c_int) -> c_int;
    fn owo_gpio_set(g: *const c_void, active: c_int) -> c_int;
    fn owo_spi_transceive(s: *const c_void, tx: *const u8, rx: *mut u8, len: usize) -> c_int;
    fn owo_dac_channel_setup(d: *const c_void, ch: u8, resolution: u8) -> c_int;
    fn owo_dac_write(d: *const c_void, ch: u8, value: u32) -> c_int;
    fn owo_pwm_set_percent(p: *const c_void, percent: u8) -> c_int;
    fn owo_adc_prime(a: *const c_void) -> c_int;
    fn owo_adc_read_polled(a: *const c_void, timeout_us: u32, out: *mut u16) -> c_int;
    fn owo_busy_wait_us(us: u32);
    fn owo_irq_lock() -> u32;
    fn owo_irq_unlock(key: u32);
    fn owo_suit_handle(kind: c_int) -> *const c_void;
}

fn check(r: c_int) -> Result<(), Errno> {
    if r < 0 {
        Err(r)
    } else {
        Ok(())
    }
}

/// Handle kinds understood by `owo_suit_handle` (see the shim).
#[derive(Clone, Copy)]
#[repr(i32)]
pub enum SuitHandle {
    BridgeHighA = 0,
    BridgeHighB = 1,
    BridgeLowA = 2,
    BridgeLowB = 3,
    SwitchLatch = 4,
    SupplyA = 5,
    SupplyB = 6,
    SwitchSpi = 7,
    Dac = 8,
    BoostPwm = 9,
    SenseAdc = 10,
}

/// The suit's devicetree node, if the board describes one. Null when the node is absent or its
/// device is not ready.
pub fn suit_handle(kind: SuitHandle) -> *const c_void {
    // SAFETY: pure lookup of a static handle; no side effects.
    unsafe { owo_suit_handle(kind as c_int) }
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

/// One ADC channel read with Zephyr's non-sleeping `adc_read_polled()` (see
/// `patches/zephyr/`).
pub struct AdcPolled(*const c_void);
// SAFETY: static spec.
unsafe impl Send for AdcPolled {}
impl AdcPolled {
    /// # Safety
    /// `p` must be a `struct adc_dt_spec *` with static lifetime, from the shim.
    pub unsafe fn from_raw(p: *const c_void) -> Self {
        Self(p)
    }
    /// Set the channel up and take one ordinary (sleeping) read, which programs the hardware.
    /// Call from thread context before the first [`AdcPolled::read`].
    pub fn prime(&mut self) -> Result<(), Errno> {
        // SAFETY: valid handle by construction.
        check(unsafe { owo_adc_prime(self.0) })
    }
    /// Raw 12-bit sample. Safe in interrupt context; gives up after `timeout_us`.
    pub fn read(&mut self, timeout_us: u32) -> Result<u16, Errno> {
        let mut v = 0u16;
        // SAFETY: `v` is a live u16 that the shim writes exactly once.
        check(unsafe { owo_adc_read_polled(self.0, timeout_us, &mut v) })?;
        Ok(v)
    }
}

pub fn busy_wait_us(us: u32) {
    // SAFETY: k_busy_wait is callable from any context.
    unsafe { owo_busy_wait_us(us) }
}

/// Run `f` with interrupts locked (Zephyr `irq_lock`/`irq_unlock`).
pub fn irq_locked<R>(f: impl FnOnce() -> R) -> R {
    // SAFETY: the key returned by lock is handed back to unlock exactly once.
    let key = unsafe { owo_irq_lock() };
    let r = f();
    // SAFETY: see above.
    unsafe { owo_irq_unlock(key) };
    r
}
