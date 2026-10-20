//! Glue only. Every part has its own crate or is a stock Zephyr driver chosen in devicetree
//! (`owo,suit-output` node, see `dts/bindings/misc/owo,suit-output.yaml`); every board-specific
//! number comes from that node, none lives here.
//!
//! | part | logic | Zephyr driver used |
//! |------|-------|--------------------|
//! | H-bridge gates | `owo-hbridge` | GPIO |
//! | channel switch | `owo-switch` (channel -> pin mask) | GPIO port; `ti,sn74hc595` (with read-back verification) |
//! | amplitude DAC | here (scaling) | DAC API; `ltc2634` is the Rust driver for the part |
//! | supply enables | here | regulator (`regulator-fixed`) |
//! | boost converter | here | PWM |
//! | feedback | here (scaling) | ADC async sequence; timestamps from the timing API |
//! | phase widths | here | Counter API (a 32-bit counter with <= 1 us ticks, running only while the supply is on) |

#![no_std]

use owo_hal::Switchable as _;
use owo_hbridge::{HBridge, State};
use owo_stim::hw::{Bridge, Hw, HwError, Sample};
use owo_switch::Switch;
use zephyr_io::{
    now_us, suit_handle, suit_param, suit_post, suit_supply_handle, suit_switch_routes, Dac, Gpio, Port, Pwm,
    FineCounter, Regulator, Sampler, SuitHandle, SuitParam,
};

/// The DAC driver's entry points are called only from C (`drivers/dac/dac_ltc2634.c`); this keeps
/// the linker from dropping the Rust driver object because nothing in Rust refers to it.
#[cfg(not(test))]
#[used]
static LTC2634_DRIVER: (
    extern "C" fn(u8, u8, bool) -> i32,
    unsafe extern "C" fn(*const core::ffi::c_void, *const core::ffi::c_void) -> i32,
    unsafe extern "C" fn(*const core::ffi::c_void, u8, u32) -> i32,
) = (
    ltc2634::zephyr::ltc2634_rs_channel_setup,
    ltc2634::zephyr::ltc2634_rs_init,
    ltc2634::zephyr::ltc2634_rs_write_value,
);

/// Sampling interval while the supply is on; see CONFIG_OWO_STIM_SENSE_INTERVAL_US.
const SENSE_INTERVAL_US: u32 = 100;
/// Maximum number of supply regulators the binding allows.
const MAX_SUPPLIES: usize = 4;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InitError {
    /// The board has no `owo,suit-output` node (or a device in it is not ready).
    NoDevice,
    Errno(i32),
}

/// Board constants, read once from devicetree.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Scale {
    pub dac_channel: u8,
    pub dac_resolution: u8,
    pub dac_num: u32,
    pub dac_den: u32,
    pub max_level: u8,
    pub uv_per_level: u32,
    pub sense_offset: u32,
}

impl Scale {
    fn from_dt() -> Self {
        Self {
            dac_channel: suit_param(SuitParam::DacChannel) as u8,
            dac_resolution: suit_param(SuitParam::DacResolution) as u8,
            dac_num: suit_param(SuitParam::DacScaleNum),
            dac_den: suit_param(SuitParam::DacScaleDen),
            max_level: suit_param(SuitParam::MaxLevel) as u8,
            uv_per_level: suit_param(SuitParam::SenseUvPerLevel),
            sense_offset: suit_param(SuitParam::SenseOffset),
        }
    }

    /// DAC code for a commanded level, `None` if above the board's range.
    pub fn dac_code(&self, level: u8) -> Option<u32> {
        if level > self.max_level || self.dac_den == 0 {
            return None;
        }
        Some((level as u64 * self.dac_num as u64 / self.dac_den as u64) as u32)
    }

    /// Commanded-level units for a pin voltage in millivolts.
    pub fn level_from_mv(&self, mv: i32) -> u16 {
        if mv <= 0 || self.uv_per_level == 0 {
            return 0;
        }
        let lv = (mv as u64 * 1000 / self.uv_per_level as u64).saturating_sub(self.sense_offset as u64);
        lv.min(u16::MAX as u64) as u16
    }
}

