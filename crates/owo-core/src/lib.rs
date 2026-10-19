//! Portable core of the proof-of-concept OWO suit firmware.
//!
//! * [`proto`]  - packet parsing/encoding, derived from the 7.9.07 firmware handlers
//!   (addresses in comments are into that image) and the Android apps.
//! * [`engine`] - turns accepted frames into a pulse schedule. This is an
//!   *interpretation* of the frame fields, not a copy of the stock stimulation code.
//!
//! `no_std`, no allocation, no clock: callers pass a monotonic microsecond time in.
#![no_std]
#![forbid(unsafe_code)]

pub mod engine;
pub mod proto;
