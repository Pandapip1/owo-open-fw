# Stimulation in simulation: the suit board model, stock vs open firmware

**Everything here is emulation.** No result below was measured on a suit; every electrical value
of the board model is a parameter, most of them unmeasured. Evidence tags as in
`stock-stim-path.md`: **[F]** read from code, **[I]** inferred, **[E]** observed in emulation,
**[U]** unknown (a parameter with a nominal value). Stock-firmware addresses, injection
recipes and raw stock recordings are kept outside this repository, next to the stock dump
(`stim-sim-2026-10-31/`); this page gives the method and the results in own words.

## Safety findings first

Current state (2026-11-01, master f8982fe plus the stim2 changes; full matrix below). Earlier
findings that are fixed keep a short entry.

1. **Open electrode and sense stuck at zero now stop the open firmware** [E]. Before, neither
   firmware stopped (stock notes "status 4" only for 0 < reading < level - 14 and keeps driving;
   the open firmware only logged a flag): 125 more live pulses in the model. Now an in-phase
   reading more than `OWO_STIM_UNDER_MARGIN` (14 levels, the stock firmware's own open-circuit
   tolerance) below the command locks every channel until the user starts again: one reading
   cannot tell an open pad from a dead sense path, and a dead sense path blinds the over-delivery
   trip on every channel. Sense stuck at zero: one live phase after the fault (the positive phase
   of the next pulse, 0.29 uC), then stopped; open pads: no current flows, stopped at the next
   pulse. Below level 15 a zero reading lies inside the tolerance band and cannot be told from a
   valid one (the first 0.375 s of a soft start to level 20); narrowing the margin needs a silicon
   measurement of the sense reading against the command.
