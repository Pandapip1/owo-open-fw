# Scenario adapter for the open firmware built with CONFIG_OWO_SIM_INJECT (src/sim_inject.rs).
#
# Injection boundary: the GATT write callback (src/ble.rs write_cb) and the disconnect
# callback, entered from the system work queue through the simulation mailbox, exactly as
# the Bluetooth host would call them after an ATT write request passed its permission check.
import gdb

# src/ble.rs `Id` values of the writable characteristics.
IDS = {"widths": 1, "run": 2, "frame": 3, "power": 4, "config": 6, "test": 9}
KIND_DISCONNECT = 0x100
# An erased flash word region past the application in the composed image (sim/README.md):
# "wfi; b.n .-2" is written there for the hang scenarios.
HANG_LOOP = 0x4B000
HANG_LOOP_CODE = (0xBF30, 0xE7FD)


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


def _write_loop():
    for i, half in enumerate(HANG_LOOP_CODE):
        gdb.execute(f"set {{unsigned short}}{HANG_LOOP + 2 * i} = {half:#x}")


def hang(kind):
    _write_loop()
    if kind == "irqs-off":
        # A wedged core: interrupts masked, spinning in place. Peripherals and WDOG0 run on.
        gdb.execute("set $primask = 1")
        gdb.execute(f"set $pc = {HANG_LOOP:#x}")
        return "pc=hang-loop primask=1"
    if kind in ("mid-pulse", "thread-mid-pulse"):
        # The pump thread stops right after closing the bridge for a phase
        # (owo_counter_mark is the phase's first timestamp, taken after bridge(pol)).
        bp = gdb.Breakpoint("owo_counter_mark", internal=True, temporary=True)
        gdb.execute("continue")
        gdb.execute(f"set $pc = {HANG_LOOP:#x}")
        return "pump thread spinning mid-phase, interrupts enabled"
    raise ValueError(kind)


def poke(addr, size, value):
    gdb.execute(f"set {{unsigned {'char' if size == 1 else 'short' if size == 2 else 'int'}}}"
                f"{addr} = {value}")


def radio_shutdown():
    # The radio's deadline-miss safe shutdown with an empty miss record, as the scheduler
    # calls it (modules/efr32-radio/hal/radio.c k_deadline_miss_handler).
    gdb.execute("call (void)k_deadline_miss_handler(&efr32_radio_miss)")
    return "k_deadline_miss_handler called"


def status():
    out = {}
    for expr in ("*(int *)((unsigned int)&owo_sim_mailbox + 16)", "efr32_radio_halted"):
        try:
            out[expr] = int(gdb.parse_and_eval(expr))
        except gdb.error:
            pass
    try:
        out["pc"] = int(gdb.parse_and_eval("$pc"))
    except gdb.error:
        pass
    return out
