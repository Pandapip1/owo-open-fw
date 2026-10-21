# One Zephyr scheduler across active and deep-sleep clocks

## Decision

The radio must not retain a private deadline scheduler.  RX, TX, TIFS, and
unrelated real-time work are ordinary persistent Zephyr jobs: an interrupt may
make a job ready, but it must neither run the job nor own a second timeout
queue.  The long-term implementation is one Zephyr timeout queue with a
power-aware system-timer driver:

| CPU/power state | Clock-event backend | Purpose |
| --- | --- | --- |
| Active / EM1 | PROTIMER | high-resolution next-deadline interrupt |
| EM2 / EM3 / EM4-capable idle | BURTC | retained low-power wake alarm |

The system timer, rather than a Bluetooth driver, chooses and changes the
clock-event backend.  It presents one monotonic kernel tick stream and one
`sys_clock_set_timeout()` implementation.  The scheduler remains the sole
owner of runnable-job selection.

This document is a design contract, not a claim that the current partial
PROTIMER model implements it.

## Why BURTC alone is insufficient for a reactive 150-us deadline

On the BG22, the current BURTC driver deliberately enforces a six LF-clock
cycle lead before a `COMP` change (`MIN_DELAY_CYC = 6` in
`vendor/zephyr/drivers/counter/counter_silabs_burtc.c:28-31`).  At 32,768 Hz
that is 183.1 us.  A 32,768-Hz Zephyr tick would improve normal kernel timeout
granularity to 30.5 us (the current tests use 1,024 Hz, or 976.6 us), but does
not remove the six-cycle write-visibility floor.  A deadline discovered only
when a packet ends cannot therefore be programmed through BURTC for BLE's
150-us TIFS.

This is a hardware synchronization constraint, not a reason for an RX/TX
special case.  The active backend must be a clock-event source with a safely
shorter arm latency.  The existing `silabs,protimer` Counter interface reports
a 1-MHz timebase (`RAIL_TIMER_FREQ_HZ` in
`vendor/zephyr/drivers/counter/counter_silabs_protimer.c:25-30`).  It is the
candidate active backend, subject to proving its compare latency and ownership
rules on hardware.

The stock facts support treating PROTIMER as the active radio-clock candidate,
but not yet treating it as a complete timer specification:

- `flash-stock.bin:0x29974..0x29a36` is the observed setup window; its first
  traced writes include `PROTIMER +0x008 = 0x00011102`, `+0x030 = 0x1233`,
  `+0x038 = 0x80015558`, and `+0x00c = 1`
  (`docs/radio-firmware-plan.md:41`).
- The stock IRQ vector dispatches IRQ36 through the shared radio handler;
  archive offsets `rfhal_protimer.o:0x527c0..0x527cc` read `+0x014` and clear
  the observed flag bank (`docs/radio-tables.md:973-981`).
- The current RAIL Counter wrapper is not safe to make the kernel timer: its
  Kconfig explicitly says that RAIL must initialize first and owns/enables the
  radio clock (`vendor/zephyr/drivers/counter/Kconfig.silabs:11-23`).

In particular, this does **not** infer an autonomous PROTIMER-to-RAC TIFS
route.  That route remains unproven.

## Driver architecture

1. Make 32,768 Hz the candidate fixed kernel tick rate only after the
   clock-event driver supports it.  It is a logical unit, not a promise of
   32,768 interrupts per second: tickless mode still arms only the earliest
   timeout.  The kernel documentation defines this setting as the minimum
   timing granularity in tickless mode (`vendor/zephyr/kernel/Kconfig:728-746`).
   The current BG22 configuration is 1,024 Hz, and the current BURTC driver
   intentionally rejects a kernel tick shorter than its six-cycle arm lead
   (`counter_silabs_burtc.c:458-465`).  Therefore simply changing Kconfig is
   not an implementation of this design.
2. Keep a protected logical-clock state: last announced kernel tick, fractional
   LF remainder, selected backend, and an epoch sample for each hardware
   counter.  Both backend ISRs convert their raw count to that same logical
   timeline before calling `sys_clock_announce()`.  They never rebase or reset
   the kernel timeout queue.
