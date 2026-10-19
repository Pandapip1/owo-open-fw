//! EFR32xG22 backend for [`Hw`], for the board in [`crate::board::OWO_SUIT`].
//!
//! **Never run on hardware.** Everything here is derived from reading firmware 7.9.07
//! (`docs/stock-stim-path.md`); where the stock firmware used a peripheral (USART SPI, I2C)
//! this backend bit-bangs the same pins instead, to need nothing from Zephyr beyond clocks,
//! GPIO, one timer and the IADC.
//!
//! Safety properties of this layer (the supervisor adds its own on top):
//! * [`Xg22::new`] drives every output to its safe level *before* anything is enabled.
//! * The bridge is break-before-make in hardware order, and a direct positive<->negative
//!   request is refused with [`HwError::BridgeShoot`].
//! * Any error leaves the bridge in the "off" (both low sides on) state.

#![allow(clippy::missing_safety_doc)]

use core::ptr::{read_volatile, write_volatile};

use crate::board::{Board, Pin, BOOST_DUTY_PERCENT, BOOST_PWM_HZ};
use crate::hw::{Bridge, Hw, HwError};

#[allow(
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    dead_code,
    unused_imports,
    clippy::all,
    clippy::undocumented_unsafe_blocks
)]
mod sys {
    include!(concat!(env!("OUT_DIR"), "/emlib_bindings.rs"));
}
use sys::*;

/// Channel switch bytes for each protocol channel 0..=9 (index 10 is an extra entry the stock
/// firmware keeps for a test mode). Recovered by emulating the stock start-up code and reading
/// RAM 0x20000E6C after the .data/.bss initialisation. Each entry is sent as 4 bytes, MSB first.
pub const SWITCH_TABLE: [[u8; 4]; 11] = [
    [0x01, 0x00, 0x01, 0x00],
    [0x00, 0x10, 0x00, 0x10],
    [0x00, 0x40, 0x00, 0x40],
    [0x00, 0x01, 0x00, 0x01],
    [0x00, 0x04, 0x00, 0x04],
    [0x00, 0x80, 0x00, 0x80],
    [0x00, 0x02, 0x00, 0x02],
    [0x00, 0x08, 0x00, 0x08],
    [0x00, 0x20, 0x00, 0x20],
    [0x02, 0x00, 0x02, 0x00],
    [0x04, 0x00, 0x04, 0x00],
];

/// DAC code per level unit, from the stock fw (0x3b7fe): code = level * 0x12FED / 0x9C4.
const DAC_NUM: u32 = 0x12FED;
const DAC_DEN: u32 = 0x9C4;
/// Sense scaling from the stock fw (0x1a666): level = raw * 0xA1220 / 0xEF510A - 3.
const SENSE_NUM: u32 = 0xA_1220;
const SENSE_DEN: u32 = 0xEF_510A;
const SENSE_OFFSET: u32 = 3;

/// `sl_clock_branch_t` values, by position in the enum in sl_device_clock.h (anonymous-enum
/// constants are not exported by bindgen).
const BRANCH_SYSCLK: sl_clock_branch_t = 0;
const BRANCH_EM01GRPACLK: sl_clock_branch_t = 8;

/// PD3 is the stimulation feedback input (IADC pos input 0xB3 in the stock firmware).
const SENSE_PIN: Pin = Pin { port: 3, pin: 3 };

const DEMCR: *mut u32 = 0xE000_EDFC as *mut u32;
const DWT_CTRL: *mut u32 = 0xE000_1000 as *mut u32;
const DWT_CYCCNT: *mut u32 = 0xE000_1004 as *mut u32;

