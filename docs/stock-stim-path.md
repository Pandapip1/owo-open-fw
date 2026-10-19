# Stock stimulation path (fw 7.9.07)

Evidence tags: **[F]** read directly from code or data, **[I]** inferred, **[E]** obtained by emulating the image.
Addresses are flash addresses in fw 7.9.07 (app base 0x16000) unless marked RAM. The same text, with the
rest of the firmware analysis, is in `owo-suit-firmware-analysis.md` §10.

## SoC and architecture
- EFR32xG22-family Cortex-M33. **[F]** All peripheral literals match the xG22 headers (TIMER0-4, GPIO, USART0/1, I2C0/1,
  IADC0, CMU, EMU, RTCC, WDOG0, DCDC, MSC, PRS) and the vector table follows the xG22 IRQ order. Exact part number unknown.
- No RTOS. Stimulation is a hardware-timer ISR state machine: TIMER0 ISR at 0x3BAE9. **[F]**
- TIMER0 reload = 0x0249F000 / 0x4E20 = 1920 counts of 38.4 MHz => a 20 kHz (50 us) tick (init at 0x3BA1C). **[I]** (assumes
  the timer clock is 38.4 MHz and prescaler 1; not measured)

## Output stage
| Function | Evidence |
|---|---|
| **H-bridge**: PB0 = high side A, PB1 = high side B, PB2 = low side A, PB3 = low side B. **[F]** `0x3B6BA`: mode 0 off = PB2,PB3 high (both low sides on); mode 1 positive = PB0 + PB3 on; mode 2 negative = PB1 + PB2 on. Transitions turn the high sides off first. |
| **Shoot-through guard**: `0x3B6BA` refuses a direct 1<->2 change (state byte RAM 0x20001277 must be 0, else returns -2). **[F]** |
| **Amplitude DAC**: SPI on USART1, PA0 MOSI, PA4 MISO, PA8 SCLK, CS = PA5 (PA6 is a second select, purpose unknown). 24-bit frame `0x3F, hi, lo` with value = level * 0x12FED / 0x9C4 << 4. SPI mode 0, MSB first, 8 MHz. **[F]** (`0x3B7FE`, `0x3B74C`; USART route registers at GPIO+0x61C/0x63C) The frame matches the LTC262x family (command 0x3 write-and-update, address 0xF all DACs; 0x4 power-down), so the part is probably an LTC2634-class quad 12-bit DAC. **[I]** |
| **Channel switch**: SPI on USART0, PC0 MOSI, PC1 MISO, PC2 SCLK, CS = PC3. Four bytes per channel from the table below; shifted in with CS low, CS raised (latch), then 4 zero bytes clocked with CS **high** and the returned bytes compared with what was written; mismatch => fault. **[F]** (`0x444C8`) |
| **Boost / HV supply**: TIMER1 CC0 PWM on PC7, 4 kHz (0xFA0 passed at boot, 0x24B54), duty 50 % when enabled (0x3AFE2), 0 when off. **[F]** Purpose (boost converter) **[I]**. |
| **Expander**: I2C0 on PB4 (SDA) and PA7 (SCL), 8-bit write address 0x40, two data bytes (P0-P7 then P10-P17); this is the PCF8575 protocol. **[F]/[I]** P8..P13 = six LEDs (active low), P5 = DAC /CLR (pulsed at boot, 0x3B7E0), P2 and P3 set while running and cleared on power-down (0x24E94, 0x3B0EC); exact function unknown. **[F]/[I]** |
| **Feedback ADC**: IADC0 single conversion, input PD3 (posInput 0xB3), PD2 (0xB2) is the battery divider. Config (0x1A5EC): normal mode, OSR 2x, gain 1x, VDDX reference 3300 mV, 20 MHz source / 10 MHz ADC clock. level = raw * 0xA1220 / 0xEF510A - 3. **[F]** |
| **Button**: PC4, input pull-up, both-edge GPIO interrupt. **[F]** |

### Channel switch table (RAM 0x20000E6C, recovered by emulating the start-up code) **[E]**
Four bytes per entry, protocol channel i uses entry i (the frame bytes are copied to the slot table in order, 0x3C1CE).
```
 0: 01 00 01 00    4: 00 04 00 04    8: 00 20 00 20
 1: 00 10 00 10    5: 00 80 00 80    9: 02 00 02 00
 2: 00 40 00 40    6: 00 02 00 02   10: 04 00 04 00   (extra entry, test mode)
 3: 00 01 00 01    7: 00 08 00 08
```
The static image only holds this table in the compressed init data; it was read after running the reset handler in an
emulator (`unicorn`) up to the board-init call. The 4-byte pattern (one bit, repeated in both 16-bit halves) suggests two
daisy-chained 16-channel switches, one for each electrode of a pair. **[I]**

