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
above). B is one blocker the policy does not switch from.

For a one-shot job the bound runs per busy period, not from the check. The simulation records
which priority ran when. The level-p busy period (p the job's priority) in which the job really
starts begins at a time u at which nothing at p or above ran; interference only delays work, so
the simulation, which has done at least as much by any time, was idle at level p there too, and
u is the check or the start of one of the simulation's level-p busy periods at or before s. From
each such u the job starts within the least fixed point of w = L(u, s) + B + R(w) + W(s, u + w),
L the level-p work the simulation ran in [u, s); the bound is the largest u + w
(`sim_one_shot_start()`). A release far ahead, after the CPU has gone idle, is charged the
interference of its own busy period, not of the whole wait (test
`busy_period_bound_for_a_far_release`).

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

**Which kind.** Released by time alone: periodic (timed). Released by an event (a GATT write, a
radio completion): armed.

**Timed releases** (vendor/zephyr, `CONFIG_SCHED_DEADLINE_TIMED_JOBS`). A periodic job's
releases ride a scheduler clock event of the kernel's own pool, delivered at the release time,
not at the next tick and not through kernel timeouts. Its start-by counts from the release
time, not the delivery; the delivery's lateness, up to `CONFIG_CLOCK_EVENT_SCHED_LATE_TOLERANCE_NS`
(beyond which the clock service reports it), is its jitter, and admission checks its own start
that much earlier. `k_deadline_event_release_at()` lets the job choose its next release (a pulse
schedule: any time at least a period after the last), which cannot fail. Without the clock
service a kernel timeout stands in (a tick of jitter).

**Pause and resume.** `k_deadline_event_pause()` stops a timed job's releases and keeps its
reservation: admission charges it at its declared rate as for an armed job, so nothing is
admitted into its space (paused = armed). `k_deadline_event_resume()` is an activation and cannot
fail: the job is released now, or held (not reported) until a period after its last release,
and its period restarts there. Only `k_deadline_event_disarm()` frees the capacity; starting
again after it is a new arm, checked. A job declared `non_pausable` refuses a pause (`-EPERM`):
the watchdog feed and the amateur identification. The pump's pulse job pauses when stimulation
stops instead of disarming.

**Change requests.** `k_deadline_event_change()` replaces an armed or timed job's declaration
(budget, inter-arrival or period, deadline, interrupt budget, pausability; the resource set with
the resource model). The new declaration is checked against everything else admitted with the
current one removed, and until the switch the envelope of both is charged (larger budget and
interrupt budget, shorter inter-arrival, earlier deadline), so the transition is covered too.
Admitted, it takes effect at the job's next release; the run in progress completes under the old
one; a timed job's first release under it is the new period after the last old one. Refused, the
old declaration stays in force, its charge unchanged. A change that only reduces demand charges
the current declaration, so it passes. A non-pausable job keeps a non-pausable declaration.
Thread context only: the check is admission work.

Tests (deadline_admission, native_sim, mps2/an385 and the BG22 model): pause then resume cannot
fail; an early resume is held to its period; a paused job's capacity is not given away; a
non-pausable job refuses a pause; a change applies at the next release; a changed period spaces
from the last release; an infeasible change is refused with the old one charged; a reducing
change passes; a non-pausable job cannot change into a pausable one; on the model, two timeouts
100 us apart both expire within a tick.

### Jobs: every timed call belongs to the job that makes it

`CONFIG_SCHED_DEADLINE_JOBS` (vendor/zephyr). The kernel tracks the job each thread runs: a
deadline event's consumer is in a run from the wait that starts it until it waits again.
`z_deadline_current_job()` is that job for the running thread, NULL for a thread with none, for
an interrupt (Cortex-M entry saves the job, clears it, and restores it at exit, so a nested
handler never inherits one) and before the scheduler starts. Every kernel timeout records the
job that armed it (a thread's own timeout, its thread's job) and its expiry runs as that job. A
timeout armed with no current job is an undeclared claim: recorded per thread and handler in
`z_deadline_job_probe` (`CONFIG_SCHED_DEADLINE_JOBS_PROBE`), and with `CONFIG_SCHED_DEADLINE_JOBS_STRICT` reported as
`K_DEADLINE_MISS_UNDECLARED` (object NULL) at the next resume point.

**A sleep inside a run ends it; the wake is the next release** (default, pending user
confirmation: "a timed wait mid-run is a new release"). The release time is the tick the
timeout expired at, so the announcement's lateness counts against the job's start-by. The wake
must fit the job's declaration: closer than its spacing it is `K_DEADLINE_MISS_OVERRUN`; a job
with no reservation is `K_DEADLINE_MISS_UNDECLARED` (strict) and resumes unchecked; a start
after the deadline is `K_DEADLINE_MISS_START`. A timed job's period restarts from that release.
**Interim deviation, pending the resource model:** waits on kernel objects (mutexes,
semaphores, queues, a device's completion) are resource waits and the run continues; only a
sleep ends it. Ending the run at every such wait cut each pump pass at its first I2C or IADC
completion and STATE contention (about 430 `K_DEADLINE_MISS_OVERRUN` per 120 s stimulating run).
The resource model, under which a started job never blocks, removes those waits and with them
this deviation.

**No shared announcing thread.** Expiries are announced at the clock-event interrupt again;
each callback is attributed to its job, so the line's cost per expiry is that job's. The
announcing job of step 1 and its spacing floor are gone.

**Threads that wait on an object for their work** (work queues, the Bluetooth receive
threads) become armed jobs with `k_deadline_job_bind()` (`k_work_queue_job_bind()` for a work
queue): each wake from that wait is a release and each wait on it ends a run; their other waits
stay resource waits. A wake sooner than the declared inter-arrival after the last release is
held until it, not reported (as a resume is), so the declaration's spacing holds by
construction and `D <= T` keeps one run outstanding. The Bluetooth host and controller expose
their queues and receive threads for the product to declare.

**Not yet bound in the product** (finding, pending the pulse option and a priority decision).
Bound with provisional declarations, admission refused four of the six Bluetooth jobs and then
the pump: the Bluetooth receive threads are cooperative at -8 and -10, above the pump (-2) and
the feed (-3), and the pump's own declaration (1893 us every 2000 us) leaves no capacity below
it. Binding them needs the pump decomposed (the pulse option) and the safety jobs placed above
the Bluetooth threads. The product binding (`src/bt_jobs.c`, a startup job for the main
thread's bring-up) is kept aside until then; strict mode stays off.

**Essential threads may suspend themselves.** A deadline-event wait suspends its consumer; the
main thread (essential) is the product's pump, so `z_thread_halt()` now panics only when an
essential thread terminates, not when it suspends.

Tests (deadline_jobs, native_sim and the BG22 model): a sleep in a job releases on time; a late
wake is reported; a wake inside the declared spacing is reported; an undeclared blocking call in
a job is reported; a timed call outside a job is reported naming the thread; the current job
follows switches and is not inherited by an interrupt or a preempting thread; a timer started in
a run expires as its job.

**The product's jobs** (src/feed_job.c, src/lib.rs, src/wdt.rs). Defaults pending user confirmation:

| Owner | Kind | Why |
|---|---|---|
| WDT feed (S10) | periodic, non-pausable; its own thread, cooperative above the pump | released by time alone, at its period; legal and safety containment |
| Pump pass and pulse schedule | pending the pulse option (user) | a kernel-semaphore loop for now; its timed waits are undeclared |
| sim_inject polling (simulation builds) | periodic | released by time alone |
| Amateur identification (not in the product build) | periodic, non-pausable | released by time; legal requirement |
| Amateur frame and release timers | armed | started by the identification event |
| BT host threads | armed, one per thread (rx, tx processor, work queues, controller receive) | released by controller events, queued packets, work submissions, radio completions; not yet converted |


The feed job's declaration: period half the time to the warning (23.4 ms), start-by a quarter
(11.7 ms), budget 200 us; `owo_core::watchdog::feed_fits_window()` checks that period + jitter +
start-by + run stays under the warning (46.9 ms) before it is declared and on every change
(`wdt::change_feed()`). WDOG0's period and warning cannot change: CFG is written only with the
watchdog disabled (`em_wdog.c` disables it first) and `src/watchdog.c` locks it after
configuring; disabling it would remove the containment. Only the feed's timing changes, inside
the fixed window.

### Dispatch: priority among the satisfying orders

**The rule.** Among all orderings that meet every admitted deadline, run the highest-priority
ready job first; run a lower-priority job ahead of a higher one only when every satisfying
ordering requires it. Feasibility first, priority breaks the ties.

**Today.** Not implemented. Dispatch is Zephyr's fixed priority (EDF within one priority), and
admission simulates exactly that, so it refuses sets another order satisfies. Evidence:
`test_fixed_priority_refuses_a_satisfiable_set` (vendor/zephyr 1fd04697089). A low job (100 us,
start by +400) and a high job (500 us, start by +5000), both released at +100: fixed priority
starts the low job at +600, late, and the high release is refused; low then high starts them at
+100 and +200.

**Feasibility oracle.** For start-by deadlines on one CPU with the jobs released, ordering by
start-by deadline (EDF) is optimal: if any order starts every job in time, EDF does (exchange
argument). For the released set R at time t, with C the remaining budgets, define each job's EDF
slack S_k = d_k - t - sum of C_i over i != k with d_i <= d_k. R is satisfiable iff every S_k >= 0.

**Dispatch decision.** At a scheduling point, let h be a candidate. Running h first (to the end
of its run, the conservative case) keeps R satisfiable iff C_h <= S_k for every k with
d_k < d_h: jobs with later deadlines already count h's work. With S in deadline order and a
prefix minimum, each candidate is checked in O(1): sort once (n <= 12, insertion sort), prefix
minima once, then walk the candidates in priority order and take the first that passes. O(n log
n) per scheduling point. If none passes, run EDF's choice (the earliest deadline), which keeps
R satisfiable by the oracle. By induction the schedule meets every deadline whenever the set was
satisfiable at admission, and it runs the highest priority the deadlines allow at every step.

**Where it decides.** Only at scheduling points, which already exist: a release (the post that
readies a job enters the scheduler, the user's "free preemption at activation"), a run's end
(its next wait), an arm or disarm. Between them nothing re-decides, and the check assumes the
chosen job runs to its budget, so no timer is needed for the rule itself (G1's budget timer
catches a job that runs longer). The decision is cached at the scheduling point; `next_up()`
reads it in O(1), so the scheduler's critical section (interrupts locked) grows only by the
O(n log n) step at releases and run ends. Cost to measure on silicon before enabling: n = 12 is
on the order of a few hundred instructions.

**What it changes in the meta-IRQ and cooperative semantics, and what it does not.**
- A running job is never preempted by a lower-priority one. A lower-priority job can only be
  chosen over a higher one that has not started. So nothing a running job relies on changes:
  run-to-completion within a cooperative priority, and the radio's mutual exclusion between its
  jobs (same meta-IRQ level, non-preemptive), stay as they are; a higher priority still
  preempts a lower one.
- Choosing a lower job L while a higher job H is released but not started is equivalent, for
  every piece of code, to H having been released after L started: any code already has to be
  correct under that interleaving, since releases come at arbitrary times. No new data race.
- What changes: a higher-priority job can now start later than an admitted lower one when the
  deadlines require it; and the realizable schedules are "priority, except where the guard
  forbids", not fixed priority. Zephyr APIs that assume fixed priority (priority inheritance on
  mutexes, `k_yield()`) are unaffected for threads outside admission; the override applies only
  between deadline-event consumers about to start a run.
- Threads outside admission (the BT host, the pump until it is mapped) are background: they run
  when no admitted job is ready, or when the guard leaves slack. Mapping them onto job kinds
  (kernel timeouts, above) brings them inside the rule.

**What admission checks.** The existence of a schedule the rule produces, i.e. that the rule's
own schedule meets every deadline. Admission simulates the rule (the same guard at every
simulated step, over the admitted releases, the reservations' worst-case releases and the
interrupts' supply bound per busy period), replacing the fixed-priority simulation. Because the
rule never leaves EDF-feasibility, this accepts every set EDF can schedule within the rule's one
constraint (no lower preempting a running higher), which a blocking term covers: per job, the
largest remaining run of a higher-priority job that can be running when it is released.
Sufficient, not exact, inside non-preemptive groups: non-preemptive scheduling is NP-hard in
general (it may need inserted idle time), so a set satisfiable only by idling is refused.

**Cost and RAM.** The simulation's per-step selection becomes the O(n log n) guard: the check
grows from about 26 x 12 steps to about 26 x 12 x 4. The admission check runs in the releasing
interrupt, so its time is part of that interrupt's budget; measure before enabling. RAM: the
slack and order arrays, 12 x 8 B, static.

**Implemented** (vendor/zephyr 11e01fd7b78, `CONFIG_SCHED_DEADLINE_DISPATCH`, default on with
admission, uniprocessor). The guard sorts the jobs by deadline once per check (insertion sort)
and takes every candidate's fit from one sweep (least slack of the strictly earlier deadlines),
so a simulated step is O(n). Admission simulates the rule; with the option off it simulates
Zephyr's fixed priority, as the runtime then is. Tests: the demonstration case is now admitted;
higher first when it fits; lower first only when required; a running job not preempted by a
lower release (the release is refused instead).

| Measurement [Q] (EFR32BG22 model, `-icount shift=3`, `CONFIG_SCHED_DEADLINE_ADMISSION_PROBE`) | Longest | Count |
|---|---|---|
| Admission check, kernel admission suite (up to 12 jobs, reservations, full table), fixed-priority simulation | 4688 cycles (140 us) | 104 |
| Same, the dispatch rule, first version (slack per job per step, O(n^2)) | 22978 cycles (685 us) | 104 |
| Same, the dispatch rule, sorted sweep (shipped) | 11680 cycles (348 us) | 104 |
| Dispatch decision, same suite | 276 cycles (8.2 us) | 172 |
| Admission check, product (QEMU profile with radio admission on, about 5 radio jobs, 60 s) | 1393 cycles (41.5 us) | 11 |
| Dispatch decision, product, same run | 248 cycles (7.4 us) | 58 |

(cycles of the 2^25 Hz scheduler clock.) The product run is not a working link: the QEMU profile
keeps radio admission off because the model's longest interrupt (72.4 us) exceeds the ADV_IND
completion's window (46.6 us), and with it on the product refused two releases and the Central
never connected; its figures are the checks that ran. On silicon [P] (kit list): the model runs
the M33's paths up to 35 % short (`radio-tables.md` 7.93), so silicon may be up to 1/0.65 of
these [I]. A check runs in the releasing interrupt, so its time belongs in that interrupt's
budget: a 12-job worst case of 348 us is too long for the radio's windows, and the product's
own job count (about 5) must be what bounds it. The full-table case is the suite's, not the
product's.

### Resources: the CPU, mutexes and the sequencer as one model (design, not implemented)

**Rule (user):** a claim of a shared resource that could make another admitted job miss its
deadline (planned preemption included) blocks; jobs declare up front every resource they may
hold; the CPU itself is a declared resource; with several processors the CPU is a semaphore.

**Model.** A resource has a capacity: a mutex 1, a counting semaphore N, the CPU the number of
interchangeable processors (1 on the BG22's M33). A job's admission request (one-shot release,
arm, periodic arm, interrupt-line reservation) carries its declaration: for each resource it
may hold, the longest hold, as own execution time inside the hold. The CPU entry is the job's
budget; an interrupt line's reservation is a declaration of CPU time at interrupt level.

**Ceilings and claims (Stack Resource Policy).** Every resource gets a ceiling per number of free
units: the highest preemption level among the jobs whose declared need exceeds that many units
(for a mutex, simply the highest level among its declared users). Ceilings are computed when a
declaration is admitted or removed (arm, reserve, disarm, budget registration), never at a claim.
A job starts only when its preemption level is above the system ceiling (the highest ceiling of
the resources held now) and the dispatch guard allows it; once started it never blocks on a
declared resource, so there is no priority inversion beyond one critical section and no
deadlock, nested claims included. A claim then only checks the declaration: a resource the job
did not declare is refused and reported (`K_DEADLINE_MISS_UNDECLARED_RESOURCE`, through the miss
policy: safe shutdown in the product), never granted. Holding past the declared length is an
overrun, caught by the same enforcement as G1.

**Admission.** One path over all declared resources. CPU demand is what the simulation and the
per-busy-period bound already compute. Every other resource adds the SRP blocking term: per job,
the longest declared hold of a lower-level user of a resource whose ceiling reaches the job's
level, at most one such hold. What exists maps onto it:
- the cooperative blocker B_j and the radio's run-to-completion groups: each cooperative
  priority level is a capacity-1 "run" resource that every job at that level holds for its whole
  run, ceiling = that level; B_j is exactly its SRP blocking term;
- `irq_lock()` sections and `CONFIG_SCHED_DEADLINE_ADMISSION_BLOCKING_NS`: a resource "interrupts
  masked" whose ceiling is above every interrupt level; its longest declared hold is the term;
- `EFR32_RADIO_AES_BLOCKING_NS`: the RADIOAES resource's blocking term (below);
- the busy-period bound: CPU demand, unchanged;
- G1: the CPU entry's overrun; IRQ_STRICT: a CPU claim (an interrupt) without a declaration.
Nothing stays a parallel path.

**Enforcement.** One hold timer per running job: armed for the earliest declared end among the
resources it holds (the CPU's being its remaining budget), with G1's accounting (own time:
switched-out and interrupt time excluded), so G1 becomes the CPU case of the hold check.

**Code outside admission** (the BT host, the work queues, drivers called from host threads).
Resources used only among unadmitted threads keep Zephyr's semantics, priority inheritance
included. A resource shared between an admitted job and unadmitted code needs the unadmitted
side declared too: a declaration without a deadline (its holds and their lengths), so the
ceiling and the blocking term count it; its claims are checked against the ceiling like any
other. SRP makes priority inheritance unnecessary for admitted jobs (they never block on a
declared resource once started); keep inheritance for mutexes used only by unadmitted threads.

**Several processors.** The declaration, admission and enforcement code stays capacity-generic.
Admission for CPU capacity above 1 (global or partitioned multiprocessor analysis, multi-unit
SRP) is future work, not implemented.

**The BG22's M0+ sequencer** is a second processor that runs only sequencer code, so it is its own
capacity-1 resource, "sequencer", not a second unit of the M33's CPU. Declare the radio's
sequencer work against it: a timed command holds it from its post to the TXEN or RXEN it times,
a staged reply from its staging to the end of its turnaround. Today the HAL serialises these
implicitly (one timed command at a time); as declared holds, admission proves two radio
operations never need the sequencer at once and refuses the release that would. SEQRAM's single
port (B75, B77) is a bus effect, not a claimable resource: concurrent accesses slow both cores.
Fold it into the budgets (both cores' holds measured under the worst contention), and where the
sequencer's timing must not be slowed (its T_IFS hold, 7.74's rule against M33 SEQRAM access),
declare a capacity-1 "SEQRAM quiet" resource that the sequencer window holds and every M33 job
touching SEQRAM declares: the ceiling then keeps M33 SEQRAM accesses out of those windows.

**Inventory: resources shared between admitted jobs, or between admitted jobs and other code.**

| Resource | Capacity | Users | Hold (declared) | Today |
|---|---|---|---|---|
| CPU (M33) | 1 | every job, every interrupt line | budgets, handler budgets | budgets, G1 |
| Sequencer (M0+) | 1 | radio timed commands (TX/RX launch), staged replies (connection, scan response, SCAN_REQ, CONNECT_IND) | post to TXEN/RXEN; staging to turnaround end | implicit HAL serialisation |
| SEQRAM quiet | 1 | the sequencer's timing-critical windows; M33 radio code that writes SEQRAM (mailbox, staged frames) | the window; the M33 access | 7.74's rule, by code review |
| LLL run group (each cooperative meta-IRQ level) | 1 | LLL context, TX/RX launch jobs, completion job (LLL level); ULL worker (its level) | each run | cooperative semantics, B_j |
| RADIOAES | 1 | radio CCM (encrypted connection events); host AES-CMAC/ECB (SMP f4/f6/g2, session key; unadmitted) | CCM per PDU; host 412 cycles (5.37 us silicon) | `EFR32_RADIO_AES_BLOCKING_NS` |
| Interrupts masked | 1 (ceiling above all lines) | every `irq_lock()` section (kernel, drivers, radio HAL) | longest section, to be measured | `CONFIG_SCHED_DEADLINE_ADMISSION_BLOCKING_NS` |
| I2C0 bus with the expander | 1 | stop-input read (S6), supply enable and DAC /CLR writes (pulse setup, teardown, safe-off), expander /INT read (host work queue today) | one transfer: 3 bytes at the bus clock | driver mutex (priority inheritance) |
| IADC | 1 | S2, S3, S7 conversions (supervisor) | one conversion (17.6 us QEMU, silicon [P]) | one conversion at a time (`Hw` contract) |
| Stimulation command state (pump / GATT write) | 1 | pump (to be admitted); BT RX thread's `write_cb` (unadmitted) | the copy in or out | Rust `Mutex` |
| USART0 (channel switch), USART1 (DAC), TIMER1 (boost) | 1 | the supervisor only | - | single user: declared, no blocking |
| WDOG0 warning | - | outside the scheduler by design (containment) | - | direct interrupt |

**Tests (with the implementation):** a claim that would endanger an admitted deadline blocks and
is granted once safe; a safe claim is granted at once; no inversion in a three-job inversion
scenario; no deadlock with nested claims; an undeclared claim refused and reported; an over-long
hold reported; a declaration that makes an admitted set infeasible refused at admission.

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
