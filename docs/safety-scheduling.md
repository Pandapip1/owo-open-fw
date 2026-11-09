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