2. **The open firmware now watches the battery; the stock firmware's filter hides a sag** [E/F].
   Open firmware: a session starts only at or above 7200 mV and stops below 6840 mV (the stock
   firmware's own 2 % power-down point), on single readings of PD3 against the IADC's internal
   1.21 V reference (independent of the supply that sags), before every session and once a second
   while pulses fire. Model: 6.5 V at 5.0 s stopped it at the next reading (2 live pulses, 24.5 ms
   after the step); 7.0 V (between the thresholds) let the running session go on, 6.8 V then
   stopped it; a session asked for at 7.0 V never started. Stock: its filter ignores any reading
   more than 500 mV below its running value [F], so a step from 7.6 V to 6.5 V and then 5.0 V is
   never acted on: it stimulated to the end of the run in every battery row. How the boost rail
   behaves as the cells sag is not modelled (the model's rail does not depend on the battery); a
   rail that sags out of compliance shows as under-delivery, which finding 1 now stops.
3. **External parts after an MCU reset: safe after 4.65 ms instead of 12.3 ms** [E]. The expander
   and the DAC keep their state across a reset: after a reset with the stage live (interrupts
   masked, so the watchdog warning could not run first) the boost supply stayed enabled and the
   gates floated until the application's safe-off. The open firmware now re-asserts the safe levels
   at Zephyr's first init level (EARLY, before the SoC's clock setup and every driver) with
   register writes only, every wait bounded by the cycle counter (no watchdog runs yet): gates
   driven 4.39 ms and boost supply off with the DAC cleared (/CLR) 4.65 ms after the reset, against
   11.2 / 12.3 ms before. About 2.6 ms of that is the stock bootloader before the application's
   first instruction; the C runtime's .bss/.data initialisation took 6.2 ms with byte-wise string
   routines and 1.7 ms with the word-wise ones that are now the product setting. Stock after the
   same reset: gates floating 81.0 ms, boost supply enabled 80.2 ms. Model times: the clocks the
   bootloader leaves and the flash timing on silicon differ.
4. **Phase lengths no longer depend on the sense sampler** [E]. The free-running 100 us sampler
   (a kernel timer plus the ADC interrupt, about 58 us of interrupts per conversion in the model)
   stretched a commanded 50 us phase to 118 / 123 us (+ / -). The supervisor now starts every
   conversion itself: a zero check before the pulse, one conversion 20 us into each phase, more
   only while 60 us of the phase remain, so no conversion's interrupt lands at a phase end; nothing
   samples between pulses. Model, 150 full-level pulses: phases 53.1 / 53.9 us median (maximum
   71.8 / 59.5 us: other interrupts, bounded by the overrun check), gap 65.9 us (commanded 50),
   median |net charge| 6.7 nC per pulse. Two supporting changes: the IADC driver no longer
   re-initialises the converter for a read with unchanged settings (vendor/zephyr; that alone took
   a phase from 50 to about 85 us), and a phase that an unrelated interrupt carries past its width
   before its conversion could start ends unmeasured (a pulse needs one measured phase, else a
   channel fault) instead of being taken for an ADC stall. **These figures depend on the model's
   IADC, which completes a conversion at its start command (no warm-up, no conversion time):
   re-run them once the IADC timing model lands**, and measure `OWO_STIM_SENSE_GUARD_US` and
   `OWO_STIM_SETTLE_US` on silicon.
5. **The power button does not stop the open firmware** [E]. Expander P0 low (stock: power-off
   request, stops within 78 ms) is not read by the open firmware: it stimulated to the end of the
   run. P0's meaning is inferred from the stock code only [I]. Open item.
6. **After a radio safe shutdown the open firmware keeps stimulating the frames it has** [E]:
   125 live pulses after the shutdown, to the end of the run, with no way left for the user to send
   Stop until the coverage window plus the 1.5 s dead-man ends (up to 27 s for a 25.5 s frame). A
   disconnect stops it at once. The radio module has no hook to the output yet. Open item: treat a
   radio safe shutdown like a disconnect.
7. **Expander faults leave the boost supply as it was** in both firmwares [E]: with the expander
   not acknowledging, or SDA stuck, stimulation goes on and the supply enable cannot be switched
   off afterwards (the open firmware's regulator calls and its warning-interrupt write fail the
   same way). Open item: an independent way to remove the rail.
8. **A stuck bridge is bounded by the watchdog in both firmwares** [E] (`docs/watchdog.md`). Open
   firmware, pump stopped inside a positive phase at level 20 (8.08 mA): with interrupts running or
   inside `irq_lock()`, live 38.1-38.2 ms after the fault time, until the warning (0.31 mC); with
   interrupts masked on the core (`cpsid i`), 56.8 ms, until the reset (0.46 mC). Stock, the same
   masked hang in its stimulation interrupt: bridge held 62.5 ms, until its reset; the stopped slot
   in these runs was a level-0 one (no current), so the stock rows show the time bound only.
9. **The suit's sense input was not routed to the IADC on silicon** [F, fixed]: the sense input
   moved to PD2 (an even C/D pin), but the board allocated only the C/D odd analog bus; the IADC
   would have flagged a port allocation error. Both buses are now allocated, as the stock firmware
   does. The QEMU model does not check the allocation, which is why the model never showed it.
10. Fixed earlier, kept for the record: the open firmware stopped after two soft-start pulses (the
    system timer could leave the kernel timeout unarmed; the work queue preempted a phase); it
    sampled the battery divider as its sense input; it drove pad 8 for wire channel 0 (stock: pad
    7); its over-delivery check skipped the conversions it most often saw. Stock keeps stimulating
    1.47 s after Stop (72 live pulses) and drives an open electrode at full level with the rail up.
    With `gate-float-on` every MCU reset is a shoot-through in both firmwares (a PCB property nobody
    has measured).

## Architecture: emulated board and host passthrough

The rule is "PCIe passthrough, through a translation layer": the per-silicon peripheral model
owns registers, timing and interrupts; its other side is a silicon-independent line that a board
model or a host device can take.

| Bus | SoC model (translation) | Line side | Board part (emulated) | Host passthrough |
|---|---|---|---|---|
| GPIO | `efr32-gpio.c` (modes, DIN, EXTI, TIMER routes, lock) | qdev lines `pin-out`/`pin-oe`/`pin-in` per pin | H-bridge gates, chip selects, latch, /INT | not implemented (gpio-sim / GPIO chip) |
| SPI | `efr32-usart.c` synchronous main mode | SSI bus `usart0.ssi`, `usart1.ssi` | DAC, switch chain | `-device spidev-host,bus=usart1.ssi,path=/dev/spidevB.C` |
| I2C | `efr32-i2c.c` leader | I2C bus `i2c0` | expander, 0x69 placeholder | `-device i2c-dev-host,bus=i2c0,address=0x20,path=/dev/i2c-N` |
| ADC | IADC single + scan in `efr32bg22.c` | `analog-source` QOM interface, sampled per conversion | output stage, battery | `-object iio-analog,dir=/sys/bus/iio/devices/iio:deviceN,map=PD02:in_voltage0` (the SoC link from a machine option is not wired yet) |
| UART | `efr32-usart.c` asynchronous | chardev | — | `-chardev serial,id=soc.usart0,path=/dev/tty...` (existing) |

The passthrough devices were built but **not tested**: the host has no spidev, no i2c-stub and
no gpio-sim, and its GPIO chips and IIO devices are real hardware, which nothing here may touch.
To test them on a disposable host, as root:
```
modprobe i2c-stub chip_addr=0x20          # /dev/i2c-N appears; also: modprobe i2c-dev
modprobe iio_dummy                         # then configfs: mkdir /sys/kernel/config/iio/devices/dummy/d0
modprobe gpio-sim                          # configfs bank under /sys/kernel/config/gpio-sim/
chmod a+rw /dev/i2c-N                      # for the user running QEMU
```

Other SoC changes: TIMER up-count PWM outputs with TOPB/OCB buffering; WDOG0 (it read EN as 0
and never fired); IADC scan queue. Qtests: `tests/qtest/efr32bg22-board-test.c`.

## The board model (`-M efr32bg22,board=owo-suit`, QOM `/machine/suit`)

| Item | Model | Evidence / status |
|---|---|---|
| Gates PB0..PB3 (HS A, HS B, LS A, LS B), active high | net = driven level, else floating | [F] pins, [I] polarity |
| Floating gate | off, or on with `gate-float-on` | [U] |
| DAC | LTC2634 class, 24-bit frames on CS = PA5 rising, /CLR = expander P5, channel `dac-channel` (A) sets the current; `full-scale-mv` 2500 | [F] frames; [I] part; [I] 2.5 V: makes DAC volts per level (19.0 mV) match the stock sense volts per level (19.15 mV) |
| Switch chain | 32-bit shift register, MISO = chain output, latch on PC3 rising (`transparent-low` for a latch-enable part); pad k in circuit if bits 16+k and k are set | [F] traffic; [I] wiring and latch type |
| Expander | PCF8575 class at 0x20, quasi-bidirectional, /INT on PC4; P2 enables the boost supply (`boost-enable-mask`) | [F] address, /INT; [I] P2 meaning |
| 0x69 | acknowledging placeholder (`unknown-0x69-present`) | [U] part unidentified |
| Output stage | current sink I = Vdac / `sense-milliohm` (47 ohm), compliance-limited by the rail; sense on PD2 = I x R | [I] structure; [U] value |
| Bridge | HS A + LS B positive, HS B + LS A negative, both LS on = pads shorted (stock "off"), same-leg HS+LS = shoot-through (rail crowbar, counted) | [F] states, [I] circuit |
| Pads | Rs (500 ohm) + Rp (2 kohm) parallel Cp (47 nF), textbook electrode/skin values | [U] |
| Boost rail | to `boost-target-mv` (60 V) with 2 ms while the supply is enabled; bleed 50 ms; 10 uF | [U] values; [I] enable semantics: stock toggles PC7 only for a 0.2 s kick, so PC7 cannot be what keeps the rail up (`boost-needs-pwm` for the other reading) |
| Battery | `battery-mv` 7600 through 33/133 on PD3 | [F] scaling; [I] two-cell pack: the stock 2 % level is about 6840 mV |

Faults (QOM properties, settable at run time or by the `script` property at exact virtual
times): `fault-open`, `fault-short` (pad masks), `fault-overcurrent-pct` (current above the
command: a failed regulator), `fault-sense-stuck-mv`, `fault-dac-ignore`, `fault-readback-xor`,
`fault-expander-nack`, `expander-ext-low` (buttons: P0 low = power-off request, P4 low 0.5 s =
self-test), `battery-mv`; on the SoC `/machine/soc/periph x-iadc-stall`,
`/machine/soc/i2c0 x-sda-stuck`, `x-nack-addr`. Script steps `pause` and `quit` stop the VM for
a debugger at a virtual time and end the run.

Recording: `vcd=<file>` writes nets, bridge state, switch, DAC, expander, rail, load current
and voltage, every sense and battery conversion, MCU resets, faults and script steps, in virtual
nanoseconds, bounded by `vcd-max-bytes`.

## How stimulation is commanded without Bluetooth

Both firmwares receive the same GATT payloads (widths, Start 0x25, frame, Stop 0x00) at the
same virtual times; a gdb session does the writes at pauses the board schedules.

- **Open firmware**: build with `CONFIG_OWO_SIM_INJECT=y` (test only, default off, warns at
  configure time). A RAM mailbox drained by a 1 ms system work item calls `write_cb` — the GATT
  write callback — with the characteristic's attribute, exactly as the host calls it after an
  ATT write passed the permission check; kind 0x100 calls the disconnect callback. Skipped: the
  link, ATT, SMP pairing and the authenticated-write permission, notification delivery.
- **Stock firmware**: the characteristic dispatcher the app's event handler calls after reading
  the written value back from the GATT database. The recipe one level up (database write plus
  the event) fails in QEMU because the stock Bluetooth stack never initialises there (the write
  returns "not supported"). Skipped: the same as above plus the database update and read-back.
  The stock app also reaches idle only after its boot calibration state, which needs a
  plausible battery level (it stayed in that state with the 3.9 V first used).
- Disconnect: the stock connection-closed handler restarts advertising, which fails in QEMU and
  ends in a halt loop; the watchdog then resets it. That reset is a model artifact.
- The open firmware's radio safe shutdown is entered by calling the kernel's miss report from
  gdb (`z_deadline_miss_report` with the radio's miss record; under LTO the radio's handler is
  inlined into it), on an image whose miss policy is the radio's handler (silicon timing):
  `sim/product-qemu.conf` only counts misses.
- The open firmware's outcomes are read from a test-only record (`owo_sim_outcomes`,
  `src/sim_inject.rs`): pulses fired, the last reading and the last fault, so a stop can be told
  from a reset or a hang without a console.

