# Safety scheduling: every safety is a scheduled job

**Design, not implemented.** This document replaces the "one supervision pass per watchdog feed"
model (`watchdog.md`, "Who feeds, and what a feed certifies") once the scheduler gaps below are
closed. Until then `watchdog.md` describes the tree. Tags as in `watchdog.md`: **[F]** code or
RM/datasheet, **[E]** QEMU, **[I]** inferred, **[P]** placeholder with a silicon measurement owed.

## Principle

The product runs on an RTOS: a safety's timing is a guarantee the scheduler gives, never a
consequence of what other work happens to do.

- Every safety is a job (a deadline event with a declared budget and start-by deadline) or a
  condition checked inside one. Its release, window and deadline are its own parameters.
- Admission proves the job set feasible before a job is armed. A request that cannot be proved
  is refused when it is made (unsat), with the numbers. Nothing widens a tolerance to absorb
  preemption.
- At run time the scheduler enforces each job's budget and deadline. A late start or an overrun
  is a miss, and a miss of a safety job is the ordered safe-off (the warning and the reset stay
  the containment if that cannot run).
- Interrupts are work too. Each is declared (budget, minimum inter-arrival) and accounted, so
  no job's guarantee depends on a guess about interrupts.
- Each safety records when it last ran (a timestamp in a registry). The watchdog feed is itself
  a job; it is granted only if every safety that is active in the current state ran recently
  enough (its freshness limit) and the output is quiescent.

The 3414 us overrun slack (branch `worktree-agent-a1093d25eb42b1faf`, not merged) was the
opposite: it summed other jobs' budgets into the phase check. Its term table remains valid
evidence of what can preempt a phase today, and is the input to the gap list below.

## Safety inventory

States: **idle** (not armed), **armed** (a session is armed, no pulse in flight), **pulsing** (a
pulse job is in flight), **EM2** (deep sleep; only reachable while not armed, `radio-tables.md`
7.95).

