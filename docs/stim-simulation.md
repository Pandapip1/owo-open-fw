# Stimulation in simulation: the suit board model, stock vs open firmware

**Everything here is emulation.** No result below was measured on a suit; every electrical value
of the board model is a parameter, most of them unmeasured. Evidence tags as in
`stock-stim-path.md`: **[F]** read from code, **[I]** inferred, **[E]** observed in emulation,
**[U]** unknown (a parameter with a nominal value). Stock-firmware addresses, injection
recipes and raw stock recordings are kept outside this repository, next to the stock dump
(`stim-sim-2026-10-31/`); this page gives the method and the results in own words.

## Safety findings first

1. **The open firmware delivers no output in this model after two soft-start pulses** [E]. Its
   Bluetooth radio misses a start-by deadline during boot, the radio safe shutdown runs, and the
   tickless kernel later stops arming its system timer (section *Open firmware in the model*).
   With the miss report bypassed by a debugger patch (diagnostic only), it stimulates, but the
   sense sampler then shows a 3.5 ms gap and the supervisor's freshness check ends the session
   by its ordered safe-off (DAC 0, switch open, supply off). It fails safe, but a stimulation
   comparison beyond the first two pulses is not possible until the timer problem is fixed.
2. **The open firmware sampled the battery divider as its sense input** [F, fixed here]. The
   stock firmware reads feedback on PD2 and its x133/33 battery divider on PD3 (IADC positive
   inputs 0xB2/0xB3); `owo_suit_hw.overlay` sensed PD3. On a 2-cell pack PD3 sits near 1.9 V,
   about level 95, so every pulse would have tripped the over-delivery check; a sense signal on
   PD2 was never seen. `docs/stock-stim-path.md` had the two pins swapped.
3. **The open firmware drove a different electrode than stock for the same command**
   [E, fixed here]. Stock maps wire channel 0 to pad 7 (its wire-to-slot map, then the switch
   table); the open firmware used the table in wire order and drove pad 8. The overlay's
   `switch-routes` are now in the stock slot order; both firmwares then select pad 7.
4. **A stuck bridge lasts until the watchdog, in both firmwares** [E]. Open firmware: with the
   pump thread stopped right after it closed the bridge (interrupts running), the bridge stayed
   closed with current flowing for **2047 ms**, until the 2 s watchdog reset; its sense-freshness,
   overrun and interrupt-window interlocks all run in that same thread. Stock: with its
   stimulation interrupt stopped after closing the bridge, 62.3 ms, until its watchdog (PERSEL 8
   on the 32.768 kHz LFRCO, 62.5 ms). At 8 mA (the test level in the model) 62 ms is about
   0.5 mC of one-polarity charge, 2 s about 16 mC; a normal phase is under 1 uC.
5. **Stock keeps stimulating for 1.47 s after Stop** [E]: 72 live pulses after the Stop write,
   consistent with its stop state waiting for its 10 Hz counter to reach 15 [F weak].
6. **Stock leaves the high-voltage rail up and keeps driving an open electrode** [E]: with both
   pads of the channel open it drives the bridge at full level for the rest of the frame into
   no load; the rail stays at its full value. Re-contact gets full level at once.
7. **Neither firmware detects a sense input stuck at zero** [E stock / I open]: stock carries on
   at full level (nothing to compare against). The open firmware's freshness check does not
   catch a wrong value either; its hard level cap and duty budget still bound the output.
8. **Reset window**: if the gate drivers turn on while their pins float (`gate-float-on`, a PCB
   property nobody has measured), every MCU reset — including each watchdog reset above —
   shoots through both legs [E with that parameter]. Both firmwares share this window.

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
- The open firmware's radio safe shutdown is entered by calling its deadline-miss handler from
  gdb.

Tooling: `sim/scripts/run-suit-board.sh` (plain run), `stim_scenario.py` (QEMU + gdb, scenario
JSON), `stim_gdb.py` (driver), `adapter_ours.py` (open firmware adapter; the stock adapter lives
with the stock dump), `make_scenarios.py` (the scenario set), `stim_vcd.py` (pulse summary of
a recording), `stim_report.py` (comparison table).

## Results

Stimulus: wire channel 0, level 20, 50 Hz, table widths 0x49, 5 s frame from 4.0 s; faults at
5.0 s; runs end at 7.5 s. Times are virtual; pulse timing reflects the model's instruction
timing (icount shift 3), not measured silicon timing.

### Waveforms (no fault)

| | stock | open firmware (diagnostic variant) |
|---|---|---|
| Pad | 7 | 7 (after the route fix; 8 before) |
| Round | 10 slots, one every 2.05 ms; slot period 20.52 ms (commanded 50 Hz) | one channel; 76.7 ms between its two pulses |
| Bridge activity | every slot is pulsed biphasically, also at level 0 (DAC 0) | only the commanded channel |
| Phases (+ / gap / -) | 74.5 / 44.6 / 107.3 us: the second phase is 44 % longer (charge imbalance, the first phase includes the ADC sample code) | 87.6 / 75.9 / 76.9 us |
| DAC | code 622 = level 20 x 0x12FED/0x9C4 | soft start: codes 31, 124 |
| Peak current | 8.08 mA (model) | 1.61 mA (soft start) |
| Boost PWM | PC7 at 4 kHz, 50 %, only for 0.2 s after Start | 4 kHz, 50 % while the supply is on |
| Net charge per pulse | up to 9 nC (phase asymmetry) | 8 nC |