Tooling: `sim/scripts/run-suit-board.sh` (plain run), `stim_scenario.py` (QEMU + gdb, scenario
JSON), `stim_gdb.py` (driver), `adapter_ours.py` (open firmware adapter; the stock adapter lives
with the stock dump), `make_scenarios.py` (the scenario set), `stim_vcd.py` (pulse summary of
a recording), `stim_report.py` (comparison table).

## Results

Run 2026-11-01 on master f8982fe's model (vendor/qemu 58ec437, `build/qemu-new`) with the
stim2 changes. Stimulus: wire channel 0, level 20, 50 Hz, table widths 0x49 (phases 50 us, gap
50 us), 5 s frame from 4.0 s; faults at 5.0 s; runs end at 7.5 s. Open firmware: `owo_suit`,
`-DOWO_HW=ON`, `sim/product-qemu.conf`, `CONFIG_OWO_SIM_INJECT=y` (PM on); the two radio rows use
the same image without `sim/product-qemu.conf` (silicon timing; its miss policy is the radio's safe
shutdown, the QEMU profile only counts misses). Stock: the unit's flash. Times are virtual; pulse
timing reflects the model's instruction timing (icount shift 3) and its IADC, which converts at
its start command, not measured silicon timing. The hangs now mask interrupts on the core itself
in both adapters (`cpsid i`): the earlier gdb `$primask` writes reached the Non-secure bank and
masked nothing. The full per-run table (`stim_report.py`) and the recordings are kept with the
stock dump (`stim-sim-2026-11-01/`).

