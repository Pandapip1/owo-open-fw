# One Zephyr scheduler across active and deep-sleep clocks

## Decision

The radio must not retain a private deadline scheduler.  RX, TX, TIFS, and
unrelated real-time work are ordinary persistent Zephyr jobs: an interrupt may
make a job ready, but it must neither run the job nor own a second timeout
queue.  The long-term implementation is one Zephyr timeout queue with a
power-aware system-timer driver:

| CPU/power state | Clock-event backend | Purpose |
| --- | --- | --- |
| Active / EM1 | TIMER0 (32-bit, EM01GRPACLK) | high-resolution next-deadline interrupt |
| EM2 / EM3 / EM4-capable idle | BURTC | retained low-power wake alarm |

The system timer, rather than a Bluetooth driver, chooses and changes the
clock-event backend.  It presents one monotonic kernel tick stream and one
`sys_clock_set_timeout()` implementation.  The scheduler remains the sole
owner of runnable-job selection.

This design is implemented as the scheduler clock service
(`vendor/zephyr/kernel/clock_event_sched.c`, `CONFIG_CLOCK_EVENT_SCHED`) with
`CONFIG_CLOCK_EVENT_SYSTEM_TIMER` as the system timer (`prj.conf`). The active
provider is TIMER0 (`CONFIG_COUNTER_SILABS_TIMER_CLOCK_EVENT`), not PROTIMER,
whose start encoding is not public (`docs/radio-tables.md` 7.82); the deep-sleep
provider is BURTC (`CONFIG_SILABS_BURTC_CLOCK_EVENT`). The sections below keep
the design reasoning and say where the implementation differs.

## Current clock-event integration boundary

The service keeps one 64-bit logical timebase at
`CONFIG_CLOCK_EVENT_SCHED_FREQUENCY` (2^25 Hz on Series 2, so a 32,768-Hz BURTC
cycle is a whole number of logical cycles; `soc/silabs/silabs_s2/Kconfig.defconfig`).
Clients bind with `k_clock_event_sched_init()` and never name a provider: an
arm whose lead covers the deep provider's floor plus
`CONFIG_CLOCK_EVENT_SCHED_DEEP_WAKE_NS` waits on BURTC, any other waits on
TIMER0 while PM states deeper than runtime idle are locked
(`kernel/clock_event_sched.c`, header comment). The board gives TIMER0 and BURTC
to the service (`boards/owo_product.dtsi`). The radio module selects
`CLOCK_EVENT_SCHED` (`modules/efr32-radio/Kconfig`); the ticker, the radio
launch and the QEMU TIFS wake are three scheduler clock events
(`modules/efr32-radio/hal/cntr.c`, `K_CLOCK_EVENT_DEFINE`), and the radio's
persistent jobs are deadline events (`k_deadline_event_*`, `hal/ll_threads.c`).
No radio code uses the Counter alarm. The generic deferred-release selector
(`k_deferred_release_post_selected()`, `vendor/zephyr/kernel/deferred_release.c`)
remains a tested primitive the radio does not use.

