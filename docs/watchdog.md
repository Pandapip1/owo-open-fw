# Watchdog

**Unvalidated on hardware. Bench-test on a dummy load first.** Every timing below was measured
in the QEMU suit board model (`-M efr32bg22,board=owo-suit`, `-icount shift=3`), not on silicon.
Evidence tags as in `stock-stim-path.md`: **[F]** from code or the reference manual (RM),
**[E]** observed in emulation, **[I]** inferred, **[U]** unknown.

## Configuration: fixed at boot, locked

`src/watchdog.c` (`CONFIG_OWO_WATCHDOG`, on in every product build) owns WDOG0 with raw
register writes (CMSIS device header). Zephyr's `wdt_gecko` driver is no longer used: it can
select neither the warning, nor the energy-mode bits, nor the lock, and it re-configures at run
time. The configuration is written once, at `PRE_KERNEL_2` (10.3 ms after reset in the model,
after the bootloader and the SoC's clock setup), and never again:

| Field | Value | Why |
|---|---|---|
| Clock | the devicetree's WDOG0 branch: LFRCO on the suit (precision mode), LFXO on the kit, 32768 Hz both; `clock_control_get_rate` is checked against devicetree, else `k_panic` | as stock (WDOG0 on the LFRCO, `suit-board.md`) [F]; ULFRCO (1 kHz) would make each CMD synchronisation 3-4 ms (RM 4.2.4.4.4) and the 4-cycle clear rule 4 ms (RM 27.5.4) |
| PERSEL | 8 (`CONFIG_OWO_WATCHDOG_PERSEL`): T = (2^11 + 1) / 32768 Hz = **62.53 ms** | the shortest period that covers the measured feed intervals with margin (below); equal to stock |
| WARNSEL | 3 (`CONFIG_OWO_WATCHDOG_WARNSEL`): warning at 75 % = **46.90 ms** after a clear | for a given longest feed interval, the latest warning gives the shortest period, so the reset backstop (the only bound when the warning cannot run) is as short as possible; the 15.6 ms between warning and reset is ample for the warning's register writes |
| EM2RUN, EM3RUN | 1 | below (energy modes) |
| DEBUGRUN | 1 (product); 0 only with `CONFIG_OWO_WATCHDOG_FREEZE_IN_DEBUG`, which cannot be combined with the real output stage | below (debugging) |
| CLRSRC | 0 (software clear) | feeding certifies software supervision (below); a PRS clear would certify a hardware signal instead |
| WINSEL, PRS0/1MISSRSTEN, WDOGRSTDIS, EM4BLOCK | 0 | no window: the pump may feed early (a WAKE from a GATT write ends its sleep); EM4 is not used and the watchdog does not run in EM4 (RM 27.3.3), so EM4 may only ever follow the ordered safe-off |
| IEN | WARN | the warning interrupt (below) |

Then `WDOG0_LOCK` gets the lock key and `CMU_WDOGLOCK` any key but its unlock key: EN, CFG and
the clock selection can no longer be changed (RM 27.5.8, 8.5.4) [F]. The lock guards against
stray writes, not a deliberate unlock: writing the unlock key (0xABE8, resp. 0x93F7) unlocks
again, and `EMU_RSTCTRL.WDOG0RMODE` can still block the watchdog's reset (RM 12.5.14) [F]. Both
are single specific values no code path in this firmware writes.

CFG is a CONFIG register: writing it while EN is set is a BusFault (RM 4.2.4.4.2) [F]. CMD is
an LFSYNC register: a clear is pending for 3-4 watchdog-clock cycles (92-122 us), and a second
CMD write while `SYNCBUSY.CMD` is set is a BusFault (RM 4.2.4.4.4) [F]. `owo_wdt_feed` therefore
skips the write while a clear is pending: that clear restarts the count when it completes.

Resets: a watchdog, lockup or `SYSRESETREQ` reset "resets the entire device except some EMU
registers" (RM 12.5.14 `EMU_RSTCTRL`), so WDOG0 starts disabled and unlocked after every reset,
including the OTA reboot into the Gecko bootloader and AppLoader, which never enable it [F; E:
after the OTA reboot in the model no WDOG0 event follows]. `EMU_RSTCTRL.LOCKUPRMODE` resets to
0 (lockup does not reset); the watchdog is then the bound on a locked-up core [F].

## Who feeds, and what a feed certifies

Only the pump feeds (`src/wdt.rs`), once per loop iteration **after its supervision pass**:
every pulse that was due has gone through the supervisor (the zero check and each phase with its in-phase sense,
over- and under-delivery and overrun checks) and the supervisor's dead-man poll has run for the current time.
`wdt::feed` grants the feed only if

1. every live lease's safety predicate holds (an armed session holds one: not resetting), and
2. the output stage is quiescent: no bridge high side is driven on (`owo_suit_output_quiescent`
   reads the GPIO output latches named by the devicetree). Between passes the bridge must be
   off; a closed bridge outside a supervised phase means supervision has already failed.

A refused feed makes the pump revoke the output resource and run its ordered safe-off; it does
not feed, so if that safe-off does not complete the warning and the reset follow. A feed
therefore means "the output-safety supervisor finished a pass and left the output off", not
"some thread ran". The leases no longer start or stop anything: the watchdog cannot be stopped.

The pump bounds its sleep to half the warning time, 23.45 ms (`cap_sleep`), whether or not a
session is armed (armed sessions also wake every `CONFIG_OWO_STIM_MONITOR_INTERVAL_MS` = 10 ms).
Boot has two more feeds under the same condition (after the output baseline and after
`bt_enable`); the OTA path feeds after its reset-preparation safe-off and then through its
200 ms settling window (`wdt::sleep_fed`); the "baseline unverified" halt keeps feeding while
the output stays quiescent (a reset loop would cross the reset window of floating pins again
and again, `stim-simulation.md` finding 8).

## Choosing the period: measured feed intervals [E]

Interval = time between consecutive clears as WDOG0 completes them (model trace event
`efr32_wdog ... clear`; `scratch/wdog/feeds.py` in a work tree). Runs and their raw traces are
kept with the stock dump (`wdog-2026-10-31/`, the flash images contain the stock bootloader).

| Workload | Image | Run | Longest interval |
|---|---|---|---|
| Boot: enable to first feed | default / `-DOWO_HW=ON` | every run | 1.4 ms / 4.8 ms |
| Boot incl. `bt_enable` and P-256 key generation on the long work queue | both, PM off | first 700 ms | 23.97 ms |
| Advertising, idle, 116 GATT writes (mailbox, `write_cb`) over 60 s | default, PM off | 2545 intervals | **23.98 ms** |
| Advertising, idle | default, PM on | until the timer stall (below) | 23.72 ms |
| Armed stimulation, soft start, 3 pulses | `-DOWO_HW=ON`, PM off | 4.0-4.10 s, until the timer stall | 13.98 ms (10.1 ms without a pulse) |
| OTA control point write: safe-off, 200 ms settle, `bt_disable`, reboot | default, PM off | | 23.69 ms; `bt_disable` and the last feed 0.39 ms after the window |

The longest legitimate interval is the sleep cap (23.45 ms) plus one wake-up and pass: at most
0.53 ms idle and 3.98 ms with a pulse in the model. QEMU runs one instruction per 8 ns
(125 MIPS); the M33 at 76.8 MHz executes at most 76.8 MIPS, so CPU-bound parts take at least
1.63x as long on silicon [I]; doubling them gives 24.5 ms idle and 18 ms armed. Against the
warning at 46.90 ms that is a margin of 1.9x (22 ms) idle and 2.6x armed.

PERSEL 7 (T = 31.28 ms, warning 23.46 ms) would need the sleep cap halved to 11.7 ms, which
doubles the idle wake rate and leaves 1.3x (armed, after scaling) — not enough for the
workloads that could not be measured. So **PERSEL 8, WARNSEL 3**: the warning, and with it the
safe state, at most 46.9 ms after the last feed; the reset at most 62.5 ms after it.

Not measured (each needs a fix outside this change; see "Blockers"): a live connection with GATT
writes over the air, SMP pairing (DHKey and the AES users), the EM2 wake-up latency on
silicon, stimulation beyond the first soft-start pulses, and anything with the radio's deadline
safe shutdown followed by a working kernel. Flash writes: none exist at run time
(`CONFIG_BT_SETTINGS=n`, nothing writes flash) [F].

## The warning interrupt: output safe with raw register writes

WDOG0's IRQ is connected as a direct, zero-latency interrupt at priority 0 (`IRQ_ZERO_LATENCY`;
the product already has two zero-latency levels): it runs even inside `irq_lock()` (BASEPRI on
ARMv8-M Mainline), never enters the kernel and never returns. `owo_suit_force_safe_from_isr`
(modules/owo-stim/shim/owo_shim.c) makes only register writes, all derived from the devicetree
(`owo,suit-output` node), in the order of `Stimulator::safe_off`:

