//! Pin map recovered from fw 7.9.07's board init (0x3ae02) and helpers. Ports: A=0 B=1 C=2 D=3.

#[derive(Clone, Copy, Debug)]
pub struct Pin {
    pub port: u8,
    pub pin: u8,
}

const fn p(port: u8, pin: u8) -> Pin {
    Pin { port, pin }
}

pub struct Board {
    /// H-bridge drive pins (four gates; see `xg22` for the drive truth table).
    pub bridge: [Pin; 4],
    /// DAC SPI (bit-banged here; the stock firmware uses USART1): MOSI, MISO, SCLK, CS.
    pub dac_mosi: Pin,
    pub dac_miso: Pin,
    pub dac_sclk: Pin,
    pub dac_cs: Pin,
    /// Channel switch SPI (stock: USART0): MOSI, MISO, SCLK, CS.
    pub sw_mosi: Pin,
    pub sw_miso: Pin,
    pub sw_sclk: Pin,
    pub sw_cs: Pin,
    /// Boost converter PWM (TIMER1 CC0 in the stock firmware).
    pub boost_pwm: Pin,
    /// I2C to the PCF8575-type expander (bit-banged here): SDA, SCL; 8-bit write address.
    pub exp_sda: Pin,
    pub exp_scl: Pin,
    pub exp_addr: u8,
    /// Expander bit numbers (0..=15, low byte first).
    pub exp_dac_reset: u8,
    pub exp_supply_a: u8,
    pub exp_supply_b: u8,
}

/// The OWO suit as seen in firmware 7.9.07. Everything here is inferred from code.
pub const OWO_SUIT: Board = Board {
    bridge: [p(1, 0), p(1, 1), p(1, 2), p(1, 3)],
    dac_mosi: p(0, 0),
    dac_miso: p(0, 4),
    dac_sclk: p(0, 8),
    dac_cs: p(0, 5),
    sw_mosi: p(2, 0),
    sw_miso: p(2, 1),
    sw_sclk: p(2, 2),
    sw_cs: p(2, 3),
    boost_pwm: p(2, 7),
    exp_sda: p(1, 4),
    exp_scl: p(0, 7),
    exp_addr: 0x40,
    exp_dac_reset: 5,
    exp_supply_a: 2,
    exp_supply_b: 3,
};

/// Boost PWM frequency used at boot by the stock firmware (0x24b54: 0xfa0).
pub const BOOST_PWM_HZ: u32 = 4000;
/// Duty used when the stock firmware turns the supply on (0x3afe2: 0x32).
pub const BOOST_DUTY_PERCENT: u8 = 50;
