//! TEST ONLY (`CONFIG_OWO_SIM_INJECT`): a RAM mailbox through which the QEMU board simulation
//! commands the firmware without a Bluetooth link (docs/stim-simulation.md).
//!
//! A debugger fills `owo_sim_mailbox` and then increments `seq`; `owo_sim_poll` (1 ms system
//! work item, src/sim_inject.c) hands the entry over and copies `seq` to `done`.
//! * `kind` 1..=11: a GATT write of `data[..len]` to the characteristic with that `Id`
//!   (1 widths, 2 run, 3 frame, 4 power, 6 config): `ble::write_cb` runs exactly as for an
//!   ATT write request that passed the stack's permission check. `result` is its return.
//! * `kind` 0x100: the Bluetooth disconnect callback with reason `data[0]`.
//!
//! Skipped compared with a real write: the link, ATT and SMP (pairing, the authenticated-write
//! permission), and the notifications' delivery (they are queued to no connection).

use core::ptr::{addr_of, addr_of_mut, read_volatile, write_volatile};

pub const KIND_DISCONNECT: u32 = 0x100;
const DATA_LEN: usize = 32;

#[repr(C)]
pub struct Mailbox {
    pub seq: u32,
    pub done: u32,
    pub kind: u32,
    pub len: u32,
    pub result: i32,
    pub data: [u8; DATA_LEN],
}

#[no_mangle]
#[used]
#[allow(non_upper_case_globals)]
pub static mut owo_sim_mailbox: Mailbox =
    Mailbox { seq: 0, done: 0, kind: 0, len: 0, result: 0, data: [0; DATA_LEN] };

#[no_mangle]
extern "C" fn owo_sim_poll() {
    // SAFETY: the mailbox is only written by the debugger while this work item is not
    // looking at it (the debugger bumps `seq` last), and only read here.
    unsafe {
        let m = addr_of_mut!(owo_sim_mailbox);
        let seq = read_volatile(addr_of!((*m).seq));
        if seq == read_volatile(addr_of!((*m).done)) {
            return;
        }
        let kind = read_volatile(addr_of!((*m).kind));
        let len = (read_volatile(addr_of!((*m).len)) as usize).min(DATA_LEN);
        let data = read_volatile(addr_of!((*m).data));
        let result = if kind == KIND_DISCONNECT {
            crate::ble::sim_disconnect(data[0]);
            0
        } else {
            crate::ble::sim_write(kind as usize, &data[..len]) as i32
        };
        write_volatile(addr_of_mut!((*m).result), result);
        write_volatile(addr_of_mut!((*m).done), seq);
    }
}