## Pulse sequence (state machine in the TIMER0 ISR) **[F]**
1. State 0: reload from the frame queue, validate the slot table (below).
2. State 1: set the DAC to the (ramp-scaled) level, select the channel through the switch (verify), compute the phase length.
3. State 2: bridge positive for `width` ticks; ADC sample at tick 2 or 3; compare (below). Bridge off.
4. State 3: gap (0, 1 or 2 ticks).
5. State 5: bridge negative for the same `width`; ADC sampled again. Bridge off.
6. State 6: idle until the slot period (`0x4E20 / (10 * freq)` ticks = 1/(10 f) s) has elapsed, then the next channel.
   Ten slots form one round at `f` Hz (f <= 100).
7. State 7: end of run; boost off after a delay; P3 toggled (HV discharge?). **[I]**

The slot's table width (`byte` from the widths table) is divided by 25 for the phase length and by 50 for the gap. At a
50 us tick that makes the phase 2 x the table value and the gap equal to the table value. **[I]** Only the first
width field of the table is ever used, for both phases; the second width (field B) is read by no code in the sequencer. **[F]**

## Interlocks in the stock firmware
1. Slot sanity every cycle: width <= 200, gap in {0, 50, 100}, per-channel limit < 0x7E else 0. (0x3BC3E)
2. Frequency clamp 1..100.
3. XOR integrity check over the level array; a mismatch zeroes all levels (error code 0x89). (0x3BCEA)
4. Mid-pulse ADC check. Measured > commanded + 8, or > 0x7E, turns the bridge off (both halves) and latches the channel
   in an error state. Another band (> +15) gives a second error code and measured below command - tolerance gives
   status 4 (open circuit / no contact). (0x3BE38, 0x3C000)
5. Channel switch read-back; mismatch is a fault. (0x444C8)
6. Shoot-through guard in the bridge routine. (0x3B6BA)
7. Bridge forced off between phases and on any error.

## Interlocks the stock firmware does NOT have (all implemented in `owo-stim`)
| # | Missing interlock | Where implemented |
|---|---|---|
| 1 | **Dead-man shorter than a frame.** A queued frame runs for its own `total` field (up to 25.5 s) with no check that the controller is still alive. I found no frame-starvation or link-loss stop in the sequencer; the disconnect path was not traced, so the stock may stop on disconnect. | `Stimulator::poll` (default 1.5 s) |
| 2 | **Hard output ceiling independent of the protocol.** The stock limit is whatever the client writes (up to 0x7D). | `Config::hard_max_level`, `CONFIG_OWO_STIM_HARD_MAX_LEVEL` |
| 3 | **Soft start.** The first pulse after idle is at full commanded level. | `Config::soft_start_us` |
| 4 | **Per-channel duty budget.** Nothing bounds average on-time (frequency x width) per channel. | `Config::max_duty_permille` |
| 5 | **Hard limit on phase and gap in the lowest layer.** The stock clamps (width <= 200) are applied to RAM that the integrity check can overwrite. | `Config::max_phase_us`, `max_gap_us` |
| 6 | **Charge balance by construction.** Both phases use one level and one width. The protocol carries a second width (field B); an asymmetric pair would put net DC through the skin. | `Stimulator::fire` |
| 7 | **Fault lockout.** The stock latches a channel only; repeated trips on different channels never stop the session. | `Config::fault_lockout` |
| 8 | **Supply off whenever the pulse queue is empty.** The stock switches the boost from the Start/Stop handlers and the end-of-run state (0x3AFD6 / 0x3AFEC), not per queue-empty. **[F]** weakly: the callers were read, the full state graph was not. | `Stimulator::release` / `safe_off` |
| 9 | **Supply, routing and DAC are cleared after every pulse**, not only at the end of a run. | `Stimulator::fire` |
| 10 | **Bounded interrupt-off window.** The pulse runs with interrupts off for at most 2 x max phase + max gap; the stock relies on ISR priorities. | `Hw::critical`, `fire` |
| 11 | **Sense freshness.** A stuck ADC cannot hold the bridge on: there is no wait: the newest ADC conversion (stamped by the ADC interrupt) must be fresh, else the bridge opens. | `owo-stim` (sense freshness), `owo-suit` (ADC sampler) |
| 12 | **Output only after explicit opt-in at build time**, behind a Kconfig symbol that states the code has not run on hardware. | `CONFIG_OWO_STIM_UNVALIDATED_HARDWARE` |

## Unknowns that block calling the output validated
- Pin map and sequencing are read from code, not from a schematic. A wrong pin could drive the wrong net.
- The meaning of expander P2/P3 and of PA6.
- Whether the stock sense calibration (RAM 0x20003D58, piecewise trim) is needed; this backend uses the uncalibrated formula.
- The real phase length (the 2 x scale above), the boost duty/frequency for each level (the stock firmware uses 50 % always),
  and the electrical limits of the output stage (isolation, maximum voltage/current).
- Pin states between reset and `Suit::new` (the gate drivers may float), whether the stage is current-limited in hardware.