The BURTC provider (`vendor/zephyr/drivers/timer/silabs_burtc_clock_event.c`)
owns `COMP` and requires the BURTC Counter driver to be off. Because `COMP` is
shared, a latched `COMP` flag only says that *some* event matched: each event
is delivered only once `CNT` has reached its own deadline, and an event already
due without its own match pends the IRQ instead of having a past value written
to `COMP` (that file's header comment). The older Counter-driver arbitration
(`counter_silabs_rearm_compare()`, `drivers/counter/counter_silabs_burtc.c`)
is still exercised by `tests/burtc-arbitration-port`: delivering on the flag
alone had released the ticker alarm early after a cancelled clock event's
match.

With icount `sleep=on`, QEMU used to charge host wakeup latency to guest time
whenever every vCPU idled: on macOS (no `ppoll`) BURTC compares were delivered
35-42 LF cycles (about 1.2 ms) late, which looked like missed scheduler
deadlines. The patched icount warp now stops at the next virtual deadline
(`vendor/qemu/accel/tcg/icount-common.c`, `icount_warp_rt()`). The
dual-endpoint harnesses additionally use `sleep=off` so a run's virtual
timeline does not depend on host scheduling.

## Why BURTC alone is insufficient for a reactive 150-us deadline

On the BG22, the BURTC provider enforces a five LF-clock cycle lead from the
`CNT` read to a deadline (`MIN_DELAY_CYC` in
`vendor/zephyr/drivers/timer/silabs_burtc_clock_event.c`; the older Counter
driver used six).  At 32,768 Hz that is 152.6 us.  The 32,768-Hz Zephyr tick
(`SYS_CLOCK_TICKS_PER_SEC` with `CLOCK_EVENT_SYSTEM_TIMER`) gives kernel
timeouts 30.5-us granularity, but does not remove that write-visibility floor.  A deadline discovered only
when a packet ends cannot therefore be programmed through BURTC for BLE's
150-us TIFS.

This is a hardware synchronization constraint, not a reason for an RX/TX
special case.  The active backend must be a clock-event source with a safely
shorter arm latency.  The existing `silabs,protimer` Counter interface reports
a 1-MHz timebase (`RAIL_TIMER_FREQ_HZ` in
`vendor/zephyr/drivers/counter/counter_silabs_protimer.c`).  It was the
candidate active backend; TIMER0 took that role instead
(`COUNTER_SILABS_TIMER_CLOCK_EVENT_MIN_LEAD_NS`, default 2,000 ns; 76.8 MHz
on silicon, `docs/radio-tables.md` 7.82).

The stock facts support treating PROTIMER as the active radio-clock candidate,
but not yet treating it as a complete timer specification:

- `flash-stock.bin:0x29974..0x29a36` is the observed setup window; its first
  traced writes include `PROTIMER +0x008 = 0x00011102`, `+0x030 = 0x1233`,
  `+0x038 = 0x80015558`, and `+0x00c = 1`
  (`docs/radio-tables.md` §4.3).
- The stock IRQ vector dispatches IRQ36 through the shared radio handler;
  archive offsets `rfhal_protimer.o:0x527c0..0x527cc` read `+0x014` and clear
  the observed flag bank (`docs/radio-tables.md` §4.3).
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
   timing granularity in tickless mode (`SYS_CLOCK_TICKS_PER_SEC`, `vendor/zephyr/kernel/Kconfig:897-925`).
   With `CLOCK_EVENT_SYSTEM_TIMER` the BG22 runs 32,768-Hz kernel ticks over
   the 2^25-Hz logical timebase (`soc/silabs/silabs_s2/Kconfig.defconfig`).
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
   wake timer (`vendor/zephyr/include/zephyr/drivers/timer/system_timer.h:294-313`).
   Zephyr calls the former under `sys_clock_lock()` after subtracting declared
   PM exit latency (`vendor/zephyr/subsys/pm/pm.c:232-245`).  The driver must
   additionally reject an idle/deep-state hand-off whenever the next deadline
   is inside BURTC's synchronization lead plus oscillator/PM-exit budget.
5. The active backend must be initialized before general scheduling and must
   not depend on RAIL.  Therefore it needs a small direct clock-event
   driver or a validated always-available hardware interface; wrapping
   `sl_rail_get_time()` is not sufficient.  This is the TIMER0 provider in
   `drivers/counter/counter_silabs_timer.c`; QEMU models TIMER0's counter and
   compare (`docs/radio-tables.md` 7.73).

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
(`vendor/zephyr/include/zephyr/kernel.h:1380-1408`).  Admission control,
priority assignment, and bounded job work remain required.

Time arithmetic: the service keeps 64-bit logical time; every 32-bit value a
client sees (`k_cycle_get_32()`, deadline-event deadlines, clock-event arms,
provider counts) is compared only as a modular difference read as signed, so
any two values compared must lie within 2^31 cycles (64 s at 2^25 Hz) of each
other. `tests/kernel/clock_event_sched` delivers arms across logical 2^31,
2^32, 2^32 + 2^31 and 2^33; the one place that broke the rule, the radio
sequencer's LIMIT check at TIMER0 = 2^31, is `docs/radio-tables.md` 7.88.

### Releases are not deadlines

The current upstream timeout interface cannot implement an “arm too late, so
run it now” policy by itself. `sys_clock_set_timeout(uint32_t ticks, bool
idle)` receives only the earliest timeout delta (`kernel/timeout.c:126-172`),
not a job identity, a preparation time, or a latest-completion bound. More
importantly, `z_add_timeout()` deliberately rounds a relative timeout *up* by
a tick so that it cannot fire before the requested interval
(`kernel/timeout.c:176-235`). Making that timeout's blocked thread ready early
would silently break the kernel's public timeout semantics.

The needed generic scheduler abstraction is therefore a separately declared
two-bound job request, rather than a radio exception or a new meaning for
`k_sleep()`/`k_timer`:

| Field | Meaning |
| --- | --- |
| `release` | Earliest time that running the CPU preparation is permitted. A job that is already released is runnable now. |
| `deadline` | Latest useful completion/preparation time. It is not an instruction to defer execution until that instant. |
| `emission` (optional) | A hardware-gated external action time. It must not be advanced merely because the CPU preparation job ran early. |

The vendored kernel provides `release` and `deadline` as a deadline event's
not-before bound and start-by deadline (`k_deadline_event_post_at()`, with
admission control in `k_deadline_event_admit()`; `docs/radio-tables.md` 7.76,
7.77); `emission` is the sequencer's timed command (7.82).

The scheduler should run an eligible job immediately according to ordinary
priority/deadline selection. A clock event is a last-chance wakeup only when
the CPU has no selected runnable work and is about to idle; it is not a reason
to keep an eligible job blocked while the CPU is active. This preserves normal
preemption policy: making a job runnable asks the existing scheduler to choose
it at the usual scheduling point and does not perform a manual context switch
from an interrupt.

Each selected clock-event backend must expose its minimum arm lead in the
same physical-time domain as its compare operation. At idle entry the system
timer compares `deadline - now` with that lead plus power-exit margin. If the
remaining slack is shorter, it does **not** program an inevitably late
compare: it leaves/marks the already-released preparation job runnable before
idle. A receive-window preparation may safely take this path. A transmit job
may prepare its FIFO/radio state early, but must not emit before its intended
airtime without a separately proven hardware gate.

This is deliberately not a new `sys_clock_set_timeout()` flag. That hook is
also used to preserve ordinary timeout semantics while the CPU is running;
`timeout.c` reprograms it whenever a newly inserted timeout becomes earliest.
A future upstream implementation needs a scheduler-level release/deadline
object and a clock-event capability interface, then can pass the selected
idle wake request to the active or low-power backend. The backend capability
must include at least arm lead, maximum arm span, power-state availability,
and wake/exit latency; a tick count alone is insufficient for a 32-kHz/1-MHz
handoff.

Even a correct high-resolution scheduler cannot guarantee a *newly discovered*
150-us radio turnaround: a packet-end IRQ leaves less than BURTC's 152.6-us
write floor, and software dispatch/jitter consumes more budget.  If on-device
measurement proves that a PROTIMER interrupt plus scheduler dispatch cannot
meet the radio requirement, the only compliant answer is a documented
hardware-controlled response (for example a validated sequencer/PRS route),
while the resulting completion notification still merely readies a Zephyr job.
It must not be papered over with a radio-private Counter queue. That is the
route taken: the M0+ sequencer launches timed commands and sends staged replies
at T_IFS (`docs/radio-tables.md` 7.82, 7.87), and T_IFS matches the stock stack
on silicon (7.71).

### Job kinds: one-shot, armed, periodic

Admission (`vendor/zephyr/kernel/deadline_admission.c`) guarantees what it accepts: a job it
admits starts by its deadline, and a request that would break that is refused when it is made,
never by revoking earlier work (`docs/safety-scheduling.md`, G5). Three kinds of job differ in when
that one check happens:

| Kind | API | Checked | After a run |
|---|---|---|---|
| One-shot | `k_deadline_event_post()`, or `k_deadline_event_admit()` ahead of a timed release | at the release, or when the timed release is admitted | its capacity is released |
| Armed (sporadic) | `k_deadline_event_arm()`, then `k_deadline_event_post()` for each activation | once, when armed | back to armed, keeping its reservation; no check |
| Periodic | `k_deadline_event_arm_periodic()`; the kernel releases it | once, when armed | asleep until its next period, keeping its reservation; no check |

**Arming.** A reservation (`struct k_deadline_reservation`) states the model of the job's
releases: budget C, minimum inter-arrival T (the period of a periodic job), start-by deadline D
from each release, and the budget of the interrupt handler that releases it. Arming checks the
job released at the worst moment against everything admitted, and adds the reservation to every
later check, so no later request can eat into it. The arrival model: at most one run outstanding,
no two releases closer than T (`D <= T` and `C + handler <= T` are required). In any interval of
length t the job then has at most floor((t + jitter) / T) + 1 releases; jitter is 0 for an armed
job (its spacing is enforced at the release) and one kernel tick for a periodic one (a release is
announced up to a tick after its tick). That bound is sound because the model is enforced: a
release while a run is outstanding, or sooner than T after the previous one, is refused and
reported as `K_DEADLINE_MISS_OVERRUN`.

**Activation outside the model.** Refused at its source, not latched and not run late: a second
activation while a run is outstanding is demand no reservation covers, and latching it would let
it delay jobs whose guarantees assumed it could not exist. The report is the source's contract
broken, handled by the miss policy like any other failure (the product: safe shutdown). A
periodic release that finds its previous run outstanding is the same and is dropped; the next
period is still scheduled.

**Recurring jobs cannot fail to re-arm.** Finishing a run only changes the event's state; the
reservation stays in the admission state until `k_deadline_event_disarm()`, and every check proves
it still holds. A periodic job's next release is scheduled when the current one is made, not when
its run ends.

**Release is a scheduling point.** Releasing a job enters the scheduler (the post that readies it
reschedules), so it takes the CPU at once from any lower-priority job the policy lets it preempt,
with no separate preemption mechanism. That holds when the released job is a meta-IRQ thread
(meta-IRQs preempt cooperative threads) or the running job is preemptible. It does not hold for:
a running cooperative thread against a non-meta-IRQ release; a same-priority cooperative run
(cooperative threads are not preempted by their own priority, which the radio's jobs rely on for
mutual exclusion); sections with interrupts masked or the scheduler locked; interrupt handlers.
Admission's bound counts a lower-priority blocker only in those cases (`preempts()`,
`sim_blocker()`).