1. Bridge, through the GPIO port's DOUT SET/CLR aliases: both high sides off first, then both
   low sides on — the board's safe levels (`suit-board.md`, the hogs in the board dts, the C
   reset safe-off): break before make, never a same-leg pair on.
2. Boost PWM: TIMER compare buffer 0 if the timer is enabled (duty 0 from the next period, as
   `pwm_set` with pulse 0).
3. Expander (PCF857x on I2C0): the transfer the interrupted thread may own is aborted, then one
   port write: supply enables at their inactive level, the DAC's /CLR asserted (amplitude to
   zero scale), every other pin 1 (input/weak high, what the driver leaves on pins it does not
   drive). On the suit that is 0xFFD3. Its waits end with the reset at the latest.

It then spins until the reset, so nothing below it (a thread that was merely late) can re-energise
the output. When the warning cannot run — interrupts masked with PRIMASK, a fault handler, a
lockup, a debug halt — the reset 15.6 ms later is the bound.

## Energy modes

The configuration cannot change at run time, so the watchdog's energy-mode bits hold whether or
not output may be energised. **EM2RUN = EM3RUN = 1**: if the CPU reached EM2 with the bridge
closed (a pump blocked mid-phase on something that sleeps, and the idle thread enters EM2), a
frozen watchdog would leave the output energised indefinitely; running, its warning (an EM2
wake-up source, RM 12.3) forces it safe. The price is the idle wake-up: the pump wakes every
~23.6 ms (42 per second) even with nothing to do. Per wake the model runs 0.2-0.5 ms of CPU;
at the datasheet's 27 uA/MHz x 76.8 MHz = 2.1 mA, doubled for silicon instruction timing, that
is of the order of 40 uA average, plus the EM2 exit and re-entry cost (HFXO/DPLL restart),
which is not measured [I]. EM2 itself is 1.75 uA (LFRCO precision mode, datasheet). Accepted:
a safety bound that depends on the CPU staying awake is not a bound; the suit's output stage,
when used, dominates its consumption. The watchdog does not run in EM4 (RM 27.3.3); nothing
enters EM4.

