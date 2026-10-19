//! `owo-stim`: the stimulation safety supervisor.
//!
//! * [`hw::Hw`] is the narrow hardware interface (bridge, DAC, channel switch, sense ADC,
//!   boost supply, delays, interrupt lock). Nothing above it knows about pins or buses.
//! * [`stim::Stimulator`] owns every safety decision and is unit-tested on the host against a
//!   mock.
//!
//! The per-part drivers live in their own crates (`owo-hbridge`, `owo-switch`, `ltc2634`) and
//! `owo-suit` wires them to Zephyr's driver APIs and implements [`hw::Hw`].
//! The sequencing comes from reverse engineering firmware 7.9.07 (`docs/stock-stim-path.md`);
//! none of it has run on hardware.

#![cfg_attr(not(test), no_std)]

pub mod hw;
pub mod stim;
