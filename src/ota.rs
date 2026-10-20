//! Over-the-air update: the Silicon Labs bootloader (kept in flash below the application) does the
//! transfer; this side only has to be asked to reboot into it, as the stock firmware does when the
//! OTA control point is written (see docs/ota.md).
//!
//! The "reboot into the bootloader" request is Zephyr's generic boot-mode retention API
//! (`zephyr,boot-mode` in boards/<board>.overlay): the board's devicetree places the retention
//! area where the bootloader looks for it and sets the bytes it expects as the area's prefix.

use core::ffi::c_int;
use core::sync::atomic::{AtomicBool, Ordering};

extern "C" {
    // include/zephyr/retention/bootmode.h
    fn bootmode_set(boot_mode: u8) -> c_int;
    // include/zephyr/sys/reboot.h
    fn sys_reboot(kind: c_int) -> !;
}

/// `BOOT_MODE_TYPE_BOOTLOADER` in bootmode.h.
const BOOT_MODE_BOOTLOADER: u8 = 1;
/// `SYS_REBOOT_COLD` in reboot.h.
const REBOOT_COLD: c_int = 1;
/// Time for the ATT write response to leave before the radio goes away.
const SETTLE_MS: u64 = 200;

static REQUESTED: AtomicBool = AtomicBool::new(false);

/// Ask for a reboot into the bootloader (callable from the GATT callback).
pub(crate) fn request() {
    REQUESTED.store(true, Ordering::SeqCst);
}

/// Called by the pump with the output already off. Does not return if a request is pending.
pub(crate) fn reboot_if_requested() {
    if !REQUESTED.load(Ordering::SeqCst) {
        return;
    }
    // SAFETY: plain kernel/retention calls; the boot-mode device is a board requirement.
    unsafe {
        if bootmode_set(BOOT_MODE_BOOTLOADER) != 0 {
            // Cannot record the request: stay running rather than reboot into the application again.
            REQUESTED.store(false, Ordering::SeqCst);
            return;
        }
    }

    // The ATT handler has returned by now. Keep the radio alive only for the
    // response settling window, with output already de-energised; shutdown then
    // runs in the pump's thread context. If Bluetooth teardown wedges, the
    // watchdog remains armed and resets the SoC.
    crate::prepare_reset();
    // Output is already off, but teardown can still wedge before reset.  This
    // local lease keeps reset containment active until sys_reboot() consumes it.
    let _teardown_watchdog = crate::wdt::Lease::acquire(None).ok();
    zephyr::time::sleep(zephyr::time::Duration::millis_at_least(SETTLE_MS));
    crate::ble::shutdown();

    // SAFETY: reset preparation has put the output stage in its safe state.
    unsafe {
        sys_reboot(REBOOT_COLD)
    }
}