## Debugging

DEBUGRUN = 1 in the product: a debug halt does not stop the watchdog. A halted core cannot run
the warning, so the reset (at most 62.5 ms after the last feed) is the only bound on an output a
halt leaves energised; with DEBUGRUN = 0 a halt mid-pulse would hold the bridge closed for as
long as the debugger waits. For debugging without a real output stage,
`CONFIG_OWO_WATCHDOG_FREEZE_IN_DEBUG=y` sets DEBUGRUN = 0 (`depends on
!OWO_STIM_UNVALIDATED_HARDWARE`). A QEMU gdbstub stop is not a debug halt: it stops virtual
time, so scenario pauses never reach the watchdog.

## Fault scenarios in the model [E]

Hang at the start of a pulse (soft start, 0.40 mA in the model), PM-off diagnostic image,
last feed 4031.28 ms (`stim-simulation.md` has the matrix):

| Hang | Bridge live | Output at the warning | Reset |
|---|---|---|---|
| Pump spins mid-phase, interrupts on | 34.9 ms, until the warning 46.9 ms after the last feed | shorted-off, DAC 0, expander 0xFFD3 (supplies off, /CLR), boost stopped | 62.5 ms after the last feed |
| Same inside `irq_lock()` (BASEPRI 0x40) | 34.9 ms | same: the zero-latency warning still runs | 62.5 ms |
| Same with PRIMASK set | 50.5 ms, until the reset | the warning cannot run | 62.5 ms |
| Pump spins between pulses (supply on) | none | supplies off, boost stopped | 62.5 ms |
| PRIMASK between pulses | none | (output already off) | 62.5 ms |