const TIMER1: *mut TIMER_TypeDef = TIMER1_BASE as *mut TIMER_TypeDef;
const IADC0: *mut IADC_TypeDef = IADC0_BASE as *mut IADC_TypeDef;
/// GPIO CDBUSALLOC (stock: 0x4003C328): routes port C/D analog pins to the ADC bus.
const GPIO_CDBUSALLOC: *mut u32 = (GPIO_BASE as usize + 0x328) as *mut u32;
const CDBUSALLOC_CDODD0_ADC0: u32 = 1 << 16;
/// GPIO TIMER1 route registers (stock: 0x4003C59C = ROUTEEN, +4 = CC0ROUTE).
const GPIO_TIMER1_ROUTEEN: *mut u32 = (GPIO_BASE as usize + 0x59C) as *mut u32;
const GPIO_TIMER1_CC0ROUTE: *mut u32 = (GPIO_BASE as usize + 0x5A0) as *mut u32;

pub struct Xg22 {
    b: &'static Board,
    bridge: Bridge,
    exp: u16,
    cycles_per_us: u32,
    timer_hz: u32,
    supply_on: bool,
}

fn gpio_mode(p: Pin, m: GPIO_Mode_TypeDef, out: bool) {
    // SAFETY: emlib call with a valid port/pin pair taken from the static board map.
    unsafe { GPIO_PinModeSet(p.port as GPIO_Port_TypeDef, p.pin as u32, m, out as u32) }
}
/// Port register block stride and the atomic SET/CLR aliases (stock fw: 0x4003D000 / 0x4003E000).
const PORT_STRIDE: usize = 0x30;
const SET_ALIAS: usize = 0x1000;
const CLR_ALIAS: usize = 0x2000;
const DOUT_OFF: usize = 0x10;
const DIN_OFF: usize = 0x14;

fn hi(p: Pin) {
    let a = GPIO_BASE as usize + SET_ALIAS + p.port as usize * PORT_STRIDE + DOUT_OFF;
    // SAFETY: GPIO DOUT set alias for a port/pin from the board map; a single atomic store.
    unsafe { write_volatile(a as *mut u32, 1 << p.pin) }
}
fn lo(p: Pin) {
    let a = GPIO_BASE as usize + CLR_ALIAS + p.port as usize * PORT_STRIDE + DOUT_OFF;
    // SAFETY: GPIO DOUT clear alias; a single atomic store.
    unsafe { write_volatile(a as *mut u32, 1 << p.pin) }
}
fn level(p: Pin, v: bool) {
    if v {
        hi(p)
    } else {
        lo(p)
    }
}
fn read(p: Pin) -> bool {
    let a = GPIO_BASE as usize + p.port as usize * PORT_STRIDE + DIN_OFF;
    // SAFETY: GPIO DIN register read.
    unsafe { read_volatile(a as *const u32) & (1 << p.pin) != 0 }
}

fn clock(c: *const u32) -> Result<(), HwError> {
    // SAFETY: `c` is one of the SL_BUS_CLOCK_*_VALUE statics exported by the device manager.
    if unsafe { sl_clock_manager_enable_bus_clock(c) } == 0 {
        Ok(())
    } else {
        Err(HwError::Bus)
    }
}

fn branch_hz(b: sl_clock_branch_t) -> Result<u32, HwError> {
    let mut f: u32 = 0;
    // SAFETY: `f` is a valid out-pointer for the duration of the call.
    if unsafe { sl_clock_manager_get_clock_branch_frequency(b, &mut f) } == 0
        && f != 0
    {
        Ok(f)
    } else {
        Err(HwError::Bus)
    }
}

