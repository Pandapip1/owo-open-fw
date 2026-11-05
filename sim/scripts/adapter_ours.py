# Scenario adapter for the open firmware built with CONFIG_OWO_SIM_INJECT (src/sim_inject.rs).
#
# Injection boundary: the GATT write callback (src/ble.rs write_cb) and the disconnect
# callback, entered from the system work queue through the simulation mailbox, exactly as
# the Bluetooth host would call them after an ATT write request passed its permission check.
import gdb

# src/ble.rs `Id` values of the writable characteristics.
IDS = {"widths": 1, "run": 2, "frame": 3, "power": 4, "config": 6, "test": 9, "ota": 11}
KIND_DISCONNECT = 0x100
# An erased flash word region past the application in the composed image (sim/README.md):
# "wfi; b.n .-2" is written there for the hang scenarios.
HANG_LOOP = 0x4B000
HANG_LOOP_CODE = (0xBF30, 0xE7FD)
# The same with interrupts masked by the core itself (gdb cannot write the Secure bank of
# PRIMASK/BASEPRI through QEMU's stub): "cpsid i; b ." and "movs r0, #lvl; msr basepri, r0; b .".
HANG_IRQOFF = HANG_LOOP + 0x10
HANG_IRQOFF_CODE = (0xB672, 0xE7FE)
HANG_IRQLOCK = HANG_LOOP + 0x20
THUMB_MOVS_R0_IMM8 = 0x2000
THUMB2_MSR_R0 = 0xF380
THUMB2_MSR_SYSM_BASEPRI = 0x8811
THUMB_B_SELF = 0xE7FE


# src/sim_inject.rs `Mailbox` (repr(C)): seq, done, kind, len, result (u32/i32), data[32].
MB_SEQ, MB_DONE, MB_KIND, MB_LEN, MB_RESULT, MB_DATA = 0, 4, 8, 12, 16, 20


def _mb():
    return int(gdb.parse_and_eval("(unsigned int)&owo_sim_mailbox"))


def _word(addr):
    return int(gdb.parse_and_eval(f"*(unsigned int *){addr:#x}"))


def _post(kind, data=b"", wait=True):
    mb = _mb()
    seq = _word(mb + MB_SEQ) + 1
    gdb.execute(f"set *(unsigned int *){mb + MB_KIND:#x} = {kind}")
    gdb.execute(f"set *(unsigned int *){mb + MB_LEN:#x} = {len(data)}")
    for i, b in enumerate(data):
        gdb.execute(f"set *(unsigned char *){mb + MB_DATA + i:#x} = {b}")
    gdb.execute(f"set *(unsigned int *){mb + MB_SEQ:#x} = {seq}")
    if not wait:
        return None
    wp = gdb.Breakpoint(f"*(unsigned int *){mb + MB_DONE:#x}", gdb.BP_WATCHPOINT,
                        gdb.WP_WRITE, internal=True)
    try:
        while _word(mb + MB_DONE) != seq:
            gdb.execute("continue")
            gdb.execute("thread 1", to_string=True)
    finally:
        wp.delete()
    return int(gdb.parse_and_eval(f"*(int *){mb + MB_RESULT:#x}"))


def setup():
    pass


def prep():
    return "nothing to prepare"


def command(name, data, wait=True):
    return _post(IDS[name], data, wait)


def disconnect():
    # 0x13: remote user terminated connection (Core spec Vol 1 Part F).
    return _post(KIND_DISCONNECT, bytes([0x13]))


def _write_code(addr, halves):
    for i, half in enumerate(halves):
        gdb.execute(f"set {{unsigned short}}{addr + 2 * i} = {half:#x}")


def _write_loop():
    _write_code(HANG_LOOP, HANG_LOOP_CODE)
    _write_code(HANG_IRQOFF, HANG_IRQOFF_CODE)
    basepri = ZLI_LEVELS << (8 - NVIC_PRIO_BITS)
    _write_code(HANG_IRQLOCK, (THUMB_MOVS_R0_IMM8 | basepri, THUMB2_MSR_R0,
                               THUMB2_MSR_SYSM_BASEPRI, THUMB_B_SELF))


# The supervisor reads its phase clock (owo_counter_since_us) in every pass of the phase loop,
# and also while waiting for a conversion with the bridge open; the condition picks a read with
# high side A (PB0, active high in boards/owo_suit_hw.overlay) driven: the positive phase.
# GPIO port B is at 0x5003C030, its DOUT at +0x10 (CMSIS GPIO_PORT_TypeDef).
GPIO_PB_DOUT = 0x5003C030 + 0x10
HS_A = 1 << 0


def mid_phase_break():
    bp = gdb.Breakpoint("owo_counter_since_us", internal=True, temporary=True)
    bp.condition = f"(*(unsigned int *){GPIO_PB_DOUT:#x} & {HS_A:#x}) != 0"