3. In active mode, translate the kernel's next tick deadline to a PROTIMER
   absolute compare from the active epoch.  In idle entry, snapshot the active
   counter and logical time while holding `sys_clock_lock()`, disable its
   compare, translate the same deadline with ceiling arithmetic to BURTC, and
   arm BURTC.  On wake, acknowledge BURTC, sample both clocks, account elapsed
   *whole* logical ticks exactly once, select PROTIMER, then program its next
   compare.  A generation number attached to each arm rejects a stale IRQ
   from the backend just relinquished.
4. `sys_clock_idle_enter()` and `sys_clock_idle_exit()` are precisely the
   upstream system-timer hooks intended for handing a deadline to a low-power
   wake timer (`vendor/zephyr/include/zephyr/drivers/timer/system_timer.h:288-313`).
   Zephyr calls the former under `sys_clock_lock()` after subtracting declared
   PM exit latency (`vendor/zephyr/subsys/pm/pm.c:232-245`).  The driver must
   additionally reject an idle/deep-state hand-off whenever the next deadline
   is inside BURTC's synchronization lead plus oscillator/PM-exit budget.
5. The active backend must be initialized before general scheduling and must
   not depend on RAIL.  Therefore it needs a small direct PROTIMER clock-event
   driver or a validated always-available hardware interface; wrapping
   `sl_rail_get_time()` is not sufficient.  QEMU must model its counter,
   compare, flag/enable and IRQ36 semantics before it is selected for real
   guest scheduling.

`sys_clock_cycle_get_32()` must retain a stable declared frequency.  Switching
the value it reports between 32-kHz and 1-MHz units would break scheduler
deadlines and public cycle APIs.  The existing runtime-frequency helper only
publishes a changed frequency and explicitly asks timer drivers to rescale
their state (`vendor/zephyr/kernel/sys_clock_hw_cycles.c:13-40`); it is not a
safe clocksource hand-off mechanism.  Use a fixed logical timebase plus private
backend conversion instead.

## Scheduling and response bounds

At a clock interrupt, the bounded ISR only acknowledges the event, advances
the logical clock, calls `sys_clock_announce()`, and lets normal Zephyr
rescheduling select among ready persistent jobs.  TX, RX, and TIFS jobs can
use ordinary priority/deadline configuration.  The kernel documentation is
explicit that deadline ordering does not prove a job will meet a deadline
(`vendor/zephyr/include/zephyr/kernel.h:1114-1141`).  Admission control,
priority assignment, and bounded job work remain required.

Even a correct high-resolution scheduler cannot guarantee a *newly discovered*
150-us radio turnaround: a packet-end IRQ leaves less than BURTC's 183-us
write floor, and software dispatch/jitter consumes more budget.  If on-device
measurement proves that a PROTIMER interrupt plus scheduler dispatch cannot
meet the radio requirement, the only compliant answer is a documented
hardware-controlled response (for example a validated sequencer/PRS route),
while the resulting completion notification still merely readies a Zephyr job.
It must not be papered over with a radio-private Counter queue.

## Acceptance plan

The implementation is ready to replace the temporary radio alarm path only
after all of these pass:

1. A native/QEMU unit test exercises active-to-BURTC, BURTC-to-active, and a
   deliberately stale old-backend IRQ; uptime never moves backward and a
   timeout fires once.
2. A hardware test schedules a 150-us `K_USEC()` deadline repeatedly in active
   mode and records compare-to-job-start latency.  It separately demonstrates
   that a just-in-time BURTC compare request is rejected/deferred at the
   six-cycle floor rather than silently waiting a wrap.
3. A PM test verifies that the next logical deadline is brought forward by
   wake latency, runs in EM2 and the supported EM4 path, and neither loses nor
   double-announces ticks over many transitions.
4. A radio test makes RX, TX, and TIFS persistent Zephyr jobs with no direct
   Counter alarm calls.  The QEMU active-scan exchange and a hardware timing
   capture must both pass.  Hardware captures, not QEMU alone, establish any
   TIFS claim.