| # | Safety | Protects against | Schedule | Records its timestamp | Freshness limit checked by | Active in |
|---|---|---|---|---|---|---|
| S1 | Phase end | energised time and charge per phase beyond the command | one job per phase: not-before = closure + width, start-by = not-before + `d_pe`; budget = the bridge-off call | the phase-end job's start (bridge opened) | the pulse's next job: the bridge opened by start-by; else a miss | pulsing |
| S2 | In-phase sense (over-/under-delivery) | overcurrent; open electrode, dead sense, rail out of compliance | one sense job per phase, anywhere in [closure + settle, phase end - t_conv]; an over-delivery reading releases S1 at once | completion time of a conversion that both started and ended with the bridge closed | the phase-end job: a reading inside this phase, else `Unmeasured` | pulsing |
| S3 | Zero check | stuck-high or leaking sense path (the bridge never closes on it) | one job per pulse, after routing and before the DAC level; start-by = the pulse's closure minus its own budget | completion of a conversion with bridge off and DAC 0 | the first phase job: a zero check of this pulse | pulsing |
| S4 | Per-pulse admission conditions: hard level cap, soft start, phase and gap clamps, duty bucket, charge balance, switch read-back, break-before-make | over-level, over-duty, imbalance, wrong electrode, shoot-through | checked by the pulse's setup job before any closure (no period of their own) | - | the pulse is refused before it is admitted | armed, pulsing |
| S5 | Dead-man (coverage) | output continuing after the controller stopped commanding | one timed job at valid_until + `deadman_us` (1.5 s), re-armed on each frame; no polling | the armed expiry it holds | the feed: armed implies a dead-man job admitted for the current valid_until | armed, pulsing |
| S6 | Stop input (power button) | the user cannot stop the output | /INT edge releases a read job; a periodic read job every `STOP_INPUT_POLL_MS` = 100 ms (stock: 10 Hz) bounds a lost edge; a failed read stops the output | completion of a successful expander read | the feed: age <= 100 ms + one read's budget | armed, pulsing |
| S7 | Battery | operating out of the pack's safe range | one read job before a session's first pulse; then every `BATTERY_INTERVAL_MS` = 1 s while pulses fire (stock: once per second) | completion of a battery conversion | the feed, while pulses fire: age <= 1 s + one conversion's budget; the setup job: a reading of this session | armed, pulsing |
| S8 | Output quiescent | bridge closed outside a phase job | evaluated by the feed job (GPIO output latches) | - | the feed | all |
| S9 | Radio deadlines | radio work past its deadlines (the radio's own safe shutdown, which stops the output) | the radio's admitted jobs (existing) | - | the scheduler (miss policy) | all |
| S10 | Watchdog feed | everything above failing to run | a periodic job, period = warning / 2 = 23.45 ms, start-by = period (existing cap) | the feed itself (WDOG0 clear) | WDOG0: warning at 46.90 ms forces the output safe, reset at 62.53 ms (`watchdog.md`) | all, EM2 included |

Parameters and their evidence:

- `d_pe`, the phase end's start-by tolerance: **one period of the stock stimulation timer, 50 us**
  (`stock-stim-path.md`: TIMER0 at 20 kHz times the stock phases, so the stock phase end is
  quantised to 50 us) [I]. It is a requirement on the product, not an allowance for preemption:
  admission must prove it. The commanded width is clamped so that width + `d_pe` never exceeds
  `OWO_STIM_MAX_PHASE_US` (400 us, the safety-reviewed bound), which tightens today's limit:
  the longest deliverable width becomes 350 us.
- Phase-end budget: the bridge-off call. QEMU: within 8.4 us (the part of the bridge calls
  outside the pin edges, slack branch derivation) [E]; silicon [P].
- `t_conv`, start of a conversion to its result: QEMU 17.6 us (17.6-18.4, 64 conversions),
  of which the RM's warm-up and conversion are 6.12 us (`stim-simulation.md` finding 5) [E, F];
  silicon [P]. This replaces `OWO_STIM_SENSE_GUARD_US` (60 us nominal).
- Settle: `OWO_STIM_SETTLE_US` = 20 us, nominal; the sense signal's rise time at the lowest load
  impedance is owed on silicon [P].
- Stop-input read budget: two data bytes and an address at the expander's I2C rate, about
  0.3 ms at 100 kHz [I]; measured [P].
- Battery conversion budget: `t_conv` as above.
- A phase that cannot hold one conversion (width < settle + `t_conv`) cannot satisfy S2; see
  decision 2.

What the feed no longer certifies: that "a pass ran". It certifies that every active safety ran
within its own limit and the bridge is off. Phase-scale safeties (S1-S3) are microseconds and
are checked by the pulse's own jobs; the feed only checks that no safety job has missed since the
last feed (a miss already ran the safe-off; the feed refuses so the safe-off's failure still
leads to the warning).

## Phase length

A pulse is a chain of jobs on one consumer at a priority above the radio's: setup (S4, S3), phase
A (closure, S2, S1), gap, phase B (closure, S2, S1), teardown. Each is admitted, with its budget
and start-by deadline, before the setup job closes anything:

- If the chain is not feasible with the work already admitted (the radio's armed prepares and
  launches among it), the pulse is refused before the supply or the bridge is touched. That is
  the pulse's unsat: the duty bucket is not charged and the engine moves on.
- Once admitted, nothing of lower priority can delay a phase end. Higher-priority work is only
  declared, budgeted interrupts, which admission counted.
- If a phase-end job still starts after `d_pe` (a scheduler bug or an undeclared interrupt), the
  scheduler reports the miss and the miss policy opens the bridge and runs the ordered safe-off.
  The software overrun check (`overrun_slack_us`) and the scheduler lock in `hal.rs` go away.

The radio's guarantees stay intact the same way: the pulse chain is admitted against them, and a
radio release that arrives later is admitted against the chain.

### Interrupt lines: rates by construction

The kernel now takes a reservation per interrupt line (`k_irq_reserve()`: handler budget, minimum
inter-arrival; vendor/zephyr 6543e623fd5). Admission counts it as interference on every job, and
with `CONFIG_SCHED_DEADLINE_IRQ_BUDGETS` the Cortex-M interrupt entry checks every arrival and
handler run against it, reporting a violation as a miss (the product: safe shutdown);
`CONFIG_SCHED_DEADLINE_IRQ_STRICT` also reports a line with no reservation. A declared rate is
only sound if something makes it hold, so each line of the hardware image gets one of three:

| Line (handler) | What fires it | What bounds its rate | Status |
|---|---|---|---|
| TIMER0, BURTC (scheduler clock events) | an armed clock event: a radio launch, the ticker, a kernel timeout | radio arms: the admitted release each one serves (its handler budget is that job's). Kernel timeouts: not bounded today | radio: with the radio mapping. Kernel timeouts: see below |
| LLL doorbell, worker doorbell | a post to the LLL context or the worker | the post: the doorbell is part of the release it rings, its handler budget that job's | with the radio mapping (and G4: the worker admitted) |
| FRC (`isr_radio`) | a radio frame event | the radio event: at most one completion outstanding per event (the completion job armed for the event's duration) | with the radio mapping (G3) |
| GPIO even/odd (expander /INT on PC4) | an external edge | nothing: masked in its handler, unmasked by the expander-read job (S6) when it has run, so its rate is that job's | G3 (driver) |
| I2C0 | a byte of a transfer the driver started | the bus: one byte time at the devicetree clock (9 SCL periods) | declare from the devicetree |
| IADC | a conversion the supervisor started | the starter: S2, S3, S7 each start one conversion and wait for it | declare with the pulse chain |
| SMU fault | a security fault | none needed: any arrival is a fault | declare with a period no arrival can meet, so it is reported |
| WDOG0 warning | the watchdog | outside the scheduler by design (direct, zero-latency; the containment when the scheduler fails) | not reserved |

Kernel timeouts (`k_sleep()`, `k_timer`, delayed work, wait timeouts) arm the same clock event
line without an admitted release. Bounding them is part of mapping the product's threads onto
job kinds: each owner's timeouts become the periodic or one-shot releases of its jobs (the pump's
sleep is S10's period), and the host's (ATT, SMP, connection timers) are counted as one line
declared from the shortest timer the host arms.

### Job kinds of the safeties

The scheduler distinguishes one-shot, armed (sporadic) and periodic jobs
(`zephyr-dual-clock-scheduler.md`, "Job kinds"; vendor/zephyr 5504cebb61b). Arming is the only
point that can fail; activations, re-arms and periodic re-sleeps cannot.

- S1-S4 are one-shot jobs of a pulse chain admitted as a whole before the bridge closes. A
  refused chain is a pulse not fired.
- S5 (dead-man check), S6 (stop-input read), S7 (battery read while pulses fire) and S10 (the
  watchdog feed) are periodic, armed when the state that needs them is entered (S10 at boot, the
  others when a session is armed) and disarmed when it is left. Their freshness limits above are
  their periods plus their deadlines.
- The phase end (S1) gets a meta-IRQ priority of its own, above the radio's: its release from the
  scheduler clock event enters the scheduler, which switches to it at once from anything below
  it, the radio's cooperative bodies included. Nothing below it then blocks it, so `d_pe` is
  bounded by the release path and the interrupts admission declares, not by the radio's runs.

## Scheduler gaps (must be closed first)

Found in `vendor/zephyr/kernel/deadline_admission.c`, `include/zephyr/kernel.h`, the radio's
`hal/ll_threads.c` and the hardware image's interrupt table (`build/prod-suithw`).

| # | Gap | Effect today | Fix |
|---|---|---|---|
| G1 | **No budget enforcement.** Miss kinds are START, DELIVERY and UNSAT; a run that exceeds its budget is not detected. | An overrunning job silently eats every lower job's guarantee. | Kernel: when a budgeted run starts or resumes, arm the scheduler clock event for its remaining budget; expiry while it runs is a new `K_DEADLINE_MISS_OVERRUN` to the miss policy. Only the running job needs a timer. |
| G2 | **Interrupts are one blocking term.** Admission subtracts `CONFIG_SCHED_DEADLINE_ADMISSION_BLOCKING_NS` (58.1 us, the longest measured interrupt) once at the start of the check. Interrupts that arrive later in the window are not modelled, are not declared, and nothing enforces their length or rate. | A job's start can be delayed by any number of interrupts after the check. | Declare every interrupt source with a budget and a minimum inter-arrival; admission adds each source's interference over the window (ceil(window / T) x C). Enforce both: the top half's length by measurement and the arrival rate by masking the source until its job has consumed the previous arrival (at most one outstanding per source). |
| G3 | **Interrupts that run work instead of releasing a job.** In the hardware image: GPIO even/odd (expander /INT, submits work to the system work queue), I2C0 (transfer state machine), IADC (completion and callback), SMU fault. Interrupts that only acknowledge and release: the LLL and worker doorbells, TIMER0 and BURTC clock events. The radio's FRC interrupt (`isr_radio`) does register work and posts the completion job. The WDOG0 warning is deliberately outside the scheduler: it is the containment when the scheduler fails, never returns and spins until the reset. | Unaccounted work at interrupt level. | Each becomes a bounded top half that releases an admitted job (G2's declaration covers the top half). The warning stays as it is. |
| G4 | **ULL worker not admitted.** The meta-IRQ level-1 worker (`ll_threads.c`) has no budget: its measured 487.8 us is wall time including radio preemption. | A meta-IRQ level runs unaccounted. | Measure its own CPU time (excluding preemption) and admit it with that budget; G1 then enforces it. |
| G5 | **Admission sacrifices an already-admitted job.** When a new release makes the set infeasible, the job reported is the lowest-priority late one, which may be work admitted earlier. | Admitting a higher-priority pulse could break an admitted radio job (radio safe shutdown, which stops the output), instead of refusing the pulse. | Admission of a new request that would make admitted work late refuses the new request (unsat to its requester); it never revokes an admitted guarantee. (Decision 1.) |
| G6 | **Admission silently drops jobs when its table is full.** `sim_add()` returns NULL at `CONFIG_SCHED_DEADLINE_ADMISSION_JOBS` (12): `sim_collect()` stops collecting and `admission_check()` checks without the new job, reporting feasible. | An unproved release is admitted as feasible. | A full table is unsat (and a configuration error to report). |
| G7 | **Releases latched while the consumer runs are not checked.** `z_deadline_admission_posted()` returns feasible unless the release finds the consumer idle. | Coalesced work is unproved. | Check every release; a latched run is a job behind the current one. |
| G8 | **Scheduler-locked and work-queue sections are outside admission.** The pump's `k_sched_lock()` around a pulse (`hal.rs`) and system work-queue items are only covered by the blocking term. | Non-preemptible work of unknown length. | The pump's pulse becomes admitted jobs (above); work-queue items that a safety depends on (the expander's) become admitted jobs. |

Closed (vendor/zephyr, branch `safety-sched`; `tests/kernel/sched/deadline_admission` on
native_sim, 18/18, each new case failing without its change; the product's QEMU pairing e2e
passes on the result):

- G6, 254668a6e29: a check whose job table overflows refuses the request that asked for it.
- G7, ebe4d826bc0: every publication not admitted when it was armed is checked; a run latched
  behind the run in progress counts as work but its deadline only orders it.
- G5, 1d1c5cbaefc: a failed check re-runs without the request; if admitted work is feasible
  without it, the request fails (`-ETIME`, or a publication reported against its own event and
  not recorded: it neither releases its consumer nor joins a pending run). Admitted work is
  reported only when it is infeasible without the request.

### G1 and G2 together

Budget enforcement (G1) cannot come before interrupt accounting (G2): interrupt time is
charged to the thread it interrupts, so an enforced budget would count interrupts that
admission already accounts for separately, and report overruns that are not. The plan:

- **Declaration.** Each interrupt line in use gets a budget C and a minimum inter-arrival T
  (a kernel table filled by the drivers and the radio). An arrival sooner than T after the
  previous one, or a handler longer than C, is a miss (new kinds), reported like any other.
  Lines whose source can arrive faster than T (the expander's /INT, I2C) mask themselves in
  the top half until the job that serves them has run, which makes T their job's period.
- **Accounting.** Admission replaces the single blocking term with the interrupts' supply
  bound: in any interval of length t, threads get at least t - sum over lines of
  (floor(t / T) + 1) C. `sim_run()` advances time through that bound instead of 1:1, so every
  job's start is checked against the interrupts that can arrive before it, not one handler at
  the start of the check.
- **Enforcement.** Interrupt entry and exit (the Cortex-M ISR wrapper and the direct-ISR
  header) record the time spent in handlers. A budgeted run arms one scheduler clock event for
  its remaining budget when it starts or resumes and disarms it when it is switched out; on
  expiry the handler time accumulated during the run is subtracted and the event re-armed, or,
  if the run's own time has reached its budget, `K_DEADLINE_MISS_OVERRUN` is reported.
- **Evidence.** C and T for each line in the product (TIMER0 and BURTC clock events, LLL and
  worker doorbells, FRC `isr_radio`, GPIO, I2C0, IADC, SMU) from QEMU traces now, silicon
  owed. The WDOG0 warning stays outside: it is the containment for the scheduler.

**Cooperative runs break the shifting argument.** "Interrupts delay every job's start by at
most the interrupt time before it" holds for preemptive scheduling, not for the non-preemptive
EDF the scheduler uses inside one cooperative priority (the radio's meta-IRQ level). A
counterexample: without interrupts a short job Z starts before a long job L is released, then an
urgent job E runs, then L. Interrupts delay Z past L's release; EDF now picks L (its deadline is
earlier than Z's), L runs non-preemptively, and E, released just after, waits L's whole budget,
more than the interrupt time. So admission must be conservative there: per job j of a
cooperative priority, add one non-preemptive blocker, the largest budget among the
same-priority jobs released before j with a later deadline, and count the work released while j
waits (higher priority, or same priority and an earlier deadline):
x = s_j + B_j + I(x) + W(s_j, x), started late if the least fixed point x is past j's deadline.

**Open: interrupt inter-arrival is not bounded today.** The supply bound needs a minimum
inter-arrival T per line, and several lines have none: the TIMER0 and BURTC clock events re-arm
at whatever spacing the timeout queue asks for (the TIMER0 provider's floor is a 2 us lead,
`COUNTER_SILABS_TIMER_CLOCK_EVENT_MIN_LEAD_NS`), and the LLL and worker doorbells fire on every
post. With T near 0 the bound is unusable, and taking it from the check's start (interrupts
arriving synchronously at time 0) is too pessimistic for timed releases milliseconds ahead (the
radio's prepares). Closing G2 needs a rate that holds by construction for each line before any
bound is computed: a clock-event line serves only admitted arms (their releases are already
jobs); a doorbell is part of the post it rings (its cost belongs in the posted job's budget, not
a separate source); a peripheral line (GPIO, I2C, IADC, FRC) masks itself until its job has run.
Then the bound runs per busy period, not from the check's start. This is the next design step.

**QEMU miss count (scripts fail on any; `tests/radio-rx-port/qemu-miss-count.sh`).**

| Profile | Advertiser / Peripheral | Scanner / Central |
|---|---|---|
| product pairing (owo_suit + `sim/product-qemu.conf`, Central product profile), 60 s | 0 | 0 |
| radio-port connection (`prj-connection.conf`) vs Central, 60 s | 0 | 0 |
| radio-port default vs passive scanner (dual-e2e), 20 s | 0 | 0 |
| radio-port default vs active scanner (active-scan-e2e), 30 s | **1** | 0 |
| amateur Peripheral / Central (data identification), 120 s | 0 | 0 |

The `radio-tables.md` 7.79 connection misses no longer occur (the staged replies, 7.82, 7.87).
The one miss: a late start of `efr32_radio_completion_event`, 88-89 ms past its deadline, on the
radio-port default image, which runs the QEMU-only TIFS and RX-descriptor seams
(`tests/radio-port/prj.conf`). Its advertiser turns from ADV_IND to RX in software after the
completion, so it reaches RXSEARCH about 160 us after the scanner's SCAN_REQ began; the model's
descriptor path (`efr32bg22.c`, the synthetic RX offer) still hands that frame over at RXSEARCH,
which silicon would not receive, the RAC never enters RXFRAME, and the completion is posted only
at the next event's launch, carrying the previous event's deadline. The silicon path (staged
replies, the connection profile's advertiser) runs the same exchange with no miss, but the
active-scan script checks the TIFS seam itself. Fix at the source, next: retire the TIFS and
descriptor seams from the radio-port default image and the active-scan script (test the staged
path), or make the model's offer path refuse a frame that began before RXSEARCH. (A change that
dated an aborted advertising event's completion from its own launch removed the first instance
but would also hide this lateness, so it was not kept.)

The smoke and dual scripts also fail on `xxd`, which the dev shell does not provide (the
awk replacement exists only as an uncommitted edit in the main tree).

## Decisions for the user

1. **Who wins a conflict between a pulse and the radio.** Recommended: the pulse chain runs above
   the radio's priority, and admission refuses a request that would break admitted work (G5): a
   pulse that does not fit around admitted radio work is not fired, and a radio release that
   does not fit around an admitted pulse is refused to the radio (its miss policy, a radio safe
   shutdown, which stops the output). The alternative, the radio above the pulse chain, makes
   every phase end wait for radio runs of up to 249.1 us (`EFR32_RADIO_WCET_LLL_NS`), which
   `d_pe` = 50 us cannot admit.
2. **Phases too short to measure.** S2 needs width >= settle + `t_conv` (20 + 17.6 = 37.6 us in
   QEMU, silicon owed). Today a pulse needs one measured phase of two; strict S2 would refuse
   shorter phases. Recommended: require every phase measured, refuse shorter widths.
3. **RAM.** The pulse chain needs a consumer thread above the radio: one more meta-IRQ priority
   (`CONFIG_NUM_METAIRQ_PRIORITIES` 2 -> 3, the radio's levels moved down one) and a thread
   (224 B object plus its stack) in an image with 276 B left (`OWO_HW=ON`, 30724 of 31000 B).
   Candidates: the main stack (2048 B, peak not yet measured), or running the pump itself at
   the top priority only while a pulse chain is admitted.
4. **`d_pe` = 50 us** (one stock timer tick) as the requirement, which caps the width at 350 us.

## Implementation order

1. Kernel: G6, G7 (small, testable on native_sim), then G1 (budget timers), G5, G2 (interrupt
   declaration and accounting) with `tests/kernel` suites for each.
2. Radio: G4 (ULL budget measured), G3 for `isr_radio`.
3. Drivers: G3 for GPIO, I2C, IADC.
4. Product: the safety registry and the feed job (S5-S10), then the pulse chain (S1-S4) and the
   removal of `overrun_slack_us` and the scheduler lock; host tests for staleness (no feed) and
   for each safety's deadline; the stimulation campaign with EM2 on and locked out.