### Waveforms (no fault)

| | stock | open firmware |
|---|---|---|
| Pad | 7 | 7 |
| Round | 10 slots, one every 2.05 ms; slot period 20.52 ms (commanded 50 Hz) | one channel, 20.03 ms; 168 pulses 4.04-7.48 s |
| Bridge activity | every slot is pulsed biphasically, also at level 0 (DAC 0) | only the commanded channel |
| Phases + / gap / -, median (min-max), 150+ full-level pulses | 74.4 (69.7-79.1) / 44.8 / 106.9 (102.3-111.5) us | 53.1 (53.1-71.8) / 65.9 (65.9-105.4) / 53.9 (53.9-59.5) us; before the stim2 changes 118.0 / 122.7 / 120.8 us |
| DAC | code 622 = level 20 x 0x12FED/0x9C4, left set between pulses | soft start over 0.5 s, then 622; 0 between pulses |
| Peak current | 8.08 mA (model) | 8.08 mA |
| Net charge per pulse, median abs | 0.26 uC (the second phase is 44 % longer) | 6.7 nC (largest 144 nC: a phase stretched by an unrelated interrupt) |
| Sense conversions | stock's own, mid-phase | zero check before the pulse, one 20 us into each phase; none between pulses |

### Failure matrix

`live` = pulses with DAC > 0 and current after the fault; `safe after` = last live drive after
the fault; charge = delivered after the fault.