pub struct Suit {
    bridge: HBridge<Gpio>,
    switch: Switch<'static, Port>,
    dac: Dac,
    boost: Pwm,
    sense: Sampler,
    fine: FineCounter,
    supplies: [Option<Regulator>; MAX_SUPPLIES],
    scale: Scale,
    boost_duty: u8,
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
    /// Run stage 1's non-energising driver POST, then stage 2's forced
    /// shutdown initialization. The forced sequence runs even if POST fails
    /// so a brown-out cannot turn a missing acknowledgement into skipped
    /// recovery. Callers must not start safety-critical work unless this
    /// returns the constructed, known-shutdown driver set.
    pub fn post_and_shutdown() -> Result<Self, InitError> {
        let post = suit_post().map_err(InitError::Errno);
        let shutdown = Self::new();

        match (post, shutdown) {
            (Ok(()), Ok(suit)) => Ok(suit),
            (Err(err), _) | (_, Err(err)) => Err(err),
        }
    }

    /// Bring the output stage up in its safe state: bridge off, switch open, DAC at zero, boost
    /// off. This is the forced initialization half of [`post_and_shutdown`].
    fn new() -> Result<Self, InitError> {
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

        let n = suit_param(SuitParam::SupplyCount) as usize;
        if n == 0 || n > MAX_SUPPLIES {
            return Err(InitError::NoDevice);
        }
        let mut supplies: [Option<Regulator>; MAX_SUPPLIES] = [None, None, None, None];
        for (i, slot) in supplies.iter_mut().enumerate().take(n) {
            let p = suit_supply_handle(i as u32);
            if p.is_null() {
                return Err(InitError::NoDevice);
            }
            // SAFETY: non-null supply handles from the shim are ready regulator devices.
            *slot = Some(unsafe { Regulator::from_raw(p) });
        }

        let sw = suit_handle(SuitHandle::Switch);
        let dp = suit_handle(SuitHandle::Dac);
        let pp = suit_handle(SuitHandle::BoostPwm);
        let ap = suit_handle(SuitHandle::SenseAdc);
        let fp = suit_handle(SuitHandle::PhaseTimer);
        if sw.is_null() || dp.is_null() || pp.is_null() || ap.is_null() || fp.is_null() {
            return Err(InitError::NoDevice);
        }
        // SAFETY: non-null handles from the shim have the types named by their SuitHandle kind.
        let (port, mut dac, mut boost, sense, fine) = unsafe {
            (
                Port::from_raw(sw),
                Dac::from_raw(dp),
                Pwm::from_raw(pp),
                Sampler::from_raw(ap),
                FineCounter::from_raw(fp),
            )
        };
        let scale = Scale::from_dt();
        ok(boost.set_percent(0))?;
        ok(dac.channel_setup(scale.dac_channel, scale.dac_resolution))?;
        ok(dac.write(scale.dac_channel, 0))?;
        let mut s = Self {
            bridge,
            switch: Switch::new(port, suit_switch_routes()),
            dac,
            boost,
            sense,
            fine,
            supplies,
            scale,
            boost_duty: suit_param(SuitParam::BoostDutyPercent) as u8,
            supply_on: false,
        };
        s.shutdown()?;
        Ok(s)
    }

    /// Put every output-owning driver in its known shutdown state and verify
    /// that its driver call accepted the request.  This is intentionally not
    /// the best-effort emergency path: startup must not proceed into normal
    /// service unless every component has acknowledged its safe state.
    pub fn shutdown(&mut self) -> Result<(), InitError> {
        let bridge = self.bridge.off().map_err(|_| InitError::Errno(-5));
        let dac = self
            .dac
            .write(self.scale.dac_channel, 0)
            .map_err(InitError::Errno);
        let switch = self.switch.select(None).map_err(|_| InitError::Errno(-5));

        // These calls are infallible at the Rust interface, but they must
        // still run before PWM/regulator shutdown even when an earlier driver
        // rejected its command.
        self.sense.stop();
        self.fine.stop();
        let boost = self.boost.set_percent(0).map_err(InitError::Errno);
        let supplies = self.supplies_set(false).map_err(InitError::Errno);
        self.supply_on = false;

        bridge.or(dac).or(switch).or(boost).or(supplies)
    }