Before this change the first row lasted 2047 ms. At 8 mA the warning bound is 0.38 mC of one
polarity at most, the PRIMASK case 0.5 mC, the same as stock's 62.5 ms.

## QEMU model (vendor/qemu `hw/arm/efr32bg22.c`, qtests in `efr32bg22-board-test`, `-dap-test`)

Implements, with RM citations in the source: CFG BusFault while enabled; CMD as LFSYNC
(`SYNCBUSY.CMD`, clear on completion after 4 cycles, BusFault on a second write and on
disabling while pending); LOCK on EN and CFG; `CMU_WDOGLOCK` on the clock selection and
`CMU_STATUS.WDOGLOCK`; the warning at WARNSEL quarters with IF as an INTFLAG (a clear does not
clear IF.WARN); DEBUGRUN against the M33 debug unit's halt state. EM2RUN/EM3RUN = 1 is what the
model does in every sleep (it does not distinguish sleep depths); enabling with either bit 0 is
logged as unimplemented. Trace events: `config`, `enable`, `lock`, `clear` (with the count),
`warning`, `timeout`, `bus-error`, `freeze`, `thaw`.

## Blockers (outside this change, found while measuring) [E]

- **Kernel timeouts stop in the model with PM on** (resolved since: a 630 s PM-on run with stimulation fed the watchdog with a longest interval of 24.50 ms, `stim-simulation.md` Results). Was: the default image (no output stage, no
  radio safe shutdown) stops waking the pump about 600 ms after boot: the pump and the 1 ms
  mailbox work item both sit in the timeout list, the idle thread runs, nothing fires. Same
  image with `CONFIG_PM=n`: no stall over 60 s. Before this change the pump slept without a
  timeout while idle, which hid it. With the watchdog this is a boot loop (warning, reset).
- **Timeouts late while the long work queue runs P-256 key generation (PM on, `-DOWO_HW=ON`):**
  the pump (priority 0) stays pended past its timeout while the long work queue (priority 10)
  computes; the longest interval was 288 ms, and afterwards timeouts arrived only with the
  100 ms advertising events. With PM off the pump preempts key generation on time. Both match
  the boot-time deadline miss in `stim-simulation.md`.
- **Kernel stall after the boot-time radio safe shutdown (`-DOWO_HW=ON`, PM off):** feeds stop
  at 4103 ms, once stimulation has started; with the miss report bypassed (diagnostic) they
  stop ~425 ms after boot instead.
- Connections cannot be driven against the product image: the QEMU central discovers a
  test-only characteristic, and the model runs the connection turnarounds past their
  start-by deadlines, which the product answers with the radio safe shutdown.

All measurements above therefore come from PM-off diagnostic images where PM-on ones fail.
Until those are fixed, a product image in the model resets about every 0.5-0.6 s.