def hang(kind):
    _write_loop()
    if kind == "irqs-off":
        # A wedged core: interrupts masked, spinning in place. Peripherals and WDOG0 run on.
        gdb.execute(f"set $pc = {HANG_IRQOFF:#x}")
        return "pc=hang-loop primask=1"
    if kind in ("mid-pulse", "thread-mid-pulse"):
        # The pump thread stops inside a positive phase: at a phase-loop clock read with the
        # bridge's high side A driven (MID_PHASE_BREAK).
        bp = mid_phase_break()
        gdb.execute("continue")
        gdb.execute(f"set $pc = {HANG_LOOP:#x}")
        return "pump thread spinning mid-phase, interrupts enabled"
    if kind == "mid-pulse-irqs-off":
        # As mid-pulse, with every configurable interrupt masked (PRIMASK): no warning
        # interrupt can run, only the watchdog reset ends it.
        mid_phase_break()
        gdb.execute("continue")
        gdb.execute(f"set $pc = {HANG_IRQOFF:#x}")
        return "pump spinning mid-phase, primask=1"
    if kind == "pump-loop":
        # The pump wedges between pulses, interrupts enabled: stopped where it is about to
        # check the feed condition (src/wdt.rs feed -> owo_suit_output_quiescent).
        gdb.Breakpoint("owo_suit_output_quiescent", internal=True, temporary=True)
        gdb.execute("continue")
        gdb.execute(f"set $pc = {HANG_LOOP:#x}")
        return "pump thread spinning before its feed, interrupts enabled"
    if kind == "mid-pulse-irq-lock":
        # As mid-pulse, but inside irq_lock(): BASEPRI masks every interrupt but the
        # zero-latency levels (Zephyr ARMv8-M Mainline; CONFIG_ZERO_LATENCY_LEVELS).
        mid_phase_break()
        gdb.execute("continue")
        basepri = ZLI_LEVELS << (8 - NVIC_PRIO_BITS)
        gdb.execute(f"set $pc = {HANG_IRQLOCK:#x}")
        return f"pump spinning mid-phase under irq_lock (BASEPRI {basepri:#x})"
    raise ValueError(kind)


# EFR32BG22: 3 NVIC priority bits (__NVIC_PRIO_BITS in the CMSIS device header); the product
# configures two zero-latency levels (CONFIG_ZERO_LATENCY_LEVELS in its .config).
NVIC_PRIO_BITS = 3
ZLI_LEVELS = 2


def poke(addr, size, value):
    gdb.execute(f"set {{unsigned {'char' if size == 1 else 'short' if size == 2 else 'int'}}}"
                f"{addr} = {value}")


def radio_shutdown(kind=None):
    # The radio's deadline-miss safe shutdown with its (empty) miss record, through the kernel's
    # report as a missed deadline reaches it (modules/efr32-radio/hal/radio.c
    # k_deadline_miss_handler, inlined into z_deadline_miss_report under LTO). Needs an image
    # whose miss policy is the radio's handler (CONFIG_SCHED_DEADLINE_MISS_CUSTOM: the hardware
    # timing; sim/product-qemu.conf only counts misses). "mid-pulse": called inside a positive
    # phase on the pump thread, as a failure detected in an interrupt during a pulse would be.
    if kind == "mid-pulse":
        mid_phase_break()
        gdb.execute("continue")
        gdb.execute("thread 1", to_string=True)
    # LTO may or may not give the static miss record a ".lto_priv.N" name.
    for call in ("call (void)k_deadline_miss_handler(&efr32_radio_miss)",
                 "call (void)z_deadline_miss_report(&efr32_radio_miss)",
                 "call (void)z_deadline_miss_report(&'efr32_radio_miss.lto_priv.0')"):
        try:
            gdb.execute(call)
            return call
        except gdb.error:
            continue
    raise gdb.error("no callable radio safe shutdown in this image")


# src/sim_inject.rs `Outcomes` (repr(C), u32 fields in this order) and the fault kinds of
# `note_outcome`.
OUTCOME_FIELDS = ("fired", "measured", "faults", "fault_kind", "fault_a", "fault_b", "skip_kind")
FAULT_KINDS = {1: "Cancelled", 2: "Hw", 3: "Overdrive", 4: "UnderDelivery", 5: "SenseNotZero",
               6: "SenseTimeout", 7: "PhaseOverrun", 8: "BatteryLow", 9: "Unmeasured"}


def outcomes():
    base = int(gdb.parse_and_eval("(unsigned int)&owo_sim_outcomes"))
    out = {f: _word(base + 4 * i) for i, f in enumerate(OUTCOME_FIELDS)}
    out["fault"] = FAULT_KINDS.get(out["fault_kind"])
    return out


def status():
    out = {}
    for expr in ("*(int *)((unsigned int)&owo_sim_mailbox + 16)", "efr32_radio_halted",
                 "'efr32_radio_halted.lto_priv.0'"):
        try:
            out[expr] = int(gdb.parse_and_eval(expr))
        except gdb.error:
            pass
    try:
        out["outcomes"] = outcomes()
    except gdb.error:
        pass
    try:
        out["pc"] = int(gdb.parse_and_eval("$pc"))
    except gdb.error:
        pass
    return out
