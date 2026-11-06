# Stimulation in simulation: the suit board model, stock vs open firmware

**Everything here is emulation.** No result below was measured on a suit; every electrical value
of the board model is a parameter, most of them unmeasured. Evidence tags as in
`stock-stim-path.md`: **[F]** read from code, **[I]** inferred, **[E]** observed in emulation,
**[U]** unknown (a parameter with a nominal value). Stock-firmware addresses, injection
recipes and raw stock recordings are kept outside this repository, next to the stock dump
(`stim-sim-2026-10-31/`); this page gives the method and the results in own words.

## Safety findings first

Current state (2026-11-05, master 33f5e79 plus the safety3 changes, on the model with IADC
warm-up and conversion timing; full matrix below). Earlier findings that are fixed keep a short
entry.

1. **The power button now stops the open firmware** [E]. Expander P0 low is a power-off request
   in the stock firmware [F] (its button meaning is [I]); the expander's open-drain /INT is PC4,
   which the board described as a button until now. The output stage reads P0 as `stop-gpios`
   through the PCF857x driver, which reads the expander when /INT falls (`int-gpios`); a low P0,
   or an expander that cannot be read, stops the output in order (bridge, DAC, routing, supply)
   and locks it until P0 is high again and the user starts again. A press is acted on at once
   (no debounce: stopping is the safe direction; the stock firmware also acts on its first low
   sample). Model, press at 5.000 s between pulses: /INT read at +0.32 ms, boost PWM stopped at
   the next period, supply enables off at +1.27 and +1.98 ms (stock: 78 ms). A press inside a
   pulse is handled when the pulse ends (the expander is read from the system work queue, which
   a pulse's scheduler lock holds off; a pulse is at most 2 x max phase + max gap). Start while
   P0 is held and frames after a press are refused; after release, Start stimulates again. While
   armed the pump also checks the /INT level every `OWO_STIM_STOP_INPUT_POLL_MS` (100 ms, the
   stock 10 Hz) and reads the expander when /INT is asserted: an /INT left asserted by a failed
   read raises no further edge.
2. **A radio safe shutdown now stops stimulation** [E]. The radio module calls a registered
   function from its safe shutdown (`efr32_radio_shutdown_cb_set`, `radio_shutdown.h`; the
   module knows nothing of the output) right after the radio is forced off; the application
   then refuses frames and Start for good and the pump locks the output out. A pulse in flight
   completes (its charge stays balanced; it is bounded by the phase and gap limits), and no
   pulse follows. Model: shutdown between pulses, supply enables off +0.50 / +1.27 ms; shutdown
   inside a positive phase (the handler run on the pump's stack, stretching that phase to
   172 us), the negative phase completes, DAC 0 +0.22 ms, supply off +1.31 / +1.99 ms after the
   pulse. Before: 125 live pulses to the end of the run, no way to send Stop. A disconnect
   already stopped it at once.
3. **The radio already shuts down spontaneously at the start of a session in the model** [E],
   on the silicon-timing image (whose miss policy is the radio's safe shutdown): a late delivery
   of the kernel's timeout clock event, 91-100 us late with the pump running, about 25 ms after
   the frame arrived. Master: 4 of 4 runs (release 4.02505 s, 99.9 us late), stimulating on for
   the rest of the run with the radio dead; open firmware: about half the runs, each one stopped
   at once by finding 2. The source of the ~100 us (close to three 32.768 kHz periods, as an
   LF-synchronised register access would take) is open; if silicon shows it, every such session
   start shuts the radio down.
4. **A late conversion read as an open electrode** [E, fixed]. With the IADC timing the model
   samples a conversion when it completes; one started late in a phase (the loop delayed by an
   interrupt) completed after the bridge opened, read 0, and under-delivery locked the output:
   master's image stopped every session in the model after 90 pulses (1.8 s). A conversion still
   running when the bridge opens is now judged for over-delivery only (a partial sample can only
   read low) and does not measure the phase. Silicon's IADC converts after its warm-up, so it
   behaves like the model here.
5. **Phase lengths and the sampler timing on the IADC timing model** [E]. Start of a conversion
   to its completion callback 17.6 us (17.6-18.4, 64 conversions): the driver's start 8.7 us,
   then warm-up and conversion (RM: 5 TIMEBASE intervals, 5.08 us, and 10 ADC_CLK cycles,
   1.04 us, at the driver's CTRL 0x10260000, CFG OSR 2, PRESCALE 3, 38.4 MHz source) and the
   interrupt. The 60 us `OWO_STIM_SENSE_GUARD_US` covers it with margin; the 20 us
   `OWO_STIM_SETTLE_US` puts the converter's sampling about 34 us into the phase; both still
   need silicon measurements. Three timing faults the model showed, all fixed: an interrupt
   between the start of a phase and the bridge closing shortened the closed time (a 15.7 us
   phase, 0.33 uC unbalanced; the width is now timed from the closure), delays that run long
   (the model's 10 us busy-wait takes about 16 us) started the first conversion a whole step
   after `settle_us` so that its interrupt landed at the end of the phase (now it waits only up
   to `settle_us`), and a converter answering at once (native_sim) kept the loop from ever
   reaching the end of the width (now a step between a result and the next start). Now: 62.2 /
   63.1 us for a commanded 50 us (max 63.8), gap 65.9 us, median |net charge| 6.7 nC, max 13 nC
   (before: 52.1 / 52.9 us, max 93.3 us, max 0.33 uC). Since these runs the IADC's timing has
   been measured on silicon and the model follows it (radio-tables.md 7.89: `adc_read_async()`
   to its callback 24.5-24.8 us on the sense channel's configuration); not re-run here.
6. **Open electrode and sense stuck at zero stop the open firmware** [E]: one live phase after
   a sense stuck at zero (0.46 uC), none for open pads. Below level 15 a zero reading lies inside
   the 14-level tolerance; narrowing it needs a silicon measurement of the sense against the
   command.
7. **The battery** [E/F]: sessions start at or above 7200 mV and stop below 6840 mV on single
   readings against the IADC's internal reference; 6.5 V stopped it 24.2 ms after the step, 6.8 V
   (after 7.0 V) 1047 ms after the first step. The stock filter ignores the sag.
8. **Expander faults leave the boost supply as it was** in both firmwares [E]: with the expander
   not acknowledging, or SDA stuck, stimulation goes on and the supply enable cannot be switched
   off. Open item: an independent way to remove the rail. Related [F]: the vendor's blocking I2C
   transfer (`sl_i2c_leader_*_blocking`, used by Zephyr's `i2c_silabs` without DMA) masks
   interrupts for the whole transfer (about 0.3 ms per expander access at 100 kHz), and with SDA
   stuck until its 1000 ms timeout, past the watchdog: an earlier variant of finding 1 that read
   the expander every 100 ms while armed stopped the output in 44 ms on these faults but
   reset-looped (35 resets) with SDA stuck. Expander accesses during a session should wait for a
   driver that does not mask interrupts.
9. **External parts after an MCU reset: safe after 4.65 ms** [E], unchanged on the current
   model: gates driven 4.37-4.40 ms and boost supply off with the DAC cleared 4.65 ms after the
   watchdog reset (stock: 81.0 / 80.2 ms).
10. **A stuck bridge is bounded by the watchdog in both firmwares** [E] (`docs/watchdog.md`). Open
    firmware, pump stopped inside a positive phase at level 20: live 42.2 ms (interrupts running)
    and 42.5 ms (inside `irq_lock()`) until the warning (0.31 mC), 57.9 ms with interrupts masked
    on the core, until the reset (0.43 mC).
11. Fixed earlier, kept for the record: the sense input was not routed to the IADC on silicon
    (C/D even bus); the open firmware stopped after two soft-start pulses, sampled the battery
    divider as its sense input, drove pad 8 for wire channel 0, and its over-delivery check
    skipped the conversions it most often saw; free-running sampling stretched phases to 118 us.
    Stock keeps stimulating 1.47 s after Stop and drives an open electrode at full level. With
    `gate-float-on` every MCU reset is a shoot-through in both firmwares (a PCB property nobody
    has measured).
12. **The stock image no longer runs on the current model** [E]: on vendor/qemu 32e626a it never
    initialises its watchdog or writes its expander (no WDOG or I2C activity in the trace) and
    never stimulates; it did on 58ec437. The stock column below is the 2026-11-01 run on
    58ec437. Open item for the model.

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
  `sim/product-qemu.conf` only counts misses. The `mid-pulse` variant makes the call from a
  breakpoint inside a positive phase, as a failure detected in an interrupt during a pulse.
- The power button is the board's `expander-ext-low` property (bit 0 = P0 held low).
- The open firmware's outcomes are read from a test-only record (`owo_sim_outcomes`,
  `src/sim_inject.rs`): pulses fired, the last reading and the last fault, so a stop can be told
  from a reset or a hang without a console.

Tooling: `sim/scripts/run-suit-board.sh` (plain run), `stim_scenario.py` (QEMU + gdb, scenario
JSON), `stim_gdb.py` (driver), `adapter_ours.py` (open firmware adapter; the stock adapter lives
with the stock dump), `make_scenarios.py` (the scenario set), `stim_vcd.py` (pulse summary of
a recording), `stim_report.py` (comparison table).

## Results

Open firmware: run 2026-11-05 on master 33f5e79's model (vendor/qemu 32e626a, with the IADC
warm-up and conversion timing of e7242f1) with the safety3 changes. Stock firmware: the
2026-11-01 run on vendor/qemu 58ec437 (instant conversions), because the stock image no longer
runs on the current model (finding 12). Stimulus: wire channel 0, level 20, 50 Hz, table widths
0x49 (phases 50 us, gap 50 us), 5 s frame from 4.0 s; faults at 5.0 s; runs end at 7.5 s. Open
firmware: `owo_suit`, `-DOWO_HW=ON`, `sim/product-qemu.conf`, `CONFIG_OWO_SIM_INJECT=y` (PM on);
the radio rows use the same image without `sim/product-qemu.conf` (silicon timing; its miss
policy is the radio's safe shutdown, the QEMU profile only counts misses). Times are virtual;
pulse timing reflects the model's instruction timing (icount shift 3) and its IADC (RM formulas,
not measured silicon timing; the model has followed silicon since radio-tables.md 7.89). Runs on this model are not reproducible run to run (the same
scenario gave different schedules); the radio rows were repeated (finding 3). The per-run
recordings are kept with the stock dump (`stim-sim-2026-11-05/`).

### Waveforms (no fault)

| | stock (2026-11-01) | open firmware |
|---|---|---|
| Pad | 7 | 7 |
| Round | 10 slots, one every 2.05 ms; slot period 20.52 ms (commanded 50 Hz) | one channel, 20.04 ms; 173 pulses 4.04-7.49 s |
| Bridge activity | every slot is pulsed biphasically, also at level 0 (DAC 0) | only the commanded channel |
| Phases + / gap / -, median (min-max), 150 full-level pulses | 74.4 (69.7-79.1) / 44.8 / 106.9 (102.3-111.5) us | 62.2 (62.2-63.8) / 65.9 (65.9-65.9) / 63.1 (63.1-63.1) us |
| DAC | code 622 = level 20 x 0x12FED/0x9C4, left set between pulses | soft start over 0.5 s, then 622; 0 between pulses |
| Peak current | 8.08 mA (model) | 8.08 mA |
| Net charge per pulse, median abs | 0.26 uC (the second phase is 44 % longer) | 6.7 nC (largest 13 nC) |
| Sense conversions | stock's own, mid-phase | zero check before the pulse, one per phase started `OWO_STIM_SETTLE_US` after the closure, completing about 42.6 us after it; none between pulses |

### Failure matrix

`live` = pulses with DAC > 0 and current after the fault; `safe after` = last live drive after
the fault; charge = delivered after the fault.

| Fault at 5.0 s | stock (2026-11-01) | open firmware |
|---|---|---|
| Stop command | 72 live pulses, safe after 1474 ms | no further pulse |
| Open electrode (pads 7, 8) | drives the open pads at full level (DAC 622) to the end, rail up; no stop | stopped at the first pulse (under-delivery, reading 0), output locked; no current flowed |
| Short (pads 7, 8) | stimulates on (current regulated; nothing to detect) | the same: stimulates on |
| Overcurrent (regulator fails, 3x) | trips after 1 pulse, 16.3 ms | trips in the positive phase of the first pulse (reading 56 against 20), 1.4 uC, supply off |
| Sense stuck high (1.5 V) | trips after 1 pulse, 17.0 ms | the zero check before the next pulse reads 75: no closure at all, output locked |
| Sense stuck at 0 | no detection, stimulates on (122 live pulses) | one live phase (0.46 uC), then stopped (under-delivery), output locked |
| ADC stalls | waits on the ADC in its main loop; watchdog reset 62.5 ms later, then a reset every 145 ms (boot loop), output off | the zero check times out before any closure: output locked; no reset |
| Switch read-back mismatch | stops at once (no live pulse) | stops at once (no live pulse), channel latched |
| DAC dead (ignores frames) | stays at the code it had (0 here): no current | no current flows: under-delivery at the first pulse, output locked |
| Expander NACK | stimulates on | stimulates on (the expander is not accessed during a session; switching the supply off afterwards fails) |
| I2C SDA stuck | stimulates on | stimulates on (as above) |
| Hang, interrupts masked (core spinning) | watchdog reset 62.6 ms after the fault | output already off between pulses; reset 58.5 ms after the fault |
| Same with `gate-float-on` | reset; gates float until its init | reset; gates float for 4.4 ms after it (shoot-through counted per reset in both) |
| Hang inside a positive phase, interrupts running | n/a (its pulses run in an interrupt) | live 42.2 ms, until the warning (0.31 mC); reset 57.9 ms after the fault |
| Same inside `irq_lock()` | n/a | live 42.5 ms, until the warning (the zero-latency warning still runs) |
| Same with interrupts masked | bridge held 62.5 ms until the reset (in a level-0 slot: no current) | live 57.9 ms, until the reset (0.43 mC) |
| After the reset | gates floating 81.0 ms, boost supply enabled 80.2 ms | gates driven at 4.4 ms, boost supply off and DAC cleared at 4.65 ms |
| Battery 6.5 V, then 5.0 V at 6.0 s | stimulates on to the end (its filter ignores the step) | stopped at the next reading: 2 live pulses, 24.2 ms |
| Battery 7.0 V, then 6.8 V at 6.0 s | stimulates on to the end | goes on at 7.0 V (above the stop threshold), stopped at 6.8 V (1047 ms after the first step) |
| Battery 7.0 V from boot | stimulates | the session never starts (below 7200 mV) |
| Power button (expander P0 low) | stops after 78 ms | no further pulse; supply enables off 1.27 / 1.98 ms after the press |
| Button pressed and held, Start at 5.5 s, frame at 5.6 s | (powers off) | Start and frame refused: no further pulse |
| Button released at 5.3 s, Start at 5.5 s, frame at 5.6 s | (powers off) | stimulates again from the new frame |
| Button pressed and released before the frame (3.8 / 3.9 s) | (powers off) | the frame does not start a session |
| Disconnect | stops 57 ms later via a watchdog reset (model artifact) | stops at once (no further live pulse) |
| Radio safe shutdown between pulses | n/a | no further pulse; supply enables off 0.50 / 1.27 ms after it (before: stimulated to the end of the run) |
| Radio safe shutdown inside a pulse | n/a | the pulse completes (both phases), no further pulse; DAC 0, switch open, supply off 0.22 / 0.50 / 1.99 ms after the pulse |
| Radio safe shutdown at 4.04 s (soft start) | n/a | no live pulse |
| Stop at 4.10 s (soft start) | 28 live pulses, 571 ms | no further pulse |
| Hang inside a pulse at 4.04 s (soft start) | bridge held 62.8 ms until the reset (a level-0 slot: no current) | live 38.2 ms at 0.40 mA (DAC 31), until the warning; reset 53.9 ms after the fault |

### Long product run with power management

2026-11-01, on vendor/qemu 58ec437: `-DOWO_HW=ON` with `sim/product-qemu.conf` and the test
mailbox, `CONFIG_PM=y`, 630 s virtual:
two sessions (4.0 s and 300.1 s, 248 pulses each), advertising throughout. Watchdog: 27084
clears, the longest interval 24.50 ms (warning at 46.9 ms), no warning, no reset; no supervisor
fault. The PM-on kernel timeout stall of `docs/watchdog.md` ("Blockers") does not occur.

## Open firmware in the model

Method: instruction-count stamps from a QEMU TCG plugin at chosen PCs (no guest change, and
the deterministic miss reproduced unchanged with it; a gdb breakpoint stop moves virtual time
by 250 us and changed the run, and an instrumented build no longer missed). Times below use the
model's M33 rate (1.84 cycles per instruction at 76.8 MHz, 23.96 ns), so they are model times.

- **Boot-time deadline miss** [E, root cause found; superseded by radio-tables.md 7.82, which
  removed the not-before spin]: about 213 ms after reset the
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
  the rest. The model converted at the start command then (it times conversions now: next
  item); silicon adds the IADC warm-up and conversion
  time, which `OWO_STIM_SENSE_GUARD_US` (60 us, nominal) must cover.
- **IADC timing in the pulse loop** [E, 2026-11-05]: method: a diagnostic image (not kept)
  recorded the cycle counter at the supervisor's conversion start, after the driver's start
  returned and in the completion callback, read by gdb after the run; and the converter's
  registers at run time (CTRL 0x10260000: HSCLKRATE 1, TIMEBASE 38, WARMUPMODE normal, so a
  full warm-up before every conversion; CFG0 OSRHS 0; SCHED PRESCALE 3; IADCCLK from
  EM01GRPACLK). 64 conversions: start to driver return 8.70 us (8.70-8.73), driver return to
  callback 8.88 us (8.85-9.66), start to callback 17.6 us (17.6-18.4). The model's 10 us
  busy-wait takes about 16 us. The model samples the input when a conversion completes;
  silicon's incremental converter integrates it over the conversion after the warm-up, so a
  conversion that ends after the bridge opened reads low on both.
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
- **Later failures after the safe shutdown are only counted** [E, fixed in vendor/zephyr,
  7ea7070: a report whose thread is suspended or dead is over]: the radio's handler
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
time in the pulse loop (`OWO_STIM_SENSE_GUARD_US`; the driver's read path alone is measured,
radio-tables.md 7.89); the time from reset to the early safe-off with
the clocks the bootloader leaves; stock pulse timing on silicon (the model's instruction timing is
not calibrated for these code paths); the stock boost/stop/battery behaviour on a unit; what
expander P0 is (button or a charger's power-good) and whether the PCF8575-class part asserts /INT
on output writes (the model does; the NXP datasheet says input changes only); whether the
~100 us late timeout delivery of finding 3 happens on silicon.