impl Xg22 {
    /// Bring up the output stage in its safe state.
    ///
    /// # Safety
    /// Must be called at most once, and nothing else may touch GPIO ports A-D pins listed in
    /// `board`, TIMER1, IADC0 or the DWT cycle counter afterwards.
    pub unsafe fn new(board: &'static Board) -> Result<Xg22, HwError> {
        let [hs_a, hs_b, ls_a, ls_b] = board.bridge;

        // SAFETY: SL_BUS_CLOCK_* are address-of constants exported by sl_device_clock.
        clock(unsafe { &SL_BUS_CLOCK_GPIO_VALUE })?;

        // 1. Safe levels first: bridge "off" (both high sides low, both low sides on).
        for p in [hs_a, hs_b] {
            gpio_mode(p, gpioModePushPull, false);
        }
        for p in [ls_a, ls_b] {
            gpio_mode(p, gpioModePushPull, true);
        }
        gpio_mode(board.boost_pwm, gpioModePushPull, false);

        // 2. Buses idle: SPI mode 0 (clock low), chip selects high.
        for p in [board.dac_mosi, board.dac_sclk, board.sw_mosi, board.sw_sclk] {
            gpio_mode(p, gpioModePushPull, false);
        }
        for p in [board.dac_miso, board.sw_miso] {
            gpio_mode(p, gpioModeInput, false);
        }
        for p in [board.dac_cs, board.sw_cs] {
            gpio_mode(p, gpioModePushPull, true);
        }
        // I2C lines: open drain with pull-up, released.
        gpio_mode(board.exp_sda, gpioModeWiredAndPullUp, true);
        gpio_mode(board.exp_scl, gpioModeWiredAndPullUp, true);
        // Feedback pin: analog input.
        gpio_mode(SENSE_PIN, gpioModeDisabled, false);

        let cycles_per_us = branch_hz(BRANCH_SYSCLK)? / 1_000_000;

        // SAFETY: core debug registers; enabling the DWT cycle counter used for busy-waiting.
        unsafe {
            write_volatile(DEMCR, read_volatile(DEMCR) | (1 << 24));
            write_volatile(DWT_CTRL, read_volatile(DWT_CTRL) | 1);
        }
        let mut hw = Xg22 {
            b: board,
            bridge: Bridge::Off,
            // Stock shadow at boot: LEDs (P8..P13) off, P0..P7 low.
            exp: 0x3F00,
            cycles_per_us,
            timer_hz: 0,
            supply_on: false,
        };
        // The cycle counter must actually run, or every delay below would be wrong.
        let a = hw.cyc();
        hw.wait_cycles(64);
        if hw.cyc().wrapping_sub(a) < 32 || cycles_per_us == 0 {
            return Err(HwError::Bus);
        }

        hw.exp_write()?;
        // DAC /CLR: pulse low then release (stock: 0x3b7e0), then power-down state.
        hw.exp_set(board.exp_dac_reset, false)?;
        hw.delay_us(40);
        hw.exp_set(board.exp_dac_reset, true)?;
        hw.delay_us(200);
        hw.dac_raw(0)?;

        hw.timer_init()?;
        hw.adc_init()?;
        hw.select(None)?;
        Ok(hw)
    }

    // ---- timing ---------------------------------------------------------------------------

    fn cyc(&self) -> u32 {
        // SAFETY: read of the DWT cycle counter enabled in `new`.
        unsafe { read_volatile(DWT_CYCCNT) }
    }
    fn wait_cycles(&self, n: u32) {
        let s = self.cyc();
        while self.cyc().wrapping_sub(s) < n {}
    }

    // ---- bit-banged SPI, mode 0, MSB first ------------------------------------------------

    fn spi_byte(&self, mosi: Pin, miso: Pin, sclk: Pin, out: u8) -> u8 {
        let half = self.cycles_per_us; // ~500 kHz
        let mut inb = 0u8;
        for i in (0..8).rev() {
            level(mosi, (out >> i) & 1 != 0);
            self.wait_cycles(half / 2 + 1);
            hi(sclk);
            inb = (inb << 1) | read(miso) as u8;
            self.wait_cycles(half);
            lo(sclk);
            self.wait_cycles(half / 2 + 1);
        }
        inb
    }

    fn dac_raw(&mut self, code16: u16) -> Result<(), HwError> {
        // LTC2634-style 24-bit frame: command/address 0x3F (write+update all), then data.
        let b = self.b;
        lo(b.dac_cs);
        self.wait_cycles(self.cycles_per_us);
        for byte in [0x3F, (code16 >> 8) as u8, code16 as u8] {
            self.spi_byte(b.dac_mosi, b.dac_miso, b.dac_sclk, byte);
        }
        self.wait_cycles(self.cycles_per_us);
        hi(b.dac_cs);
        Ok(())
    }