    fn supplies_set(&mut self, on: bool) -> Result<(), i32> {
        let mut r = Ok(());
        for s in self.supplies.iter_mut().flatten() {
            if let Err(e) = s.set(on) {
                r = Err(e);
            }
        }
        r
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
        let code = self.scale.dac_code(level).ok_or(HwError::Bus)?;
        self.dac.write(self.scale.dac_channel, code).map_err(|_| HwError::Bus)
    }

    fn select(&mut self, ch: Option<u8>) -> Result<(), HwError> {
        self.switch.select(ch).map_err(|e| match e {
            owo_switch::Error::Verify => HwError::SwitchVerify,
            _ => HwError::Bus,
        })
    }

    fn now_us(&mut self) -> u64 {
        now_us()
    }

    fn fine_mark(&mut self) -> u32 {
        self.fine.mark()
    }

    fn fine_since_us(&mut self, mark: u32) -> u32 {
        self.fine.since_us(mark)
    }

    fn sense_latest(&mut self) -> Option<Sample> {
        let (_raw, mv, at_us) = self.sense.latest()?;
        Some(Sample { value: self.scale.level_from_mv(mv), at_us })
    }

    fn wait_first_sample(&mut self, timeout_us: u32) -> bool {
        self.sense.wait_first(timeout_us)
    }

    fn supply(&mut self, on: bool) -> Result<(), HwError> {
        if on && self.supply_on {
            return Ok(());
        }
        if on {
            let r = self
                .supplies_set(true)
                .and_then(|_| self.boost.set_percent(self.boost_duty))
                .and_then(|_| self.fine.start())
                .and_then(|_| self.sense.start(SENSE_INTERVAL_US));
            if r.is_ok() {
                self.supply_on = true;
                return Ok(());
            }
            // Fall through and switch everything back off.
        }
        // Always runs fully when asked to turn off (also after a failed turn-on).
        self.sense.stop();
        self.fine.stop();
        // Boost first so the rail collapses, then the enables.
        let r1 = self.boost.set_percent(0);
        let r2 = self.supplies_set(false);
        self.supply_on = false;
        let off = r1.and(r2).map_err(|_| HwError::Bus);
        if on {
            Err(HwError::Bus)
        } else {
            off
        }
    }

    fn delay_us(&mut self, us: u32) {
        zephyr_io::busy_wait_us(us);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const STOCK: Scale = Scale {
        dac_channel: 15,
        dac_resolution: 12,
        dac_num: 0x12FED,
        dac_den: 0x9C4,
        max_level: 126,
        uv_per_level: 19150,
        sense_offset: 3,
    };

    #[test]
    fn dac_code_matches_stock_endpoint() {
        assert_eq!(STOCK.dac_code(126), Some(3921));
        assert_eq!(STOCK.dac_code(0), Some(0));
        assert_eq!(STOCK.dac_code(127), None);
    }

    #[test]
    fn sense_scale_matches_stock_formula_closely() {
        // stock: raw*660000/15683850 - 3 with raw 12-bit over 3.3 V (0.8057 mV/count)
        for raw in [0u32, 100, 1000, 4095] {
            let mv = (raw as u64 * 3300 / 4095) as i32;
            let stock = (raw as u64 * 0xA1220 / 0xEF510A).saturating_sub(3);
            let ours = STOCK.level_from_mv(mv) as u64;
            assert!(ours.abs_diff(stock) <= 1, "raw {raw}: {ours} vs {stock}");
        }
    }

    #[test]
    fn negative_mv_is_zero() {
        assert_eq!(STOCK.level_from_mv(-5), 0);
    }
}