| Fault at 5.0 s | stock | open firmware |
|---|---|---|
| Stop command | 72 live pulses, safe after 1474 ms | no further pulse |
| Open electrode (pads 7, 8) | drives the open pads at full level (DAC 622) to the end, rail up; no stop | stopped at the first pulse (under-delivery, reading 0), output locked; no current flowed |
| Short (pads 7, 8) | stimulates on (current regulated; nothing to detect) | the same: stimulates on |
| Overcurrent (regulator fails, 3x) | trips after 1 pulse, 16.3 ms | trips in the positive phase of the first pulse (reading 56 against 20), 0.9 uC, supply off |
| Sense stuck high (1.5 V) | trips after 1 pulse, 17.0 ms | the zero check before the next pulse reads 75: no closure at all, output locked |
| Sense stuck at 0 | no detection, stimulates on (122 live pulses) | one live phase (0.29 uC), then stopped (under-delivery), output locked |
| ADC stalls | waits on the ADC in its main loop; watchdog reset 62.5 ms later, then a reset every 145 ms (boot loop), output off | the zero check times out before any closure: output locked; no reset |
| Switch read-back mismatch | stops at once (no live pulse) | stops at once (no live pulse), channel latched |
| DAC dead (ignores frames) | stays at the code it had (0 here): no current | no current flows: under-delivery at the first pulse, output locked |
| Expander NACK | stimulates on | stimulates on (the expander is not written during a session; switching the supply off afterwards fails) |
| I2C SDA stuck | stimulates on | stimulates on (as above) |
| Hang, interrupts masked (core spinning) | watchdog reset 62.6 ms after the fault | output already off between pulses; reset 58.3 ms after the fault |
| Same with `gate-float-on` | reset; gates float until its init | reset; gates float for 4.39 ms after it (shoot-through counted per reset in both) |
| Hang inside a positive phase, interrupts running | n/a (its pulses run in an interrupt) | live 38.2 ms, until the warning (0.31 mC); reset 60.6 ms after the fault |
| Same inside `irq_lock()` | n/a | live 38.1 ms, until the warning (the zero-latency warning still runs) |
| Same with interrupts masked | bridge held 62.5 ms until the reset (in a level-0 slot: no current) | live 56.8 ms, until the reset (0.46 mC) |
| After the reset | gates floating 81.0 ms, boost supply enabled 80.2 ms | gates driven at 4.39 ms, boost supply off and DAC cleared at 4.65 ms |
| Battery 6.5 V, then 5.0 V at 6.0 s | stimulates on to the end (its filter ignores the step) | stopped at the next reading: 2 live pulses, 24.5 ms |
| Battery 7.0 V, then 6.8 V at 6.0 s | stimulates on to the end | goes on at 7.0 V (above the stop threshold), stopped at 6.8 V (1047 ms after the first step) |
| Battery 7.0 V from boot | stimulates | the session never starts (below 7200 mV) |
| Power button (expander P0 low) | stops after 78 ms | no reaction: stimulates on (finding 5) |
| Disconnect | stops 57 ms later via a watchdog reset (model artifact) | stops at once (no further live pulse) |
| Radio safe shutdown (at 5.0 s, and at 4.04 s) | n/a | the radio stops; stimulation of the received frame goes on to the end of the run (finding 6) |
| Stop at 4.10 s (soft start) | 28 live pulses, 571 ms | no further pulse |
| Hang inside a pulse at 4.04 s (soft start) | bridge held 62.8 ms until the reset (a level-0 slot: no current) | live 35.5 ms at 0.40 mA (DAC 31), until the warning; reset 51.2 ms after the closure |

### Long product run with power management

`-DOWO_HW=ON` with `sim/product-qemu.conf` and the test mailbox, `CONFIG_PM=y`, 630 s virtual:
two sessions (4.0 s and 300.1 s, 248 pulses each), advertising throughout. Watchdog: 27084
clears, the longest interval 24.50 ms (warning at 46.9 ms), no warning, no reset; no supervisor
fault. The PM-on kernel timeout stall of `docs/watchdog.md` ("Blockers") does not occur.

## Open firmware in the model