    fn sw_write(&mut self, bytes: [u8; 4]) -> [u8; 4] {
        let b = self.b;
        let mut back = [0u8; 4];
        lo(b.sw_cs);
        self.wait_cycles(self.cycles_per_us);
        for (i, v) in bytes.iter().enumerate() {
            back[i] = self.spi_byte(b.sw_mosi, b.sw_miso, b.sw_sclk, *v);
        }
        self.wait_cycles(self.cycles_per_us);
        hi(b.sw_cs);
        back
    }

    /// Clock bytes through the switch's shift register without touching CS.
    fn sw_shift_only(&mut self, bytes: [u8; 4]) -> [u8; 4] {
        let b = self.b;
        let mut back = [0u8; 4];
        for (i, v) in bytes.iter().enumerate() {
            back[i] = self.spi_byte(b.sw_mosi, b.sw_miso, b.sw_sclk, *v);
        }
        back
    }

    // ---- bit-banged I2C to the PCF8575-class expander --------------------------------------

    fn i2c_delay(&self) {
        self.wait_cycles(self.cycles_per_us * 5 / 2 + 1); // ~100 kHz
    }
    fn i2c_start(&self) {
        let b = self.b;
        hi(b.exp_sda);
        hi(b.exp_scl);
        self.i2c_delay();
        lo(b.exp_sda);
        self.i2c_delay();
        lo(b.exp_scl);
    }
    fn i2c_stop(&self) {
        let b = self.b;
        lo(b.exp_sda);
        self.i2c_delay();
        hi(b.exp_scl);
        self.i2c_delay();
        hi(b.exp_sda);
        self.i2c_delay();
    }
    /// Returns true if the slave ACKed.
    fn i2c_write(&self, v: u8) -> bool {
        let b = self.b;
        for i in (0..8).rev() {
            level(b.exp_sda, (v >> i) & 1 != 0);
            self.i2c_delay();
            hi(b.exp_scl);
            self.i2c_delay();
            lo(b.exp_scl);
        }
        hi(b.exp_sda); // release for ACK
        self.i2c_delay();
        hi(b.exp_scl);
        self.i2c_delay();
        let ack = !read(b.exp_sda);
        lo(b.exp_scl);
        ack
    }
    fn exp_write(&mut self) -> Result<(), HwError> {
        self.i2c_start();
        let ok = self.i2c_write(self.b.exp_addr)
            && self.i2c_write(self.exp as u8)
            && self.i2c_write((self.exp >> 8) as u8);
        self.i2c_stop();
        if ok {
            Ok(())
        } else {
            Err(HwError::Bus)
        }
    }
    fn exp_set(&mut self, bit: u8, v: bool) -> Result<(), HwError> {
        let m = 1u16 << bit;
        let n = if v { self.exp | m } else { self.exp & !m };
        if n == self.exp {
            return Ok(());
        }
        self.exp = n;
        self.exp_write()
    }

    // ---- boost PWM (TIMER1 CC0 -> PC7, 4 kHz, 50 %) ----------------------------------------

    fn timer_init(&mut self) -> Result<(), HwError> {
        // SAFETY: SL_BUS_CLOCK_TIMER1_VALUE is exported by the device manager.
        clock(unsafe { &SL_BUS_CLOCK_TIMER1_VALUE })?;
        self.timer_hz = branch_hz(BRANCH_EM01GRPACLK)?;
        let top = self.timer_hz / BOOST_PWM_HZ;
        if top < 2 || top > 0xFFFF {
            return Err(HwError::Bus);
        }
        let init = TIMER_Init_TypeDef {
            enable: false,
            prescale: timerPrescale1,
            clkSel: timerClkSelHFPerClk,
            mode: timerModeUp,
            ..Default::default()
        };
        let cc = TIMER_InitCC_TypeDef {
            mode: timerCCModePWM,
            cofoa: timerOutputActionSet,
            cmoa: timerOutputActionClear,
            ..Default::default()
        };
        // SAFETY: TIMER1 is owned exclusively by this driver (contract of `new`); the route
        // registers are the ones the stock firmware writes (GPIO TIMERROUTE[1]).
        unsafe {
            TIMER_Init(TIMER1, &init);
            TIMER_InitCC(TIMER1, 0, &cc);
            write_volatile(&raw mut (*TIMER1).TOP, top - 1);
            write_volatile(&raw mut (*TIMER1).CC[0].OC, 0);
            write_volatile(GPIO_TIMER1_CC0ROUTE, ((self.b.boost_pwm.pin as u32) << 16) | self.b.boost_pwm.port as u32);
            write_volatile(GPIO_TIMER1_ROUTEEN, 1);
        }
        Ok(())
    }

