//! Glue only: every part has its own crate, every bus/pin goes through a Zephyr driver chosen in
//! devicetree (`owo,suit-output` node, see `dts/bindings/misc/owo,suit-output.yaml`).
//!
//! | part | crate | Zephyr driver used |
//! |------|-------|--------------------|
//! | H-bridge gates | `owo-hbridge` | GPIO |
//! | channel switch chain | `owo-switch` | SPI (+ a GPIO latch) |
//! | amplitude DAC | `ltc2634` | DAC API; the driver itself is the Rust one in `ltc2634::zephyr` |
//! | boost converter | (here) | PWM |
//! | PCF8575 expander lines (supply enables, DAC /CLR) | (devicetree) | `nxp,pcf8575` GPIO |
//! | feedback | (here) | ADC, via the non-sleeping `adc_read_polled()` |

#![no_std]

use owo_hal::OutputPin as _;
use owo_hbridge::{HBridge, State};
use owo_stim::hw::{Bridge, Hw, HwError};
use owo_switch::Switch;
use zephyr_io::{suit_handle, AdcPolled, Dac, Gpio, Pwm, Spi, SuitHandle};

/// Stock conversion constants (fw @0x44aa0 and the DAC routine); see docs/stock-stim-path.md.
const DAC_NUM: u32 = 0x12FED;
const DAC_DEN: u32 = 0x9C4;
const SENSE_NUM: u64 = 0xA_1220;
const SENSE_DEN: u64 = 0xEF_510A;
const SENSE_OFFSET: u32 = 3;
const DAC_ALL: u8 = ltc2634::ADDR_ALL;
/// Boost duty the stock firmware uses when the supply is on (fw @0x3afe2).
const BOOST_DUTY_PERCENT: u8 = 50;
const SENSE_TIMEOUT_US: u32 = 100;
const MAX_LEVEL: u8 = 126;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InitError {
    /// The board has no `owo,suit-output` node (or a device in it is not ready).
    NoDevice,
    Errno(i32),
}

pub struct Suit {
    bridge: HBridge<Gpio>,
    switch: Switch<Spi, Gpio>,
    dac: Dac,
    boost: Pwm,
    sense: AdcPolled,
    supply_a: Gpio,
    supply_b: Gpio,
    supply_on: bool,
}

fn gpio(kind: SuitHandle) -> Result<Gpio, InitError> {
    let p = suit_handle(kind);
    if p.is_null() {
        return Err(InitError::NoDevice);
    }
    // SAFETY: non-null handles from the shim are static gpio_dt_spec pointers.
    Ok(unsafe { Gpio::from_raw(p) })
}

fn ok(r: Result<(), i32>) -> Result<(), InitError> {
    r.map_err(InitError::Errno)
}

impl Suit {
    /// Bring the output stage up in its safe state: bridge off, switch open, DAC at zero, boost
    /// off. Call from thread context.
    pub fn new() -> Result<Self, InitError> {
        // Bridge first: high sides inactive, low sides active (= Off).
        let mut ha = gpio(SuitHandle::BridgeHighA)?;
        let mut hb = gpio(SuitHandle::BridgeHighB)?;
        let mut la = gpio(SuitHandle::BridgeLowA)?;
        let mut lb = gpio(SuitHandle::BridgeLowB)?;
        ok(ha.configure_output(false))?;
        ok(hb.configure_output(false))?;
        ok(la.configure_output(true))?;
        ok(lb.configure_output(true))?;
        let bridge = HBridge::new(ha, hb, la, lb).map_err(|_| InitError::Errno(-5))?;

        let mut supply_a = gpio(SuitHandle::SupplyA)?;
        let mut supply_b = gpio(SuitHandle::SupplyB)?;
        ok(supply_a.configure_output(false))?;
        ok(supply_b.configure_output(false))?;

        let mut latch = gpio(SuitHandle::SwitchLatch)?;
        ok(latch.configure_output(true))?;
        let sp = suit_handle(SuitHandle::SwitchSpi);
        let dp = suit_handle(SuitHandle::Dac);
        let pp = suit_handle(SuitHandle::BoostPwm);
        let ap = suit_handle(SuitHandle::SenseAdc);
        if sp.is_null() || dp.is_null() || pp.is_null() || ap.is_null() {
            return Err(InitError::NoDevice);
        }
        // SAFETY: non-null handles from the shim have the types named by their SuitHandle kind.
        let (spi, mut dac, mut boost, mut sense) = unsafe {
            (Spi::from_raw(sp), Dac::from_raw(dp), Pwm::from_raw(pp), AdcPolled::from_raw(ap))
        };
        ok(boost.set_percent(0))?;
        ok(dac.channel_setup(DAC_ALL, 12))?;
        ok(dac.write(DAC_ALL, 0))?;
        ok(sense.prime())?;
        let mut s = Self {
            bridge,
            switch: Switch::new(spi, latch),
            dac,
            boost,
            sense,
            supply_a,
            supply_b,
            supply_on: false,
        };
        s.switch.select(None).map_err(|_| InitError::Errno(-5))?;
        Ok(s)
    }
}

impl Hw for Suit {
    fn bridge(&mut self, s: Bridge) -> Result<(), HwError> {
        let to = match s {
            Bridge::Off => State::Off,
            Bridge::Positive => State::Positive,
            Bridge::Negative => State::Negative,
        };
        self.bridge.set(to).map_err(|e| match e {
            owo_hbridge::Error::Shoot => HwError::BridgeShoot,
            owo_hbridge::Error::Pin(_) => HwError::Bus,
        })
    }

    fn dac_level(&mut self, level: u8) -> Result<(), HwError> {
        if level > MAX_LEVEL {
            return Err(HwError::Bus);
        }
        let code = level as u32 * DAC_NUM / DAC_DEN;
        self.dac.write(DAC_ALL, code).map_err(|_| HwError::Bus)
    }

    fn select(&mut self, ch: Option<u8>) -> Result<(), HwError> {
        self.switch.select(ch).map_err(|e| match e {
            owo_switch::Error::Verify => HwError::SwitchVerify,
            _ => HwError::Bus,
        })
    }

    fn sense(&mut self) -> Result<u16, HwError> {
        let raw = self.sense.read(SENSE_TIMEOUT_US).map_err(|_| HwError::Bus)?;
        let v = (raw as u64 * SENSE_NUM / SENSE_DEN) as u32;
        Ok(v.saturating_sub(SENSE_OFFSET) as u16)
    }

    fn supply(&mut self, on: bool) -> Result<(), HwError> {
        if on == self.supply_on {
            return Ok(());
        }
        if on {
            // The stock firmware sets expander P2/P3 in normal operation (fw @0x24e94); purpose unknown.
            self.supply_a.set(true).map_err(|_| HwError::Bus)?;
            self.supply_b.set(true).map_err(|_| HwError::Bus)?;
            self.boost.set_percent(BOOST_DUTY_PERCENT).map_err(|_| HwError::Bus)?;
        } else {
            // Boost first so the rail collapses, then the enables.
            let r1 = self.boost.set_percent(0);
            let r2 = self.supply_a.set(false);
            let r3 = self.supply_b.set(false);
            r1.and(r2).and(r3).map_err(|_| HwError::Bus)?;
        }
        self.supply_on = on;
        Ok(())
    }

    fn delay_us(&mut self, us: u32) {
        zephyr_io::busy_wait_us(us);
    }

    fn critical<R>(&mut self, f: impl FnOnce(&mut Self) -> R) -> R {
        zephyr_io::irq_locked(|| f(self))
    }
}
