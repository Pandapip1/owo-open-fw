//! QEMU integration workloads for the EFR32 lower-link-layer port.
//!
//! The default image is the scanner-only smoke; `CONFIG_RADIO_RX_PORT_CENTRAL`
//! selects the connection-establishment workload instead.
#![no_std]

use zephyr_panic as _;

#[cfg(not(CONFIG_RADIO_RX_PORT_CENTRAL))]
mod observer;
#[cfg(CONFIG_RADIO_RX_PORT_CENTRAL)]
mod central;