Method: instruction-count stamps from a QEMU TCG plugin at chosen PCs (no guest change, and
the deterministic miss reproduced unchanged with it; a gdb breakpoint stop moves virtual time
by 250 us and changed the run, and an instrumented build no longer missed). Times below use the
model's M33 rate (1.84 cycles per instruction at 76.8 MHz, 23.96 ns), so they are model times.

- **Boot-time deadline miss** [E, root cause found, not fixed]: about 213 ms after reset the
  radio TX launch job is released from the TIMER0 clock-event interrupt (release to its
  start-by 56.3 us: its not-before instant 35.4 us after the release plus 20.9 us). The release
  path is quick: the post comes 15 us into that interrupt, the interrupt ends 8.6 us later,
  PendSV switches in 0.5 us, and the job reads the clock 1.8 us after that and starts spinning
  to its not-before. 9.3 us into the spin a second TIMER0 interrupt delivers the kernel's own
  system-timer event (announce, a thread made ready, the timer re-armed): 34.6 us. The job
  leaves its spin 3.8 us after that interrupt, past its start-by. Admission, the PendSV path and
  interrupt masking are not involved. The cause is that the not-before spin runs with interrupts
  enabled, and any interrupt longer than the slack after not-before (20.9 us with the silicon
  figures this image uses) that arrives near the end of the spin makes the start late; admission
  places its one interrupt allowance at the start of the check, not inside the job's window, so
  it never sees this. The same holds on silicon (TIMER0 clock-event interrupt 39.4 us, §7.76);
  in this model the release path is also slower than the silicon wake latency assumes. Options:
  hold interrupts locked from dispatch to the instant (the locked span then needs its own bound
  in the clock-event late tolerance), or count one interrupt inside every timed job's window
  in admission (the TX launch is then unsat by analysis on silicon). Not decided here.
- **Kernel timer stops after the safe shutdown** [E, fixed in vendor/zephyr]: the system timer
  driver arms the next tick boundary; when the service refused it, it retried once at "now +
  minimum lead + 1 cycle". Both attempts lose races with the cost of a clock read: the generic
  layer checks the lead against one read and the service reads again (in the stopped run its
  client held the boundary 36 ticks after the last announcement, generation equal to the
  generic event's, stage idle, armed false: accepted by the generic check, refused by the
  service), and the fallback is checked against a later read than the one it was computed from.
  Nothing was then armed, so the kernel was never announced to again unless a new first timeout
  re-armed it. The radio's traffic kept doing that; after its shutdown only work timeouts were
  left, and the timer died within ~250 ms. Not caused by the miss report. The driver now arms
  the first later boundary the service accepts; `tests/kernel/clock_event_timer` reproduces the
  loss on native_sim. With the fix the clock-event client stays armed and the injection mailbox
  is served after the boot miss.
- **Sense sampler gap** [E, the same timer bug]: the 3.5 ms gap after the second pulse in the
  diagnostic variant was a dropped arm: the 100 us sampler is a kernel timer (ADC context
  interval), so it stopped until something else re-armed the kernel's timeout; the freshness
  check then ended the session as designed. With the fix: no gap over 400 us in 28321 conversions.
- **Phase overrun after five pulses** [E, fixed in src/hal.rs]: with the timer fixed, the fifth
  pulse's second phase lasted 265 us and the supervisor's overrun check (width + 150 us) ended
  the session. The pump (main thread, priority 0) was preempted inside the phase by the system
  work queue (cooperative, priority -1; here the test mailbox's 1 ms item, in the product any
  host work item), on top of the sampler's interrupts. A pulse now runs with the scheduler
  locked: bounded by the supervisor's phase and gap limits, still preempted by interrupts and the
  meta-IRQ radio jobs.
- **The over-delivery check skipped most conversions** [E, fixed in owo-stim]: with the run
  sustained, a sense stuck at 1.5 V (level 75 against a trip at 28) never tripped: 120 more
  pulses. The sampler's conversion lands mid-phase, its interrupts hold the pump off, and the
  pump's next poll already finds the width elapsed; the loop left on that poll before judging the
  conversion. It now judges the newest conversion first, then ends the phase. Both the stuck sense
  and a 3x overcurrent now trip in the positive phase of the first pulse after the fault, supply
  off. Host test `overdrive_first_seen_at_the_last_poll_still_trips` fails without the change.