### Failure matrix

`live` = pulses with DAC > 0 and current; `safe after` = last live drive after the fault.

| Fault at 5.0 s | stock | open firmware |
|---|---|---|
| Stop command | 72 live pulses, safe after 1474 ms | (stopped earlier; early-stop row) |
| Open electrode (pads 7, 8) | keeps driving into no load, DAC 622, rail up to the end; no stop | not reached |
| Short (pads 7, 8) | keeps stimulating (current regulated; nothing to detect) | not reached |
| Overcurrent (regulator fails, 3x) | trips after 1 pulse, 16 ms, channel latched | not reached |
| Sense stuck high (1.5 V) | trips after 1 pulse, 16 ms | not reached |
| Sense stuck at 0 | no detection, stimulates on | not reached |
| ADC stalls | main loop waits on the ADC, watchdog resets 64 ms later, then a reset every 145 ms (boot loop); outputs off | not reached |
| Switch read-back mismatch | stops at once (no live pulse) | not reached |
| DAC dead (ignores frames) | DAC stuck at the value it had (0 here) | not reached |
| Expander NACK / SDA stuck | stimulates on (I2C errors never stop it) | not reached |
| Hang, main loop (interrupts alive) | stimulation continues until the watchdog reset 63.6 ms later | WDT reset 1976 ms after the hang (output already off) |
| Hang, inside the pulse with the bridge closed | bridge held 62.4 ms until the watchdog reset | early-hang row |
| Battery 6.5 V then 5.0 V | stimulates on to the end of the run (no shutdown within 2.5 s) | no battery monitoring |
| Power button (expander P0 low) | stops after 78 ms | not reached |
| Disconnect | stops 57 ms later, via a watchdog reset (model artifact, see above) | not reached |
| Radio safe shutdown | n/a | early row |

Inside the open firmware's active window (4.05-4.15 s in the model), both firmwares:

| Event | stock | open firmware |
|---|---|---|
| Hang inside a pulse with the bridge closed (t = 4.04 s; stock: the stimulation interrupt stops after the bridge routine closed it, interrupts masked; open: the pump thread spins right after closing the bridge, interrupts running) | bridge positive for 62.3 ms, until the watchdog reset | **bridge positive, current flowing, for 2047 ms**, until the 2 s watchdog reset; about 0.8 mC of one-polarity charge at the soft-start level of 1 (at the hard cap of level 40 it would be about 33 mC) |
| Stop (t = 4.10 s) | live pulses for another 1.47 s | no further pulse; supply off 42 ms after the Stop |
| After the reset | pins float until the reboot drives them (a shoot-through with `gate-float-on`) | same; the external DAC kept its code and the expander kept the boost supply on for 13 ms, until the rebooted firmware's safe-off |
| Radio safe shutdown (open firmware) | n/a | not run: under LTO the deadline-miss handler has no callable symbol; the boot-time miss above runs the same path |

**The open firmware's bound on a stuck bridge is its watchdog period** (2 s, `docs/watchdog.md`):
the sense-freshness check, the overrun check and the bounded interrupt-off window all run in
the pump thread, so a pump that stops with the bridge closed keeps it closed until the reset.
Stock bounds the same failure at 62.5 ms only because its watchdog is that short. Before
hardware: a bridge cut-off that does not depend on the pump (for example a hardware timer
that opens the bridge at the end of the phase, or a much shorter watchdog while armed).

The full per-run table (pulses before and after, widths, current, charge, end state) is
generated by `stim_report.py`; the recordings and the table are kept with the stock dump.

## Open firmware in the model

- **Boot-time deadline miss** [E]: about 213 ms after reset the radio TX job (cooperative,
  priority -16) starts 2229 hardware cycles after its release against a start-by of 1889
  cycles; the thread running at release was the Bluetooth long work queue (priority 10) inside
  the P-256 arithmetic of its key generation, preemptible and holding no lock. The miss policy
  runs the radio safe shutdown. Not root-caused; reproducible deterministically in this build.
- **Kernel timer stops after the safe shutdown** [E]: some 250 ms later the clock-event
  service's kernel-timeout client is idle with a deadline in the past while a timeout is still
  queued; nothing re-arms it, work items stop running (here: the injection mailbox). With the
  miss report patched out (debugger, diagnostic only) the kernel stays alive. One suspected
  mechanism was refuted: making the system-timer fallback arm retry did not change it. Not
  root-caused; on another QEMU build the miss report itself was reported to HardFault reading
  RAC registers — in this model those reads succeed (radio clocks on), so that symptom was not
  reproduced here.
- **Sense sampler gap** [E]: in the diagnostic variant the 100 us sense sampling showed a 3.5 ms
  gap after the second pulse; the third pulse's setup ended in the supervisor's ordered safe-off,
  the signature of its stale-sense/startup check. Every scenario therefore saw only two pulses.

## Not measured (needed before any of this is a claim about the suit)

Sense resistor and the output stage's regulation and compliance; boost converter topology,
target voltage, time constants and what PC7 and P2/P3 really do; electrode/skin impedance;
gate polarity and the gates' state while the MCU pins float; switch part and latch behaviour;
DAC part (full scale, clear code); the 0x69 part; battery chemistry and divider; stock pulse
timing on silicon (the model's instruction timing is not calibrated for these code paths); the
stock boost/stop/battery behaviour on a unit.
