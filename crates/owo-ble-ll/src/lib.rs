//! Open Bluetooth LE link layer, peripheral role, for the OWO radio firmware.
//!
//! Hardware agnostic: everything that touches the radio goes through the [`radio::Radio`] trait,
//! so the same code runs against the QEMU radio model, a host test double and (later) the
//! EFR32BG22 radio. `no_std`, no allocation, `forbid(unsafe_code)`.
//!
//! Section references are to the Bluetooth Core Specification v5.2 (Vol 6 Part B unless stated);
//! test vectors are cited from the file they were taken from. NOTHING here has been verified on
//! air: see docs/radio-firmware-plan.md ("What is not verified").
#![no_std]
#![cfg_attr(not(test), warn(missing_docs))]

pub mod chan;
pub mod conn;
pub mod crc;
pub mod hci;
pub mod llcp;
pub mod peripheral;
pub mod pdu;
pub mod radio;
pub mod whiten;