- **Phase lengths depended on the sampler's interrupts** [E, fixed]: the phase ended at the
  first 10 us poll that found the width elapsed, and the free-running sampler's interrupts (the
  TIMER0 clock event of the ADC interval timer, then the IADC completion, about 58 us together)
  decided when that poll ran: 118.0 / 122.7 / 120.8 us for a commanded 50 / 50 / 50 us. Options
  weighed: masking interrupts for the end of each phase (rejected: it delays the radio's meta-IRQ
  jobs past their start-by deadlines), a per-pulse hardware end of phase (set aside earlier because
  of output shaping), a hardware-timed sampler (the IADC's own timer; still one interrupt per
  conversion, landing anywhere). Chosen: the supervisor owns the sampler. It starts each
  conversion where it wants one (a zero check with the bridge open and the DAC at zero, then the
  first in-phase conversion `OWO_STIM_SETTLE_US` into each phase, further ones only while
  `OWO_STIM_SENSE_GUARD_US` of the phase remain) and polls for the result; the phase's clock
  starts before the bridge closes and the last delay lands on the width. Compatible with output
  shaping: everything stays in software, at points the supervisor chooses. Measured in the model:
  in-phase start to result about 35 us with the driver re-initialising the IADC each read (phase
  85 us), negligible once it keeps an unchanged configuration (`vendor/zephyr`
  `adc_silabs_iadc.c`); phases 53.1 / 53.9 us median, the bridge-off path and one poll step being
  the rest. The model converts at the start command; silicon adds the IADC warm-up and conversion
  time, which `OWO_STIM_SENSE_GUARD_US` (60 us, nominal) must cover.
- **Reset window** [E]: method: QEMU gdb breakpoints at chosen code addresses that set an expander
  input low through `qom-set`, which the board records with its virtual time (`expander_pins`).
  Cold reset to the application's reset handler 2.57 ms (the stock bootloader), to `z_prep_c`
  2.95 ms; .bss clear 4.44 ms and .data copy 1.79 ms with byte-wise string routines (1.7 ms both
  with word-wise); then the EARLY init level, where `owo_suit_early_safe_off` drives the gates and
  writes the expander (0.25 ms for the I2C transfer).
- **Power management** [E, resolved]: the watchdog investigation saw kernel timeouts stop
  ~600 ms after boot with `CONFIG_PM=y`. With the clock-event timer fix, a 630 s run with PM on
  and the watchdog active fed it 27084 times, the longest interval 24.50 ms, with no warning,
  reset or fault, through two stimulation sessions (Results).
- **Miss report and radio clocks** [F static, fixed]: the miss policy is global (late clock
  events and unsat releases reach it too), so it can run before `radio_setup()` enabled the
  radio bus clocks; gated, a radio read is a bus error and a write is dropped (§7.72), which
  would fault in the report or leave the forced-off writes undone. The safe shutdown now
  enables the radio bus clocks itself (idempotent SET aliases) before touching the RAC. Not
  reproduced in this model (the boot miss comes after `radio_setup()`).
- **Later failures after the safe shutdown are only counted** [E, open]: the radio's handler
  suspends the job that reported, inside the kernel's report, so the kernel's "report in
  progress" guard is never cleared; a later late clock event (one in the sustained run, likely
  from a gdb pause, which moves virtual time) was counted but not handed to the policy. The
  guard needs to tell a nested report from one whose reporter was suspended by the policy.

## Not measured (needed before any of this is a claim about the suit)

Sense resistor and the output stage's regulation and compliance; boost converter topology,
target voltage, time constants and what PC7 and P2/P3 really do; electrode/skin impedance;
gate polarity and the gates' state while the MCU pins float; switch part and latch behaviour;
DAC part (full scale, clear code); the 0x69 part; battery chemistry and divider, the pack's sag
under stimulation load (sets the gap between `OWO_STIM_BATTERY_START_MV` and `_STOP_MV`) and how
the boost rail behaves as it sags; the sense reading against the command (sets
`OWO_STIM_UNDER_MARGIN`); the output's rise time (`OWO_STIM_SETTLE_US`); the IADC's start-to-result
time in the pulse loop (`OWO_STIM_SENSE_GUARD_US`); the time from reset to the early safe-off with
the clocks the bootloader leaves; stock pulse timing on silicon (the model's instruction timing is
not calibrated for these code paths); the stock boost/stop/battery behaviour on a unit.