    fn boost(&mut self, on: bool) {
        let duty = if on { BOOST_DUTY_PERCENT as u32 } else { 0 };
        // SAFETY: TIMER1 registers, owned by this driver.
        unsafe {
            let top = read_volatile(&raw const (*TIMER1).TOP) + 1;
            write_volatile(&raw mut (*TIMER1).CC[0].OC, top * duty / 100);
            if on {
                write_volatile(&raw mut (*TIMER1).EN, 1);
                write_volatile(&raw mut (*TIMER1).CMD, 1); // START
            } else {
                write_volatile(&raw mut (*TIMER1).CMD, 2); // STOP
                write_volatile(&raw mut (*TIMER1).CNT, 0);
                write_volatile(GPIO_TIMER1_ROUTEEN, 0);
                lo(self.b.boost_pwm);
                write_volatile(GPIO_TIMER1_ROUTEEN, 1);
            }
        }
    }

    // ---- feedback ADC (IADC0 single conversion on PD3) ---------------------------------------

    fn adc_init(&mut self) -> Result<(), HwError> {
        // SAFETY: SL_BUS_CLOCK_IADC0_VALUE is exported by the device manager.
        clock(unsafe { &SL_BUS_CLOCK_IADC0_VALUE })?;
        let mut init = IADC_Init_t::default();
        init.warmup = iadcWarmupNormal;
        // SAFETY: IADC0 is owned exclusively by this driver. Config mirrors the stock firmware
        // (0x1a5ec): 20 MHz source, 10 MHz ADC clock, normal mode, 2x OSR, gain 1x, VDDX
        // reference at 3300 mV.
        unsafe {
            init.srcClkPrescale = IADC_calcSrcClkPrescale(IADC0, 20_000_000, 0);
            let mut all = IADC_AllConfigs_t::default();
            let c = &mut all.configs[0];
            c.adcMode = iadcCfgModeNormal;
            c.osrHighSpeed = iadcCfgOsrHighSpeed2x;
            c.analogGain = iadcCfgAnalogGain1x;
            c.reference = iadcCfgReferenceVddx;
            c.vRef = 3300;
            c.adcClkPrescale = IADC_calcAdcClkPrescale(IADC0, 10_000_000, 0, iadcCfgModeNormal, init.srcClkPrescale);
            IADC_init(IADC0, &init, &all);

            let single = IADC_InitSingle_t {
                alignment: iadcAlignRight12,
                dataValidLevel: iadcFifoCfgDvl1,
                triggerSelect: iadcTriggerSelImmediate,
                triggerAction: iadcTriggerActionOnce,
                ..Default::default()
            };
            let input = IADC_SingleInput_t {
                negInput: iadcNegInputGnd,
                posInput: iadcPosInputPortDPin3,
                configId: 0,
                compare: false,
            };
            IADC_initSingle(IADC0, &single, &input);
            // Route PD3 (odd) to ADC0 through the C/D analog bus.
            write_volatile(GPIO_CDBUSALLOC, read_volatile(GPIO_CDBUSALLOC) | CDBUSALLOC_CDODD0_ADC0);
        }
        Ok(())
    }
}