**The bound with reservations.** Without reservations the simulation of the admitted jobs is
exact and is all a check does. With them, each job is also bounded against releases of the
recurring jobs at any time: a one-shot job the simulation starts at s starts by the least x with
x = s + B + R(x) + W(s, x) (R the reservations' work in an interval of length x, W the one-shot
work released after s that runs ahead of it); a recurring job starts within the least x with
x = blocking + B + O + R(x) after its release (O every admitted one-shot job at its priority or
above). B is one blocker the policy does not switch from. Interference counted from the check
covers every phase, so the bound is pessimistic for releases far ahead of the check; a bound per
busy period is the next refinement (`docs/safety-scheduling.md`).

**Cooperative runs.** Interference can reorder non-preemptive runs: a run delayed past a later
release lets that release start first and hold the CPU, so a job can wait longer than the
interference that delayed it. Within a cooperative priority B therefore includes the largest
same-priority job released before it with a later deadline. Release being a scheduling point does
not remove that term inside one cooperative priority; it removes it between priorities: an urgent
job given a meta-IRQ priority of its own above the long cooperative bodies (the radio's LLL
prepares) is never blocked by them.

**Mapping.**

| Work | Kind |
|---|---|
| Radio: the sequencer's staged replies and T_IFS turnarounds, each completion while an event is live | armed for the event's duration, disarmed at its end |
| Radio: LLL prepares (ticker expiries) | one-shot timed releases admitted ahead, or periodic for a connection's fixed interval |
| Safety S1-S4 per pulse (`docs/safety-scheduling.md`) | one-shot jobs of an admitted pulse chain |
| Safety S5 (dead-man check of the coverage window), S6 (stop input read), S7 (battery), S10 (watchdog feed) | periodic |
| An interrupt that releases an armed job | that job's reservation: its handler budget, at the job's rate |

Not yet mapped in code: the radio and the product still post one-shot releases only.

## Acceptance plan

The implementation is ready to replace the temporary radio alarm path only
after all of these pass (status: 1 is the `clock_event_sched` kernel suite;
4 passes in QEMU and on silicon with TIFS on the sequencer, `docs/radio-tables.md`
7.71, 7.76, 7.82; 3 passes in QEMU for EM2, the product entering EM2 between radio events with
the HFXO/DPLL restart in the deep wake-up budget, 7.95; 2 and EM4 are open):

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
