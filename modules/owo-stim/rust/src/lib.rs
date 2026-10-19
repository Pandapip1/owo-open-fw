//! `owo-stim`: output-stage driver and safety supervisor for the OWO suit hardware.
//!
//! Layering:
//! * [`hw::Hw`] is the narrow hardware interface (bridge, DAC, channel switch, sense ADC,
//!   boost supply, delays, interrupt lock). Nothing above it knows about pins.
//! * [`stim::Stimulator`] owns every safety decision and is hardware independent, so it is
//!   unit-tested on the host against a mock.
//! * `xg22` (feature `xg22`) implements `Hw` for the EFR32xG22 board the stock firmware
//!   targets. Pin map: [`board`].
//!
//! The pin map and sequencing come from reverse engineering firmware 7.9.07
//! (see `docs/stock-stim-path.md`). None of it has run on hardware.

#![cfg_attr(not(test), no_std)]
#![forbid(unsafe_op_in_unsafe_fn)]

pub mod board;
pub mod hw;
pub mod stim;

#[cfg(feature = "xg22")]
pub mod xg22;