impl Hw for Xg22 {
    fn bridge(&mut self, s: Bridge) -> Result<(), HwError> {
        let [hs_a, hs_b, ls_a, ls_b] = self.b.bridge;
        if self.bridge == s {
            return Ok(());
        }
        // Positive and negative are never connected directly.
        if self.bridge != Bridge::Off && s != Bridge::Off {
            lo(hs_a);
            lo(hs_b);
            self.bridge = Bridge::Off;
            // Low sides to the off state too so the error leaves a known state.
            hi(ls_a);
            hi(ls_b);
            return Err(HwError::BridgeShoot);
        }
        // Break-before-make: high sides off first, then set the low sides, then the new high side.
        lo(hs_a);
        lo(hs_b);
        match s {
            Bridge::Off => {
                hi(ls_a);
                hi(ls_b);
            }
            Bridge::Positive => {
                lo(ls_a);
                hi(ls_b);
                hi(hs_a);
            }
            Bridge::Negative => {
                hi(ls_a);
                lo(ls_b);
                hi(hs_b);
            }
        }
        self.bridge = s;
        Ok(())
    }

    fn dac_level(&mut self, level: u8) -> Result<(), HwError> {
        if level > 126 {
            return Err(HwError::Bus);
        }
        let code = (level as u32 * DAC_NUM / DAC_DEN) << 4;
        self.dac_raw(code as u16)
    }

    fn select(&mut self, ch: Option<u8>) -> Result<(), HwError> {
        // Same protocol as the stock firmware (0x444c8): shift 4 bytes in with CS low, raise CS
        // (latch), then -- with CS left HIGH -- clock 4 zero bytes and compare what comes back
        // with what was written. CS is a latch-enable here: pulsing it again after shifting
        // zeros through would latch zeros and switch the channel off.
        let Some(c) = ch else {
            self.sw_write([0; 4]);
            return Ok(());
        };
        let want = *SWITCH_TABLE.get(c as usize).ok_or(HwError::Bus)?;
        self.sw_write(want);
        let back = self.sw_shift_only([0; 4]);
        if back == want {
            Ok(())
        } else {
            self.sw_write([0; 4]); // make sure nothing stays routed
            Err(HwError::SwitchVerify)
        }
    }

    fn sense(&mut self) -> Result<u16, HwError> {
        // SAFETY: IADC0 registers/FIFO, owned by this driver.
        unsafe {
            write_volatile(&raw mut (*IADC0).CMD, 1); // SINGLESTART
            // Bounded by wall time (not iterations): this runs with interrupts locked and
            // the bridge energised, so it must give up quickly.
            let start = self.cyc();
            let limit = 100 * self.cycles_per_us; // 100 us
            loop {
                let st = read_volatile(&raw const (*IADC0).STATUS);
                if st & 0x140 == 0x100 {
                    break;
                }
                if self.cyc().wrapping_sub(start) > limit {
                    return Err(HwError::Bus);
                }
            }
            let r = IADC_pullSingleFifoResult(IADC0);
            let v = (r.data as u64 * SENSE_NUM as u64 / SENSE_DEN as u64) as u32;
            Ok(v.saturating_sub(SENSE_OFFSET) as u16)
        }
    }

    fn supply(&mut self, on: bool) -> Result<(), HwError> {
        if on == self.supply_on {
            return Ok(());
        }
        if on {
            // P2/P3 are set in normal operation in the stock firmware (0x24e94); purpose unknown.
            self.exp_set(self.b.exp_supply_a, true)?;
            self.exp_set(self.b.exp_supply_b, true)?;
            self.boost(true);
        } else {
            self.boost(false);
        }
        self.supply_on = on;
        Ok(())
    }

    fn delay_us(&mut self, us: u32) {
        self.wait_cycles(us.saturating_mul(self.cycles_per_us));
    }

    fn critical<R>(&mut self, f: impl FnOnce(&mut Self) -> R) -> R {
        let primask: u32;
        // SAFETY: save PRIMASK and disable interrupts; restored below on every path.
        unsafe {
            core::arch::asm!("mrs {0}, primask", "cpsid i", out(reg) primask, options(nomem, nostack));
        }
        let r = f(self);
        // SAFETY: restores the PRIMASK value saved above.
        unsafe {
            core::arch::asm!("msr primask, {0}", in(reg) primask, options(nomem, nostack));
        }
        r
    }
}
