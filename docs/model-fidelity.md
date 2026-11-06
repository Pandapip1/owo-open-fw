# QEMU model fidelity

Every hardware finding recorded in this project, checked against the QEMU
model of the EFR32BG22 (`vendor/qemu`: `hw/arm/efr32bg22.c` and its blocks,
`hw/misc/efr32-dap.c`, `armv8m-debug.c`, `jlink.c`, `swd-remote-bitbang.c`,
the suit board models, RADIOAES).  Audit of 2026-10-31, vendor/qemu branch
`agent/model-audit`.

Rules the model follows: everything QEMU is meant to model is modelled,
timing included (RF propagation and physics are out of scope); each
behaviour and timing rests on evidence (silicon measurement, the reference
manual `scratch/refdocs/efr32xg22-rm.txt`, the datasheet, public Silicon
Labs register models); the model follows silicon, not what the firmware
expects; a bug in a tool is fixed in the tool.

Sources audited: `docs/radio-tables.md` (sections 1 to 7.81),
`docs/debug-port-model.md`, `suit-board.md`, `stim-simulation.md`,
`watchdog.md`, `crypto.md`, `ota.md`, `hardware-bringup.md`,
`stock-stim-path.md`, `sim/README.md`, the local `TODO.md`, and the silicon
recovery logs in `scratch/silicon-rx/` (`recover*.sh`, `recover*.log`) and
`scratch/recover-step1.log`.

Status column: **yes** modelled as measured; **partial** modelled in part
(the Gap column says what is missing); **no** not modelled; **contradicts**
the model disagrees with the evidence.  "(audit)" marks a row this audit
changed.  [I] inferred, [U] unmeasured, as in the source documents.

## Summary

240 findings: 49 from radio-tables 1-7.59 (table A), 117 from 7.60-7.81 and
crypto.md (table B), 74 from the other documents, TODO.md and the recovery
logs (table C).

| | yes | partial | no | contradicts | n/a |
|---|---|---|---|---|---|
| before this audit | 139 | 48 | 44 | 8 | 1 |
| after this audit | 152 | 45 | 41 | 1 | 1 |
| after the model-gaps pass (2026-11-01) | 156 | 45 | 38 | 0 | 1 |
| after the model-gaps2 pass (2026-11-05) | 159 | 45 | 35 | 0 | 1 |

The model-gaps pass (vendor/qemu `agent/model-gaps`, section "Model-gaps
pass" below) closed C63 and B71, B79 and B115, and moved B76 and B78 to
partial; C30 gained the kit's PART.  Model defects it fixed: per-SoC resets
in the dual machine, the default DEVINFO identity.

17 rows changed: 12 gaps or contradictions closed, 2 partly closed (A35,
B95), 2 already-modelled rows now pinned by a qtest and a silicon citation
(B33, B64), 1 reclassified on evidence already in the docs (A42).  The remaining
contradiction is C63 (AES-192 accepted; this BA411E has no 192-bit keys,
but what the hardware does with the mode is unknown).  Further fixes made
on the way, outside the tables: the flaky qtest, SEQRESET resetting the
M0+'s NVIC, and the e2e script's corrupt first lines.

Tests after the audit: `efr32bg22-test` 101 (98 before), `efr32bg22-rf-test`
29 (27), `efr32bg22-dap-test` 13 (9), `efr32bg22-board-test` 7,
`test-jlink-swd-io` 3; `tests/debug-port/run-e2e.sh` over remote_bitbang
14 checks (11 before); dual-machine `scratch/sched/dual.sh b-adv b-scan 3 60`:
0 late/unsat/shutdown lines, 127 SCAN_REQ and 127 SCAN_RSP in 12.7 virtual
seconds (the pre-audit binary, run alongside: 128/128 in 12.8 s).

## What this audit changed in the model

vendor/qemu `agent/model-audit`, one commit per area:

1. **RXDIS / FORCEDISABLE in RXFRAME** (B33, B64; radio-tables 7.80).
   RXDIS lets the RAC finish the frame; FORCEDISABLE ends RXFRAME at once and
   now also drops the frame in reception.  qtest `rf/released-rx-frame-disables`.
2. **seq-sleeping-follows-wfi in virtual time** (TODO: flaky, 1 in 6-8 runs
   under load).  It read RAC.STATUS after 20 ms of real time; it now runs
   under `-icount sleep=off` and reads at a checkpoint the M33 marks after a
   fixed countdown.  24/24 with 8 copies in parallel.
3. **Bus-clock gates and the FSRCO rate** (B41, B95, B98; 7.72, 7.73).
   SEQRAM at 0xb0000000 faults with RADIOCLKCTRL.EN clear; RTCC faults with
   CMU_CLKEN0.RTCC clear; TIMERs on FSRCO count at the measured 19.807 MHz.
4. **nRESET and the debug-port wedge** (C17, C18, C21; 7.80).  See below.
5. **Lockup and EMU_RSTCTRL** (A34, A35, C41; 7.58, 7.59, RM 12.5.14).
   QEMU used to `cpu_abort()` the whole emulator on an M-profile Lockup; the
   core now stops (no execution, interrupts or exception entry) until a reset
   or a debug halt, DHCSR.S_LOCKUP reports it, and the NVIC raises a LOCKUP
   output.  The M0+ stays locked until SEQRESET or a reset, as on silicon;
   SEQRESET now resets the M0+'s NVIC with the core (an active HardFault
   survived it before).  EMU_RSTCTRL (reset 0x10405) decides whether
   SYSRESETREQ (SYSRMODE), WDOG0 (WDOG0RMODE) and an M33 Lockup (LOCKUPRMODE,
   reset 0) reset the device; it survives soft resets and returns to its
   reset value on a pin reset.  qtests `m33-lockup`,
   `rstctrl-blocks-sysresetreq`, `seq-lockup-until-seqreset`.
6. **SYNTH.STATUS and the RAC.EN ordering rule** (B5, B37, C47; 7.42, 7.60,
   7.71, 7.80).  SYNTHREADY reads 1 from reset; enabling RAC no longer needs
   PROTIMER.EN (BUFC.EN first stays, [I]).
7. **PROTIMER counter** (A39, B38; 7.32, 7.33, 7.71).  Stopped at 0 from
   reset; CMD bit 0 starts it ([I] which write of stock's sequence is the
   start).
8. **Per-channel receive hook** (7.80).  No behaviour change: a TODO at the
   frame-detection point records the ch37/38/39 silicon result while it is
   root-caused on silicon.

Superproject: `tests/debug-port/run-e2e.sh` gains step 7 (wedge, then the
EK's recovery, `tests/debug-port/wedge-recover.tcl`), and its first two
lines, a pasted fragment ahead of the shebang since c1dd7d5, are removed.

## The debug-port wedge

**Evidence** (radio-tables 7.80, TODO.md, `scratch/silicon-rx/recover2.sh`
and its logs, `scratch/recover-step1.log`; EK, J-Link OB 000440200548):

- An image whose sequencer enabled RAC clock retiming in its RX tune
  (XORETIMECTRL and DIGCLKRETIMECTRL ENRETIME, bit 0 of each, with stock's
  RXSEARCH limit fields) wedged the debug port within milliseconds of every
  boot: DPIDR still read, debug power-up, the AHB-AP and the SE's DCI AP no
  longer answered.  This state is recorded in TODO.md and 7.80 only; no log
  of it survives, and the exact AP acknowledge was not recorded.
- It came back after every pin reset because the image enabled retiming
  again at its first scan window.  Each release of nRESET gave a working
  window: in `recover2-fpb3.log` the DHCSR halt plus FPB comparator landed
  on the third back-to-back attempt and the core halted.
- With nRESET held the SW-DP did not answer at all ("Error connecting DP:
  cannot read IDR"), held by the button (`recover-step1.log`) and by the
  J-Link (`recover2-srsttest.log`, `-auto`, `-fpb2`, `-fpb3`);
  hardware-bringup.md: "SWD disappears while reset is asserted".
- The J-Link OB drives the EK's nRESET: a 100 ms SRST pulse gave one fresh
  boot banner on the VCOM (`recover2-srsttest.log`, one trial).  The model
  and docs had said no nRESET is wired.
- Stock enables the same retiming (its `fn_0xa76` clears ENRETIME, 7.7) and
  stays debuggable (AP3 captures of stock in RX, 7.64).  Stock runs SYSCLK
  from the HFXO, the open image from HFRCODPLL at 76.8 MHz; 7.80 names that
  as the remaining difference.

**Model.**  The mechanism is unknown; the model reproduces what a probe
sees, so recovery procedures can be rehearsed against QEMU.

- Trigger [I]: both ENRETIME bits set while SYSCLK is not the HFXO
  (`efr32bg22.c` `efr32_rac_retime_update`).  The limit fields are not part
  of it (only stock's values were tried); the wedge starts at the write
  ("within milliseconds" was not resolved further).
- Wedged (`efr32-dap.c` `dap_debug_wedge`): DPIDR, CTRL/STAT and the other
  DP registers answer; CDBGPWRUPACK and CSYSPWRUPACK never follow their
  requests; every AP access, AP0 and AP1 alike, is a FAULT with STICKYERR
  ([I]: the real ack is unknown).  It lasts until a reset ([I]: whether
  withdrawing ENRETIME ends it is unmeasured); an image that enables
  retiming at boot wedges the port again after each reset, as on silicon.
- nRESET: the J-Link's CLEAR_RESET/SET_RESET and remote_bitbang's SRST
  ('r'..'u') drive the SoC's RESETn.  While it is held the DP never drives
  SWDIO and the M33 is held powered off; on release the part starts from its
  vector table with EMU_RSTCAUSE.PIN set and EMU_RSTCTRL at its reset value
  (a hard reset).  [I] The DP comes out of the hold in its power-on state.
- Rehearsal: `tests/debug-port/run-e2e.sh` step 7 wedges the port through
  the AHB-AP, sees the AP refuse, then runs `recover2.sh`'s "fpb" procedure
  over remote_bitbang (SRST held during init: "cannot read IDR"; release;
  back-to-back `dap init` + DHCSR halt + FPB on `z_arm_reset`; S_HALT).
  qtests `dap/retime-wedges-debug-port` (wedge, then pin reset, attach,
  halt), `dap/retime-on-hfxo-sysclk-keeps-debug-port`,
  `dap/nreset-holds-dp-silent`, `dap/nreset-holds-core`.

Not modelled: the AP refusing the first attempts right after release
(C19: two failed tries in `recover2-fpb3.log`, duration and ack not
recorded); the time from boot to the wedge.

## Remaining gaps, and why

Transmit power (docs/tx-power.md): MODEM.RAMPCTRL and RAMPLEV have their
public reset values and masks (qtest modem-pa-ramp-registers), but the
medium carries no power, so the PA level the firmware programs changes
nothing else in the model; there is no silicon measurement to model it
from.

Measured and modelable, not done in this audit:

- **Bus contention and per-target access costs** (B71, B76-B79): done in
  the model-gaps pass (below), except the M33's SRAM traffic (B77).
  Related edge offsets that are probably emergent from the 500 ns
  per-core rounds: EOF to SCAN_RSP sync 181.5-181.7 us vs 180.1-180.9 us
  (B57), TX2RX 124.5-125.5 vs 123.4-123.7 us (B51), TXWARM spread (B48).
- **FRC.STATUS.ACTIVETXFCD** during and after TX (A21) and BUFC TX
  READOFFSET advancing as FRC consumes (A23): sampled on silicon, not yet
  modelled.
- **Access-address match and CRC failures** (B15, B20, B21): the medium
  would carry the transmitter's AA and CRC init; the per-bit CRC result is
  [I] in the doc.
- **DWT data watchpoints** (A43) and **the M0+ on AP3** (A45, C3, C25,
  C26): architectural; needed to rehearse the AP3 capture workflows.
- **Debugger effects** (C22-C24): WFI not gating the M33 clock while
  attached, EM2 never entered, side-effect-free debug reads, per-access SWD
  cost on the cores.
- **EM2/EM3** (B69, B70, TODO energy modes): no energy-mode model yet;
  silicon behaviour of the M0+ and radio in EM2/EM3 is partly measured.

Evidence too weak or missing:

- BUFC channel ADDR at reset (B16): read as 0x20000000 on silicon, but the
  BG21 header has ADDR as an offset (reset 0) plus RAMBASEADDR (reset
  0x20000000), and the model's QEMU-only descriptor seam keys on ADDR != 0.
- The empty-payload CCM tag (C62): what the BA411E does is not known
  (AES-192, C63, is now rejected per BA411E_HW_CFG_1).
- The AP window after a pin-reset release (C19) and after SYSRESETREQ
  (`sysreset-blackout-ns`, TODO): duration and acknowledge unmeasured.
- TIMER SYNCBUSY scaling with HCLK (B91), PRS and LETIMER0 gating (B95),
  CLKEN1.IFADCDEBUG (A35), RFCRC.DATA (A19), FRC.CMD quirks (A25, A26),
  PROTIMER IF bits (A40).
- Per-channel receive on ch38/39 (7.80): being root-caused on silicon; hook
  in `efr32_rac_rx_handoff`.

Model defects found during the audit (both fixed in the model-gaps pass:
each SoC now resets alone; the default DEVINFO has INFO, PART, MEMINFO and
OpenOCD's family byte, and the debug-port e2e passes without `DEVINFO=`):

- In `efr32bg22-dual`, SYSRESETREQ, WDOG0, Lockup and pin resets reset the
  whole machine (`qemu_system_reset_request`), both SoCs; on the bench each
  chip resets alone.
- The default machine's DEVINFO.PART reads 0 (the kit's PART was never
  captured), so OpenOCD's flash driver stops at "Unknown MCU family 0"
  unless the e2e is given `DEVINFO=` (a dump, e.g. the suit's).

## Model-gaps pass (2026-11-01)

vendor/qemu branch `agent/model-gaps`, one commit per area:

1. **DEVINFO identity of the default kit** (C30): INFO 0x07090000, PART
   0x021608B0 (BG22 C224), MEMINFO 8 KiB pages, and byte 0x1FE = 0x80,
   which OpenOCD's efm32 driver reads as its family (128, Series 2) before
   PART.  Evidence: OpenOCD on the kit, "EFR32BG22 C224, rev 9", 8 KiB pages
   (hardware-bringup.md); the suit's DEVINFO dump fixes the encodings (PART
   0x021608AE, byte 0x1FE 0x80, "rev" = INFO.PRODREV prints 36).
2. **AES-192** (C63): rejected per BA411E_HW_CFG_1.g_Keysize (5, read on
   silicon) and Silicon Labs' driver (NOT_SUPPORTED); reported, not computed.
3. **Each SoC resets alone**: the reset request flags its SoC; the machines'
   reset hook resets only flagged SoCs (devices in system-bus order, three
   phases, then the CPUs); power-on and QMP `system_reset` still reset the
   machine.  The suit counts MCU resets through a per-SoC notifier.  qtest
   `rf/wdog-resets-one-soc`.
4. **CRYPTOACC TRNG** (RM SS11.5): registers, 64-word FIFO popped by reads,
   FIFOFULLOFF/FORCERUN, SOFTRESET; CLKEN1.CRYPTOACC gating; host entropy
   (reproducible under `-seed`); fill timing measured on silicon (HCLK sample clock,
   start-up and block times, radio-tables.md 7.89) [F]; the post-ROM reset state (RNGCTRL
   0x00040671, INITWAITVAL 0x200) is not modelled.  qtest `trng-fifo`.
5. **GPCRC** (RM ch. 14): the LSB-first engine, fixed CRC-32 and
   programmable reversed 16-bit POLY, byte/half-word/word input, BYTEMODE,
   BYTEREVERSE, BITREVERSE, DATA/DATAREV/DATABYTEREV, INIT/AUTOINIT,
   SET/CLR/TGL; CLKEN0.GPCRC gating.  qtest `gpcrc-crc-vectors` (CRC-32
   0xCBF43926, CRC-16/CCITT-FALSE 0x29B1, KERMIT 0x2189).
6. **Structural interconnect** (B71, B75-B79; `hw/arm/efr32-interconnect.c`):
   see the calibration below.
7. **IADC timing** (B115, C70): RM SS24.3 warm-up and conversion time, measured on silicon and fitted (radio-tables.md 7.89) [F].
   Sense-sampler timing changes: a conversion now completes 5 TIMEBASE
   intervals + ((4 OSR)+2) ADC_CLK cycles after the trigger (NORMAL warm-up
   mode), not at the command; anything depending on sense timing (the
   output-stage safety work) should be re-run.

The model-gaps2 pass (vendor/qemu `agent/model-gaps2`, section
"Model-gaps2 pass" below) closed B95, B96 and B107 and moved B81 and B97
to partial.

### Interconnect calibration: SS7.74 on the model

The unchanged silicon probes (`m0.S`, `m33.c`, `m33flash.S` of SS7.74) run
on the model from a flash image that sets the stage-3 clocks, loads and
releases the M0+ probe and drives the experiments the silicon scripts drove
(`mat_m0.py`, `mat_m33.py`, `flashrun.py`, `lat.py`), printing through
semihosting; harness and comparison script in the worktree's
`scratch/model-gaps/xq/` (`build.sh`, `run.sh`, `compare.py`).  M0+ rows:
cycles per op ((cycles - 609) / 1600).  M33 rows: the per-op change the
interferer causes (the model's M33 runs every instruction at its 1.84
average, so only the bus part is comparable).

| measured | interferer | silicon | before | model |
|---|---|---|---|---|
| M0+ RAC/FRC/SYNTH/MODEM ld or st | none | 4.25 | 5.00 | 4.25 |
| M0+ BUFC ld | none | 3.25 | 4.00 | 3.25 |
| M0+ SRAM ld / st | none | 3.25 | 2.00 | 3.25 |
| M0+ TIMER1 ld | none | 8.75 | 5.00 | 8.75 |
| M0+ CMU ld | none | 4.75 | 5.00 | 4.75 |
| M0+ SEQRAM ld / st | none | 2.00 / 2.50 | 2.00 / 2.00 | 2.00 / 2.00 |
| M0+ RAC ld | M33 RAC ld (SRAM code) | 9.19 | 5.00 | 9.00 |
| M0+ RAC ld | M33 RAC ld (flash) | 9.83 | 5.00 | 8.23 |
| M0+ BUFC ld | M33 BUFC ld | 4.77 | 4.00 | 4.76 |
| M0+ TIMER1 ld | M33 TIMER1 ld | 23.28 | 5.00 | 21.01 |
| M0+ CMU ld | M33 CMU ld | 9.10 | 5.00 | 9.17 |
| M0+ CMU ld | M33 TIMER1 ld | 19.32 | 5.00 | 17.67 |
| M0+ TIMER1 ld | M33 CMU ld | 13.26 | 5.00 | 13.27 |
| M0+ ALU | M33 SEQRAM ld (flash) | 3.46 | - | 3.35 |
| M0+ SEQRAM ld | M33 SEQRAM ld (flash) | 6.01 | - | 6.05 |
| M0+ SEQRAM st | M33 SEQRAM ld (flash) | 6.93 | - | 6.05 |
| M0+ RAC ld | M33 SEQRAM ld (flash) | 5.38 | - | 4.63 |
| M0+ BUFC / SRAM ld | M33 SEQRAM ld (flash) | 3.70 / 3.83 | - | 3.66 / 3.66 |
| M0+ TIMER1 / CMU ld | M33 SEQRAM ld (flash) | 10.56 / 6.12 | - | 8.99 / 5.08 |
| M0+ SRAM ld | M33 SRAM ld / st | 4.46 / 5.15 | 2.00 | 3.25 / 3.25 |
| M0+ IRQ entry, busy | none / M33 SEQRAM ld | 16 / 47 (27-54) | 16 / - | 16 / 48 |
| M0+ IRQ entry, WFI | none / M33 SEQRAM ld | 16 / 45 (23-51) | 16 / - | 16 / 38 |
| M33 RAC ld, change per op | M0+ RAC ld (SRAM / flash code) | +0.42 / +0.98 | 0 | +0.55 / +0.87 |
| M33 SEQRAM ld (flash) | M0+ executing | +0.50 | +0.50 | +0.50 |
| M33 SEQRAM ld (SRAM code) | M0+ SEQRAM st | +0.61 | +0.50 | 0 |
| M33 TIMER1 ld | M0+ TIMER1 ld | +4.22 | 0 | +1.16 |
| M33 CMU ld | M0+ CMU / TIMER1 ld | +1.09 / +3.76 | 0 | +0.25 / +1.85 |

"before" is the per-access constant table (the pre-pass model, by its
constants).  Fitted [I]: the handover counts (radio 6.0, BUFC 4.2, APB 6.2
M33 tenures), the SEQRAM steal (1.2 / 0.35 cycles), the tenure split of
each access.  Not reproduced: M33 SRAM traffic (B77: SRAM-to-SRAM and
SRAM-code rows, the +0.5 on M0+ peripheral loads in the mat_m0 session),
M0+ SEQRAM stores at 2.5, the M33 side of APB contention (low by 2-4x).
Firmware regressions: dual `b-adv`/`b-scan` 0 late/unsat/shutdown, SCAN_REQ
= SCAN_RSP; connection pair passes (25 s idle host, 60 s under load); the
model runs about 5-15 % slower per wall second (timeline arbitration and the
M0+ SRAM window as I/O).

### RM chapter coverage

Status of each reference-manual chapter in the model after this pass.
"RM" = behaviour from the RM text, "Si" = silicon-measured, [I] = inferred.

| RM chapter | block | modelled | evidence | gaps |
|---|---|---|---|---|
| 3 | System processor (M33, M0+ timing) | partial | Si SS7.75, SS7.93 | M33 counts cycles per class (B73); unmeasured classes placeholders, no instruction-cache misses; M0+ SEQRAM store 2.5 (alignment-dependent) |
| 4 | Memory and bus system | yes (gaps) | RM, Si SS7.74 | M33 SRAM/flash traffic (B77); LDMA as a bus manager (B81) |
| 5 | Radio transceiver | yes (radio-tables) | Si | see tables A and B |
| 6 | MSC, DEVINFO, USERDATA | yes | RM, Si | erase/write durations (C34); DEVINFO calibration words |
| 7 | DBG debug interface | yes | Si, docs/debug-port-model.md | AP3 / M0+ debug (A45, C3); DWT watchpoints (A43) |
| 8 | CMU | partial | RM, Si | status words beyond those measured |
| 9 | Oscillators | yes | Si SS7.73 | HFXO warm restart [P] |
| 10 | SMU | partial | Si | only the AHBRADIO attribution |
| 11 | CRYPTOACC | partial (gaps) | RM; Si for RADIOAES | TRNG test mode, health-test failures, IRQ; PK engine; BA413/418/419 |
| 12 | EMU (incl. DCDC) | partial | RM, Si | RSTCAUSE/RSTCTRL, DCDC REGULATION only: no BOD, voltage monitors, EM2/EM3/EM4 |
| 13 | PRS | yes (gaps) | RM, Si SS7.71/7.73 (probe routes, gating, timing) | producers CMU/CORE/AGC/BUFC/FRC/PROTIMER/SYNTH/PDM/LFRCO and some USART/EUART signals; RAC, CMU, MODEM, CORE, DTI, USART, WDOG consumers |
| 14 | GPCRC | yes | RM | - (no LDMAXBAR source; the LDMA feeds INPUTDATA by SWREQ) |
| 15 | RTCC | partial | RM | CCV0MATCH, PRECNT compare base, LOCK (capture and CCV outputs modelled) |
| 16 | BURTC | yes | Si SS7.73 | - |
| 17 | BURAM | yes | RM | gating [I] |
| 18 | LETIMER | yes (gaps) | RM, Si SS7.73 (gating) | DEBUGRUN, EM2/EM3 clocks; LFSYNC delay [I] |
| 19 | TIMER | yes | Si SS7.71/7.73 | DTI, INSEL = PIN, quadrature, up/down count |
| 20 | PDM | no | - | - |
| 21 | USART | partial | RM | I2S, parity errors, TX sequencer delays, AUTOCS, secondary sync mode |
| 22 | EUART | yes (gaps) | RM | parity errors, IrDA, auto-baud, CTS/RTS, PRS RX input |
| 23 | I2C | partial | RM | follower mode |
| 24 | IADC | yes (gaps) | RM [I] timing | bipolar output, SCALEx calibration, SUPPLY inputs, tailgating |
| 25 | GPIO | yes | RM | EM4 wake |
| 26 | LDMA | yes (gaps) | RM, Si SS7.74 (error, EN) | own throughput [I]; not an interconnect manager (B81); DBGHALT |
| 27 | WDOG | yes | RM, Si | EM2/EM3 run control |

## Model-gaps2 pass (2026-11-05)

vendor/qemu branch `agent/model-gaps2`, one commit per block.  The new
blocks are devices of their own (`hw/arm/efr32-*.c`) overlaid on the core
model's peripheral window, like the USART and GPIO; `efr32-soc-link.h`
gives them the CMU bus-clock gates, the clock-tree rates and the bus-cost
model.  qtests after the pass: `efr32bg22-test` 122, `efr32bg22-rf-test`
37, `efr32bg22-dap-test` 13, `efr32bg22-board-test` 7, `test-jlink-swd-io`
3.

1. **PRS** (RM ch. 13; B95, B96, B97; `efr32-prs.c`): 12 asynchronous
   channels (SOURCESEL/SIGSEL, AUXSEL, the FNSEL look-up table
   FNSEL[2A + B], SWLEVEL, SWPULSE), 4 synchronous channels, PEEK, every
   consumer register (SPRSSEL where the header has one), CLKEN0.PRS
   gating of reads and writes [Si SS7.71/7.73].  Propagation is immediate
   [Si SS7.73: SWPULSE and CMD.START within +-1 TIMER1 cycle].  Producers:
   TIMER0-4 UF/OF/CC (PRSCONF pulse/level), IADC0 done events, RTCC CCV0-2
   (CMOA), BURTC COMP, GPIO PIN0-7, LETIMER0 CH0/1, EUART0 TXC/RXFL, and
   the radio: RACL ACTIVE/RX/TX ([I] LNAEN, PAEN), MODEML.FRAMEDET,
   MODEMH.SYNCSENT/EOF, MODEM.FRAMESENT at the model's measured frame
   anchors (SS7.71/7.72).  Consumers: TIMER CC0-2 input logic, RTCC
   capture, IADC triggers, LETIMER0 START/STOP/CLEAR, EUART0 TRIGGER,
   LDMAXBAR DMAREQ0/1, LDMA SYNCTRIG, GPIO PRS routes.  The TIMER gained
   input capture (2-entry FIFO, ICOF, ICFOF/ICFUF/ICFWLFULL,
   ICFEMPTY/CCPOL), INSEL PRSASYNCPULSE / PRSASYNCLEVEL (the RM's 2-clock
   sampler, 7 with FILT: a producer pulse is never captured, as silicon
   showed for the MODEM events) / PRSSYNC, CTRL.RISEA/FALLA and the
   one-shot CFG.OSMEN; debugger reads of CCn_ICF do not pop [Si SS7.71]
   (the DAP marks its accesses with a requester id).  qtests
   `prs-logic-table`, `prs-bus-gate`, `prs-swpulse-timer-capture`,
   `prs-timer-input-qualification`, `prs-timer-reloadstart-oneshot`,
   `prs-rtcc-capture`, `rf/prs-radio-probe-routes` (the route of
   `scripts/bg22-tifs-probe.tcl` against a released-sequencer TX:
   SYNCSENT 40.65 us after TX entry, FRAMESENT the PDU+CRC airtime later).
2. **IADC triggers** (RM SS24.3.3.3; in the PRS commit): TRIGSEL
   IMMEDIATE/TIMER (the local timer, TIMEREN/TIMERDIS)/PRSCLKGRP/PRSPOS/
   PRSNEG (2 CLK_SRC_ADC of synchronisation [I]), ONCE/CONTINUOUS,
   SINGLESTOP/SCANSTOP, STATUS.xQEN/TIMERACTIVE.  qtest `iadc-triggers`.
3. **BURAM** (RM ch. 17; `efr32-buram.c`): 32 retention words, byte lanes,
   SET/CLR/TGL; kept through every reset the machine models; [I]
   CLKEN0.BURAM gating.  qtest `buram-retention`.
4. **LETIMER0** (RM ch. 18; `efr32-letimer.c`): the 24-bit counter on
   EM23GRPACLK / 2^CNTPRESC and the state machines of Figures 18.2-18.5
   (FREE, ONESHOT, BUFFERED with REP1/TOPBUFF, DOUBLE), UF/COMP/REP flags,
   TOGGLE/PULSE/PWM outputs with OPOL and CTO to the GPIO route and the
   PRS, PRS START/STOP/CLEAR with PRSMODE, LFSYNC writes landing at the
   third LF edge with SYNCBUSY ([I] 3 edges, as the BURTC measured,
   SS7.73), gated reads fault [Si SS7.73].  qtests
   `letimer-free-running-toggle`, `letimer-oneshot-pwm`.
5. **USART frame time fix**: `usart_frame_ns` divided by 2 x 256 x fPCLK,
   which `muldiv64` takes as 32 bits; above 8.4 MHz PCLK (16.8 MHz in
   synchronous mode) the divisor wrapped and frames were far too long:
   6.5x at the reset clock (FSRCO, PCLK 19.8 MHz), 7.9x at PCLK 38.4 MHz.
   qtest `usart-frame-time`.
6. **EUART0** (RM ch. 22; `efr32-eusart.c`): a chardev-backed UART like the
   USART ("<soc>.euart0"): 4-entry FIFOs with TXFIW/RXFIW, frame timing at
   f / (ovs (1 + CLKDIV/256)) on CMU_EUART0CLKCTRL's clock, TXDATA's
   per-frame controls, a chardev break as a framing error, RXBLOCK, start
   and signal frames, multi-processor addresses, loopback, TXDELAY, INTFLAG
   IF with the level flags re-set while true, CONFIG registers faulting
   when written enabled, LFSYNC CLKDIV/TRIGCTRL/CMD with SYNCBUSY [I 3
   cycles], PRS TRIGGER, DMA requests.  qtests `euart-loopback`,
   `euart-chardev`.
7. **LDMA and LDMAXBAR** (RM ch. 26; B81; `efr32-ldma.c`): 8 channels,
   XFER/SYNC/WRI descriptors, absolute and relative addressing, LINKLOAD,
   looping, SIZE/increments/signs, BYTESWAP, BLOCKSIZE, REQMODE, fixed and
   ARBSLOTS round-robin arbitration, SWREQ/REQDIS/REQCLEAR/REQPEND,
   CHDONE/IF/ERROR, SYNCTRIG from software, descriptors and PRS; a radio
   register source ends with IF.ERROR and clearing EN does not stop a
   channel [Si SS7.74].  Requests from PRS, EUART0, USART0/1 and the TIMERs.
   Timing [I]: 2 HCLK per unit, 4 per descriptor, 1 per arbitration; not
   an interconnect manager (B81's contention is not reproduced).  The
   USART and EUART0 take byte and half-word accesses (the LDMA writes
   TXDATA a byte at a time).  qtests `ldma-memory-copy`,
   `ldma-peripheral-request`.
8. **RADIOAES clock gate** (B107): CLKEN0.RADIOAES and RADIOCLKCTRL.EN
   (RM 8.5/8.5.25; set by sli_radioaes_acquire and by the open firmware's
   radioaes_init); [I] the fault rule.  qtest `radioaes-clock-gate`.
9. **IADC resolution and window compare** (RM SS24.3.7-24.3.8): output
   resolution 11 + log2(OSR x DIGAVG), RIGHT/LEFT 12/16/20 alignment
   (LEFT12 moved from bits 15:4 to 31:20 per Figure 24.17), DIGAVG
   back-to-back conversions, the CMPTHR window with SINGLECMP/SCANCMP.
   qtest `iadc-resolution-and-window`.
10. **USART timer** (RM SS21.3.10): the bit-time counter and three
    comparators with TSTART/TSTOP/RESTARTEN, as Zephyr's async driver uses
    them; a chardev break as a framing error (IF.FERR, RXDATAX.FERR); IrDA
    passes the chardev's bytes unchanged.  qtest `usart-timer-and-break`.

Regressions: `tests/debug-port/run-e2e.sh` over remote_bitbang passes its
14 checks; dual `scratch/sched/dual.sh b-adv b-scan 3 60` on the gaps2
binary: 0 late/unsat/shutdown lines, 62.3 virtual seconds in 60 s wall
(the pre-pass binary: 11.5), ADV 1872, SCAN_REQ 687, SCAN_RSP 683.  Both
differences come from the USART frame-time fix alone (item 5): the
pre-pass model with only that commit on top gives 706 / 702 and the same
speed-up (the firmware no longer spins ~8x too long on each VCOM byte),
while the pre-pass model gives 551 / 551 over 55 virtual seconds.  With
UART logging at its real speed 4 of ~700 scan requests go unanswered; and
past 64 virtual seconds (2^31 ticks of the 33.55 MHz scheduler clock) the
advertiser logs "deadline late start ... deadline -2139858319" and makes a
safe shutdown: apparently a signed 32-bit wrap in the firmware's deadline
arithmetic (the deadline prints negative once the clock passes 2^31),
reachable now that 60 s of wall time runs past 64 virtual seconds.  Both
are firmware findings, not model ones.

Behaviour changes a firmware can see:

- USART frames now take their programmed time (item 5): at the reset clock
  a byte is 6.5x shorter on the wire than before, at 38.4 MHz PCLK 7.9x.
- TIMER CCn_CTRL keeps bits 25:24 (ICEDGE) instead of masking them to the
  counter width; TIMER STATUS reports ICFEMPTYn for enabled capture
  channels; a one-shot timer (CFG.OSMEN) now stops at TOP on overflow.
- RADIOAES faults while CLKEN0.RADIOAES or RADIOCLKCTRL.EN is clear.
- PRS, LDMA, LETIMER0, EUART0 and BURAM registers are now the devices'
  (they used to be the catch-all register store, reading back what was
  written); their gated accesses fault.
- USART byte and half-word accesses act on the word (they were decode
  errors).
- IADC: LEFT12 alignment and SHOWID placement as above; with OSR > 2 or
  averaging a 12-bit result is the truncated top of the finer result (it
  was rounded at 12 bits); DIGAVG multiplies the conversion time.
- CMU_EUART0CLKCTRL reads its reset value 1.
- The radio's PRS TX pulses arm a timer only while a PRS channel selects
  MODEMH.SYNCSENT or MODEM.FRAMESENT.

Remaining gaps after the pass: PRS producers CMU CLKOUT, CORE CTIOUT, AGC,
BUFC, FRC, PROTIMER, SYNTH, PDM, LFRCO, USART CS/IRTX/RTS/TX, EUART0
IRDATX/RTS/TX, BURTC OVERFLOW; consumers CMU CALUP/CALDN, MODEM DIN, RAC
(CLR, CTIIN, FORCETX, RXDIS, RXEN, SEQ, TXEN), CORE CTIIN/M33RXEV, TIMER
DTI/DTIFS, USART CLK/IR/RX/TRIGGER, EUART0 RX, WDOG0 SRC; TIMER INSEL = PIN
and the quadrature decoder; LETIMER DEBUGRUN; EUART0 parity errors, IrDA,
auto-baud, flow control; LDMA as a bus manager and its own throughput on
silicon; USART I2S and parity errors; I2C follower mode; EMU BOD, voltage
monitors, DCDC beyond REGULATION; EM2/EM3/EM4.

## Tables

### A. docs/radio-tables.md sections 1 to 7.59

| ID | Finding | Evidence | Modelled? | Where (file:symbol, qtest) | Gap |
|---|---|---|---|---|---|
| A1 | Radio blocks FRC/AGC/RFCRC/MODEM/SYNTH/RAC have a 1-bit `EN.EN` at +0x004. BUFC/PROTIMER +0x004 latch a 1 | §1, public revB2 model, stock trace | yes | `efr32_*_masks` {0x004,0x1}; `efr32_radio_mask_write`. test_radio_enable_masks, test_stock_setup_aliases | none |
| A2 | Read-only IPVERSION: FRC/AGC/MODEM/SYNTH/RAC=1, RFCRC=0 | §4.1, public model | yes | `efr32_radio_ipversion` (~l.1321). test_radio_ipversions | none |
| A3 | SET/CLR/TGL aliases at +0x1000/+0x2000/+0x3000 on every radio block | §4.1, public model, stock trace | yes | `efr32_peripheral_write` alias switch (~l.5153). test_register_aliases, test_rac_sr3_clear_alias, test_rac_sr0_clear_alias | none |
| A4 | AHBRADIO attribution decides the window: the M33 uses the A/AA (secure) window before SMU makes AHBRADIO non-secure and B/BA after. The inactive window is RAZ/WI, with no fault (a PROTIMER write through the secure window from NS code "neither faulted nor erred", §7.33) | §4 (silicon), §7.33 (silicon) | yes | `efr32_peripheral_write` SMU_PPUSATD1 handling (~l.5177); `efr32_soc_realize` radio_secure/radio_ns aliases (~l.6776). test_radio_nonsecure_alias, test_stock_setup_aliases, test_protimer_secure_nonsecure_alias | none. §4's text "QEMU aliases BUFC unconditionally" is stale; the model is now exclusive |
| A5 | The private M0+ reaches the radio through A/AA only. Its first store to the B window HardFaults | §4 (silicon) | yes (implicit) | `efr32_soc_realize` seq_memory maps only RADIO_BASE 0xa8000000 (~l.6807). 0xb8xxxxxx is unassigned, so the access bus-faults | no qtest. Strong evidence; a 1-test regression would be cheap |
| A6 | RAC register map and write masks (RXENSRCEN 0x3FFF, CTRL 0x1F0107EF, FORCESTATE 0xF, IF/IEN 0x00FF000F, SEQIF/SEQIEN 0x3FFF000F, CMD 0xC000FDFF), plus all other writable RAC registers | §4.1, public revB2 | yes | `efr32_rac_masks` (~l.987). test_rac_masks, test_rac_stctrl_mask | none |
| A7 | RAC.STATUS.STATE bits 27:24 enumerate OFF=0 … SHUTDOWN=13. SEQACTIVE/DEMODENS/TXENS/RXENS are bits 28–31, SEQSLEEPING bit 22 | §4.1, public model, RAIL archive | yes | `efr32_rac_status` (~l.2242) | none |
| A8 | Measured status words: TX holds 0x09400000 with TXENS=0 and SEQACTIVE=0 once STATETX is acked (§7.21/7.28). OFF pre-TXEN reads 0x10400000, so SEQSLEEPING is set even in OFF (§7.30) | §7.21, §7.28, §7.30 silicon | yes | `efr32_rac_status`: TXENS = latched request, cleared on TX entry; SEQACTIVE = uncleared SEQIF state event; SEQSLEEPING = seq CPU halted in WFI. test_rac_tx_commands, test_seq_sleeping_follows_wfi | none (the model has since gained the §7.72 refinements) |
| A9 | Every state transition latches SEQIF.STATECHANGESEQ (bit 0) and the per-state SEQIF bit (16+n). RAC.IF bit 0 drives IRQ37. RAC_SEQ is IRQ38 | §4.1, public model, RAIL archive | yes | `efr32_rac_set_state` (~l.1489), `efr32_rac_update_irqs` (~l.1456). test_rac_tx_commands | none |
| A10 | Direct IRQ lines: AGC 31, BUFC 32, FRC 34, PROTIMER 36, RAC_RSM 37, RAC_SEQ 38, RDMAILBOX 39. Each line = IF & IEN | §4.1/§4.3, device header | yes | `efr32_{agc,bufc,frc,protimer}_update_irq` (~l.866–905), `efr32_rdmailbox_update_irqs`. test_frc_completion_interrupt_enable, test_protimer_interrupt_banks_are_aliasable, test_agc_rssi_valid_latches | none |
| A11 | RAC.CMD TXEN/TXDIS/RXDIS are distinct transitions. `RXENSRCEN.SWRXEN` via the SET alias enters RXSEARCH (2). RXDIS leaves it | §3.1, §4.1, RAIL archive, public model | yes | `efr32_rac_seq_command`, `efr32_rac_seq_evaluate`, legacy path in `efr32_peripheral_write` (~l.5300, ~l.5527). test_rac_software_rx_enable; rf-test test_released_rx_state_machine | none |
| A12 | Without sequencer firmware, CTRL.FORCEDISABLE written while OFF enters SHUTDOWN (13) and stays there indefinitely | §4.1 (silicon bring-up) | yes | `efr32_peripheral_write` FORCEDISABLE branches (~l.5459). test_rac_force_disable | none |
| A13 | Enable order: enabling RAC before BUFC and PROTIMER leaves RAC in SHUTDOWN, and enabling them afterwards does not repair it | §4.2 (silicon) | yes | `efr32_peripheral_write` RAC EN check (~l.5440). test_rac_enable_requires_dependency_order | none |
| A14 | TXWARM→TX releases as soon as SEQIF bit 24 (STATETXWARM) is cleared. The same IRQ8 handler then sees and clears bit 25 (STATETX) | §7.57 (silicon, AP3 single-step), §7.56 | yes | `efr32_rac_seq_evaluate` + `efr32_rac_seq_acknowledged` (1.09 µs, from later §7.72). test_seq_unacked_state_event_holds_txen | none |
| A15 | SYEN=0 / no SYNTHSTART: RAC holds TX indefinitely, FRC never consumes. SYNTH.STATUS reads 0x4 (SYNTHREADY=1, INLOCK=0) at the pre-TXEN and stalled points on stock and open alike | §7.42 (silicon), cause in §7.60 | yes | `efr32_rac_tx_complete` waits for synth_running; SYNTH.STATUS composed in `efr32_peripheral_read_word` (~l.4980). test_seq_tx_waits_for_synthesizer; rf-test test_synth_lock_and_pa_ramp_timing | none |
| A16 | SYNTH.FREQ adv-channel words 0x01f46aab / 0x01f96aab / 0x0204aaab (ch 37/38/39); stock leaves CHCTRL=CHSP=0 | §4.2 (silicon watchpoints) | yes | `efr32_rf_channel` (~l.2087); FREQ units f_HFXO/2^19. rf-test test_tx_offer_requires_matching_synth_channel | §4.2's statement that the model is "CHNO-only" is stale |
| A17 | FRC dynamic length: after TXEN, hardware rewrites WCNTCMP0 to length-field + MINLENGTH (Length 9 → 0xA, Length 12 → 0xD) | §7.20, §7.29, §7.38 (silicon, n=2) | yes | `efr32_frc_dynamic_length_wcntcmp0` (~l.2052) applied in `efr32_rac_capture_tx_snapshot`. test_frc_wcntcmp0_dynamic_length | only DFLMODE=1 with MINLENGTH=1 has been measured; fine as is |
| A18 | RFCRC.CMD.INITIALIZE loads DATA from INIT (DATA=0xAAAAAA immediately) | §7.43 (silicon) | yes | `efr32_peripheral_write` RFCRC.CMD (~l.5390). test_rfcrc_cmd_initialize | none |
| A19 | RFCRC.DATA is a live accumulator: it changed to 0x34caed after the TX attempt whether or not INITIALIZE had been issued | §7.43 (silicon, one sample) | no | DATA is a latch only | no CRC computation over the frame. Evidence is a single value, but the CRC function itself is public (BLE CRC-24); modelable once the per-byte feed point is known |
| A20 | FRC.STATUS is read-only, mask 0x01FFFFFF, fields as in revB2 | §7.4, public model | yes | `efr32_frc_status_write_ignored` (~l.1345). test_frc_status_is_read_only | none |
| A21 | Measured FRC.STATUS values: 0 pre-TXEN; stock 0x60 at IRQ8 and 0x20 after TX (ACTIVETXFCD sticky into RXPD); synth-off stall 0x01100200 (FRAMEOK, FSMSTATE=17), set within one instruction of TXEN | §7.4, §7.6, §7.30 (silicon) | no | FRC.STATUS always reads 0 in the model | ACTIVETXFCD during and after a running TX is well sampled and modelable. FSMSTATE/FRAMEOK encodings are single-scenario and weak |
| A22 | FRC.IF at the synth-off stall = 0x40020c04 (TXABORTED bit 2 plus bits 10/11/17/30); BUFC.IF=0x4 | §7.4, §7.40 (silicon) | no | FRC.IF is raised only with TXDONE/RXDONE (`efr32_frc_raise`) | the stall scenario is modelled (A15), but its FRC.IF bits are not. Bit meanings beyond TXABORTED are unknown; low priority |
| A23 | BUFC TX consumption: with FRC running, READOFFSET advances as bytes are written (WRITEOFFSET=READOFFSET=1 after the first WRITEDATA store). In the synth-off stall FRC consumed exactly the 2-byte header (READOFFSET=2, WRITEOFFSET=0xB) | §4.2 (DWT watchpoint), §7.31 (silicon) | no | `efr32_bufc_snapshot_data` (~l.1249) copies channel 0 without advancing READOFFSET | modelable: header consumed at TX entry, READOFFSET→WRITEOFFSET over the frame or at TX end. Evidence is solid for the stall and the endpoint, thinner for per-byte timing |
| A24 | Debugger-written BUFC READOFFSET holds (forcing 3 stays put for 20 ms) and does not drive the FRC FSM | §7.48 (silicon) | yes | READOFFSET is a masked latch (`efr32_bufc_mask_write`) | none |
| A25 | FRC.CMD.STATEINIT on a stalled TX advances READOFFSET by 2 per write (2→4→6→8) with STATUS unchanged. The second write left the M33 in HardFault | §7.49 (silicon, n=3) | no | FRC.CMD handles only RXABORT (~l.5372) | the +2 step looks like a header re-parse. Weak/unexplained; not worth modelling yet |
| A26 | Live-poking FRC.CMD.BLOCKINIT on a stalled TX caused an M33 HardFault/lockup | §7.31 (silicon, n=1) | no | ignored | unexplained, single observation; not modelable |
| A27 | BUFC layout: four buffers at +0x00C, stride 0x40; IF/IEN at +0x114/+0x118, mask 0x8F0F0F0F; sparse groups (overflow/underflow/threshold/corrupt); per-buffer STATUS.THRESHOLDFLAG bit 20; CTRL/offset/threshold/CMD masks | §4, public BG21 header plus xG22 trace | yes | `efr32_bufc_*` (~l.1041–1290), `efr32_bufc_mask_write`. test_bufc_interrupt_register_masks, test_bufc_channel_register_masks, test_bufc_sparse_threshold_flag | underflow and corrupt flags are not raised (no evidence) |
| A28 | BUFC data ports take the low octet of each 32-bit store; channel capacity is 2^(n+6) circular over backing RAM; overflow latches on the 65th byte of a 64-byte buffer | §4 (public header; mechanism from RAIL code) | yes | `efr32_bufc_write_data_port`, `efr32_bufc_capacity`. test_bufc_data_port, test_bufc_backing_memory_wrap, test_bufc_overflow_latch | none |
| A29 | FRC.TRAILRXDATA: R/W, mask 0x3F, reset 0 (fields RSSI, CRCOK, PROTIMER CC0 wrap-low/high, RTCSTAMP) | §4.1/§4.2, public revB1/revB2 | partial | mask {0x054,0x3f}; CRCOK appends one octet (`efr32_rac_rx_frame_end`) | RSSI and timestamp trailer fields are not modelled; that semantics comes from §7.66, which is outside this audit's range |
| A30 | AGC RSSI/FRAMERSSI are read-only, reset 0x8000, mask 0xFFC0 | §4.2, public model | yes | `efr32_peripheral_write` drops writes. test_agc_rssi_status_registers | live RSSI is not modelled (no RF input) |
| A31 | RAC.SR0–SR3 are generic R/W scratch, reset 0 (public). SR3's 0x08F08A31 on stock is firmware-written (§7.59 correction), not a hardware value | §7.2, §7.5.2, §7.59 (public model, silicon DWT) | yes | `efr32_rac_masks` 0x040–0x04c = 0xFFFFFFFF, reset 0 | none |
| A32 | RAC.STCTRL accepts and reads back 0x01061A7F (STCAL[23:0], STSKEW[24]); XORETIMECTRL / DIGCLKRETIMECTRL read back 0x770/0x430 | §7.10, §7.24 (silicon readback) | yes | masks {0x050,0x01ffffff}, {0x16c,0x777}, {0x174,0x777}. test_rac_stctrl_mask | the meaning of STCAL (sequencer timer calibration) is not modelled; no evidence for it |
| A33 | The M0+ NVIC is private to the core (the M33 debugger reads its own NVIC); priority bytes are byte-addressable; the M0+ has 2 implemented priority bits (inferred) | §7.10 (architecture; the 2-bit count is an inference) | yes | separate `seq_armv7m` cortex-m0 (~l.6835). test_seq_nvic_ipr_byte_isolation | the 2-bit count has not been confirmed on silicon (could be read back via AP3) |
| A34 | The M0+ is Armv6-M: no CFSR/HFSR (both read 0). A fault in its fault path enters architectural lockup, and the core stays locked until an external reset (§7.58: lockup loops continuously and OpenOCD "clears lockup") | §7.58 (silicon) | yes (audit) | `hw/intc/armv7m_nvic.c` ~l.604/~l.670: QEMU does not model Lockup and calls `cpu_abort()`, killing the whole emulator | **Closed:** Lockup modelled (`armv7m_nvic.c` nvic_lockup, `cpu.h` m_locked_up, DHCSR.S_LOCKUP); the M0+ stays locked until SEQRESET or a reset (SEQRESET now resets its NVIC too). qtest `seq-lockup-until-seqreset`, `m33-lockup` |
| A35 | Gated bus clocks: each CLKEN1 bit 0–10 gates one radio window; RADIOCLKCTRL.EN gates all of them plus BUFC; CLKEN0.SYSCFG gates SYSCFG (the debugger read faults under stage 2.5). A gated read bus-faults and a gated write is dropped. The stock M0+ blob bus-faults then locks up when RDMAILBOX0/1, RDSCRATCHPAD, PRORTC, IFADCDEBUG or SYSCFG clocks are off | §7.59 (silicon, 28–36 lockups/6 s → 0 when the clocks are set) | partial (audit) | `efr32_bus_error` (~l.5613), `efr32_radio_bus_clock`. test_gated_bus_clock_faults_and_drops, test_radio_enable_requires_bus_clocks; rf-test test_radio_block_clock_gates | **Lockup consequence closed** (see A34): a stock M0+ fault in its fault path now locks the M0+ instead of aborting QEMU. Still open: CLKEN1.IFADCDEBUG gates no window (which window it gates is unmeasured) |
| A36 | RF mailbox: data at +0x004, FLAGS/IF at +0x040, IEN at +0x044. The M33 posts to endpoint C and waits for bit 1 or bit 0 to clear; the M0+ services it and clears it. FLAGS is a real latch, not always-ready (§7.5.4 lockstep: the stock boot spins on 0xB802C040 bit 0 until the M0+ acks) | §1.1, §7.5.4, §7.53 (silicon) | yes | `efr32_rdmailbox_update_irqs` (~l.1473), FLAGS stored latch; M0+ IRQ23 / M33 IRQ39. test_rfmailbox_flags_latch | §7.5.4's "QEMU returns 0 always" is stale. Delivery latency is unmeasured |
| A37 | SEQRESET/CPUWAITDIS hold/release the M0+, which starts at its reset vector (0x2a2c in stock) from the SEQRAM vector table. An M33 SYSRESETREQ cascades into an M0+ reset/hold | §7.53, §7.57 (silicon, AP3) | yes | `efr32_peripheral_write` RAC.CTRL SEQRESET/CPUWAITDIS (~l.5487); `efr32_seq_cpu_reset`. test_radio_sequencer_executes_open_code, test_radio_sequencer_release_is_idempotent | none |
| A38 | PROTIMER: NS (0xB801C000) and S windows are one block; stock init CTRL=0x00011102, STATE +0x30, PERIOD +0x38, CMD=1 start; compare slots at +0x100+0x10·i | §4.3 (RAIL archive + stock trace) | partial | `efr32_peripheral_canonical_address`, named latches. test_protimer_trace_latches, test_protimer_compare_slot_clear_alias | command, compare and capture semantics are not modelled (no hardware evidence) |
| A39 | PROTIMER counter (+0x020) stays 0 and frozen with EN=1 until CTRL plus the start command are written; once started it counts (0xb980 → 0x1031a2 over ~450 ms, i.e. roughly 2 MHz; RAIL documents a ~0.5 µs tick) | §7.32, §7.33, §7.39 (silicon) | yes (audit) | `efr32_periph_reset_hold` (~l.6095) runs the 2 MHz ptimer from reset regardless of EN/CTRL/CMD. test_protimer_counter_is_independent_from_radio_setup asserts that it counts with no start command | **Closed:** counter stopped at 0 from reset, started by PROTIMER.CMD bit 0 ([I] which write of stock's sequence is the start; no stop modelled). qtest `protimer-counter` |
| A40 | PROTIMER IF/IEN: with IEN=0xFFFFFFFF and the timer started, a TXEN leaves IF=0x3 and pends NVIC IRQ36. A CLR of IF clears bit 1 (one-shot); bit 0 reasserts at once (level). Serviced IRQ36 re-enters continuously | §7.34, §7.40 (silicon) | partial | the IF/IEN bank and IRQ36 intersection are modelled; no source ever sets a flag (`efr32_protimer_update_irq`, ~l.896) | the two bits are unnamed, so modelling them is weak. At most, model bit 0 as a level while the counter runs |
| A41 | Debug halt of the M33 freezes the radio (FRC.STATUS constant while halted) | §7.6 (silicon, n=20 reads) | no | `efr32_debug_frozen` (~l.2693) only gates BURTC/TIMER/RTCC/WDOG | weak: §7.30 shows the transition finishing within one single-step, and the observed value was itself a stalled state. Not modelable without a better probe |
| A42 | The secure radio alias read through the debugger returns 0 ("secure debug disabled"); halting secure code is refused; the DCI status reports secure debug disabled | §7.8, §7.47 (silicon) | yes (audit) | `hw/arm/efr32bg22-debug.c` l.53 sets `secure-debug-enabled` true, while the DCI GET_STATUS in `hw/misc/efr32-dap.c` (~l.217) reports it disabled. Debugger reads of 0xa8… still RAZ, but only because of A4's inactive window | **Reclassified, no change:** not a contradiction. docs/debug-port-model.md (debug blocks): the DCI "Secure debug" is the Secure Engine's certificate-based unlock, not TrustZone invasive debug; DHCSR.S_SDE read 1 on the suit (DHCSR 0x03130003). The 0xa8 RAZ is the inactive AHBRADIO window (A4) |
| A43 | DWT data watchpoints work on silicon (stock WRITEDATA loop, §4.2; SR3 writes caught, §7.59). §7.11 found them unreliable on RAC.CMD | §4.2, §7.59 (silicon) | no | `hw/misc/armv8m-debug.c` ~l.728: DWT_FUNCTION is stored only, watchpoint matching unmodelled | modelable (architectural, QEMU has watchpoint support); medium value |
| A44 | FPB hardware breakpoints survive SYSRESETREQ and fire on the next match (`bg22_reset_to_breakpoint`) | §7.27 (silicon) | yes | `dbg_op_reset` (`hw/misc/armv8m-debug.c` ~l.346). dap-test test_halt_registers_and_reset | none |
| A45 | M0+ debug access via AP3 (OpenOCD `efm32.seq`): halt, hardware breakpoints, single-step; "external reset detected" when the M33 re-issues SEQRESET | §7.53, §7.57 (silicon) | no | `hw/misc/efr32-dap.c` exposes AP0 (M33) and AP1 (DCI) only | modelable from the ADIv5 / Armv6-M debug specs; needed to reproduce the AP3 workflows in simulation |
| A46 | Connecting a fresh OpenOCD session to a running chip during early boot can itself hang stock (mailbox wait at 0x13056) | §7.53/§7.54 (silicon) | no | — | the mechanism is unknown (the halt-on-connect window races the M33/M0+ handshake). Not modelable until it is characterized |
| A47 | Radio sleep/energy: datasheet lists the BLE radio chain (RF, synth, modem, FRC, BUFC RAM, M0+) as EM1-minimum and RFSENSE as EM2–EM4-capable; EM2/EM3 must not retain a usable radio | §3.1, §5 (datasheet) | no | no EM2/EM3 gating of radio blocks in `efr32bg22.c` | modelable at block level (radio inaccessible or reset in EM2+), but the EMU/CMU transition sequence is unknown; low priority |
| A48 | Power-on reset vs SYSRESETREQ: the radio state (SR3 etc.) is the same after a real power cycle; SEQRAM is retained across an M33 reset | §7.5.2, §7.5.4 (silicon) | partial | SEQRAM (`radio_ram`) is plain RAM, so it survives system reset; no POR/warm distinction for radio registers | low value; all registers measured reset to their documented values |
| A49 | Stock TX completes a whole frame and leaves TX well inside 10 ms. The stall settles within one instruction of TXEN (no gradual approach). FRC parses the header before stalling | §7.6, §7.20, §7.21, §7.30 (silicon, coarse) | yes (superseded) | frame timing now comes from the §7.71/7.72 calibrated anchors (`EFR32_RAC_TX_ENTRY_TO_SYNCSENT_NS` etc.) | the §7.6 numbers are halt-latency bounded and must not be used as timing; superseded |

### B. docs/radio-tables.md sections 7.60 to 7.81, docs/crypto.md

Line numbers in the Where column are approximate (the file moved during the audit).

| ID | Finding | Evidence | Modelled? | Where (file:symbol, qtest) | Gap |
|---|---|---|---|---|---|
| B1 | With RAC.SYEN=0 and no SYNTH.CMD.SYNTHSTART, the RAC sits in TX forever and FRC never sets ACTIVETXFCD (BUFC read stalls at 2) | §7.60 silicon | yes | bg22.c:efr32_rac_tx_complete (~1693, tx_waiting_for_synth); T: seq-tx-waits-for-synthesizer | - |
| B2 | SYNTHSTART alone, or SYEN+SYNTHENCTRL alone, does nothing; SYEN (0x1FBF/0x7FBF) then SYNTHSTART drains the parked frame immediately, RAC -> TXPD | §7.60 silicon | yes | bg22.c peripheral_write SYNTH.CMD (~5266), EFR32_RAC_SYEN_VCO_POWER (bits 7-9) | Which SYEN bits are required is [I] (VCO bias/PFET/regulator assumed) |
| B3 | [I] TX symbol timing derives from the running LO/VCO (why FRC stalls without synth) | §7.60 [I] | yes (as rule) | same as B1 | - |
| B4 | In those pokes INLOCK stays 0 when the VCO was never calibrated, yet the frame drains | §7.60 silicon | partial | bg22.c:peripheral_read_word SYNTH.STATUS (~4982) | Model reports INLOCK 8.9 us after any SYNTHSTART with VCO supplies, calibrated or not |
| B5 | SYNTH.STATUS reads 0x4 (SYNTHREADY only) on a radio whose SYNTH.CMD was never written (stage 3 pre-§7.61 TX; §7.80 RXSEARCH from OFF) | §7.60, §7.80 silicon | yes (audit) | bg22.c reset (~6109) puts STATUS=0; SYNTHREADY only set on a SYNTH.CMD write (~5286) | **Closed:** SYNTH.STATUS = SYNTHREADY from reset. qtest `seq-tx-waits-for-synthesizer` checks STATUS == 0x4 in the stalled TX |
| B6 | Stock TX: SYEN=0x1FBF, SYNTHENCTRL=0x100200, SYNTH.STATUS 0x4010005 / 0x4000005 in TX (INLOCK, VCOFREQACTIVE bit 16 sometimes) | §7.60/7.61 silicon | partial | SYNTH.STATUS computed (~4982) | VCOFREQACTIVE (bit 16) and bit 26 not produced; only INLOCK/IFFREQEN/SYNTHREADY |
| B7 | LOCNTTARGET is read-only, hardware-derived from the programmed channel (0xFA3 on the measured channel; 0 when never enabled); in the calibration state the programmed LO's count for the last RUN's NUMCYCLE, NUMCYCLE-0 value after CAPCALSTART / CLEAR | §7.61, §7.90 silicon + public model | yes (calibration state) | bg22.c:efr32_synth_locnt_target, efr32_synth_locnt_run; RF: synth-calibration-open-loop | Outside calibration silicon read the NUMCYCLE-5 value; the model holds the last value |
| B8 | LOCNTSTATUS.LOCOUNT reflects the VCO frequency; SAR VCO calibration converges to VCOTUNING 0x8338/0x8335; per-channel clusters 0x8334, 0x82CC, 0x81E6 (ch37/38/39); closed loop it counts only with FCALRUNCLKEN; NUMCYCLE saturates at 12; 19-bit wrap; LO step response, slew and VCO limits | §7.61, §7.62, §7.80, §7.90 silicon | yes | bg22.c:efr32_vco_open_hz, efr32_synth_lo_closed_hz, efr32_synth_locnt_*; RF: synth-locnt-counts-lo, synth-calibration-open-loop, synth-freq-step-settles, synth-large-step-slews, synth-lo-stops-at-vco-limit | Model codes within 2-17 of silicon; uncalibrated SYNTHSTART taken as locked [I] |
| B9 | LOCNTSTATUS.BUSY lasts 2^(NUMCYCLE+1) HFXO periods after LOCNTCTRL.RUN (70/40/20/20 M0+ cycles at NUMCYCLE 5/4/3/2, n=200) | §7.80 silicon (rule [I]) | yes | bg22.c peripheral_write LOCNTCTRL (~5177), read (~4973); RF: locnt-busy-window | - |
| B10 | State events map to M0+ IRQ n = SEQIF bit n+16 (IRQ8 TXWARM, IRQ9 TX, IRQ10 TXPD, IRQ11 TX2RX, IRQ2 RXSEARCH, IRQ4 RXPD, IRQ7 RX2TX, IRQ1 RXWARM) | §7.62/7.63 silicon | yes | bg22.c:efr32_rac_update_irqs (~1456) | - |
| B11 | Released-sequencer TX ends in TXPD and reaches OFF only on STATETXPD ack | §7.61/7.62 silicon | yes | bg22.c:efr32_rac_tx_complete, efr32_rac_seq_evaluate (~1597); T: seq-tx-waits-for-synthesizer | - |
| B12 | MODEM TXRAMPDONE latches after a PAENCTRL.PARAMP change: set 0.44 us (one poll), clear 0.81-3.96 us (median 3.96); survives the handler's clear-after-PARAMP | §7.62/7.69/7.72 silicon | partial | bg22.c EFR32_PA_RAMP_UP_NS 440 / DOWN_NS 3960, efr32_pa_ramp_complete (~3624); T: pa-ramp-survives-clear-after-paramp; RF: synth-lock-and-pa-ramp-timing | Fixed 3.96 us (top of range) not the 0.81-3.96 distribution; latch is deferred to first MODEM.IF/SEQIF read ([I] mechanism) |
| B13 | TX with SWRXEN asserted: RAC.STATUS 0x89400002, RAC goes TX -> TX2RX -> RXSEARCH by itself | §7.63 silicon | yes | efr32_rac_tx_complete; RF: released-tx-turns-around-to-rx | - |
| B14 | RXSEARCH SYNTH.STATUS=0x6 (IFFREQEN+SYNTHREADY), never INLOCK in receive | §7.64/7.72 silicon | yes | SYNTH.STATUS read (~4982, synth_if_enabled) | - |
| B15 | Demod correlates against MODEM.VTCORRCFG0 = ror(bitrev(AA),1); the complemented value (PHY table) receives nothing | §7.64 silicon | no | - (medium delivers regardless of access address) | No access-address match in the receiver; an inverted VTCORRCFG0 still receives in QEMU |
| B16 | BUFC RX channel at reset has CTRL=0, ADDR=0x20000000; FRC then writes the received frame over the start of SRAM | §7.64 silicon | no | bg22.c:efr32_bufc_write_data_port (~1118) | Model BUFC ADDR resets to 0, so no guest-RAM write; the silicon corruption is not reproduced |
| B17 | RXFRAME with TXEN latched and SWRXEN withdrawn -> RX2TX (STATUS 0x63400000); with SWRXEN still set the RAC returns to RX (TXEN dropped) | §7.65 silicon | yes | bg22.c:efr32_rac_rx_frame_end (~1731); RF: released-rx-frame-turns-around-to-tx, released-rx-frame-end-alternatives | RX2RX as the route is [I] |
| B18 | RX2TX is held until acknowledged; TX follows with no TXWARM | §7.66/7.67/7.70 silicon | yes | efr32_rac_seq_evaluate RX2TX case | - |
| B19 | RX FIFO layout: header, length, length payload octets, then TRAILRXDATA-selected trailer; TRAILRXDATA is configuration (RW, reset 0) | §7.66 silicon/public | yes | efr32_rac_rx_frame_end | - |
| B20 | TRAILRXDATA.CRCOK appends one octet: bit 7 = CRC result ([I]), low bits vary with signal; corrupted RFCRC.INIT gives bit7=0 | §7.66 silicon + [I] | partial | efr32_rac_rx_frame_end writes 0x80 always (EFR32_FRC_TRAILER_CRCOK) | Always CRC-good, low bits 0; no CRC-INIT/whitening mismatch check between peers |
| B21 | Without FRC.RXCTRL.ACCEPTCRCERRORS a CRC-failed frame is dropped silently (no RXDONE) | §7.66 silicon | no | - | Model has no CRC failures, so the drop path does not exist |
| B22 | RAC.STIMER counts ~4.8 ticks/us (682 SysTick cycles = 85 ticks), 16-bit, restarts on every RAC state change | §7.66/7.68/7.75 silicon | yes | bg22.c:efr32_rac_stimer (~2278), EFR32_RAC_STIMER_TICKS_PER_10US 48; RF test (STIMER_TICKS_IN_INTERVAL) | Rate is fixed; RAC.PRESC.STIMER dependence not modelled |
| B23 | SEQIF clear -> RAC.STATUS change 1.09-1.22 us (TX2RX, RXPD, TXPD, RX2TX) | §7.72 silicon | yes | EFR32_RAC_ACK_TO_TRANSITION_NS 1090, efr32_rac_seq_acknowledged (~1664) | Ack of any RAC IF/IEN/SEQIF/SEQIEN write arms the timer ([I] for writes that are not a clear) |
| B24 | Each state (except OFF) is left only after its own SEQIF bit is cleared, whatever SEQIEN; pending events of other states do not hold | §7.72 silicon (refines §7.69) | yes | efr32_rac_own_event_pending, efr32_rac_seq_evaluate; T: seq-unacked-state-event-holds-txen; RF: released-rx-state-machine | Frame-driven exits obeying the hold are [I] |
| B25 | OFF leaves at once on TXEN (-> TXWARM) or SWRXEN (-> RXWARM) even with STATEOFF pending | §7.72 silicon | yes | efr32_rac_seq_evaluate OFF case | - |
| B26 | RXWARM -> RXSEARCH on clear even if SWRXEN withdrawn; RXDIS in RXWARM does nothing | §7.72 silicon | yes | evaluate RXWARM case; efr32_rac_seq_command (~1829) | - |
| B27 | RXSEARCH -> RXPD when SWRXEN withdrawn; RXDIS -> RXPD even with SWRXEN set; RXPD clear -> OFF -> RXWARM if SWRXEN still set | §7.72 silicon | yes | evaluate RXSEARCH/RXPD/OFF; rx_disable_requested | - |
| B28 | TXDIS in RXSEARCH, TXDIS/RXDIS in RXPD do nothing; SWRXEN set in RXPD gives RXWARM right after OFF | §7.72 silicon | yes | efr32_rac_seq_command | - |
| B29 | TXDIS in TXWARM clears TXENS; the clear then goes TXPD | §7.72 silicon | yes | evaluate TXWARM case | - |
| B30 | TXDIS in TX is immediate even with STATETX pending: TXPD, or TX2RX with SWRXEN; RXDIS in TX does nothing | §7.72 silicon | yes | efr32_rac_seq_command; RF: released-tx-disables | [I] no TXDONE for an aborted TX |
| B31 | TXEN in TX sets TXENS (0x49400000); effect at frame end not observed | §7.72 silicon / [I] | partial | efr32_rac_tx_complete drops it ([I]) | Unmeasured on silicon |
| B32 | FORCEDISABLE from RXSEARCH/TX -> SHUTDOWN at once, clears TXENS; SHUTDOWN clear -> OFF; OFF holds until FORCEDISABLE cleared, then SWRXEN -> RXWARM | §7.72 silicon | yes | efr32_rac_seq_force_disable (~1855), CTRL write (~5466); T: rac-force-disable | - |
| B33 | FORCEDISABLE ends RXFRAME ("RXFRAME to OFF at once", 5/5); RXDIS does not end RXFRAME (a 297 us wait observed) | §7.80 silicon | yes (audit) | force_disable (via SHUTDOWN + ack); RXDIS only acts in RXSEARCH | **Closed:** comment now cites the silicon result; FORCEDISABLE also drops the frame in reception and its timer. qtest `rf/released-rx-frame-disables` |
| B34 | RX2TX after the M0+'s TXDIS goes TXPD (TXPD entries = responses + cancels); STATUS 0x17000000 | §7.72 silicon | yes | evaluate RX2TX case | - |
| B35 | RAC.STATUS: SEQACTIVE(28)=any SEQIF state event regardless of SEQIEN; TXENS(30)=latched TX request; RXENS(31)+RXMASK bit1=SWRXEN; DEMODENS(29) only in RXSEARCH/RXFRAME; SEQSLEEPING(22)=M0+ in WFI; table of 11 samples (0x00400000...0x9d400002) | §7.72 silicon | yes | bg22.c:efr32_rac_status (~2242); T: seq-sleeping-follows-wfi (known flaky); RF state tests | RX2RX STATUS [I] (never sampled) |
| B36 | RAC.STATUS before first TXEN 0x00400000 vs 0x10400000 depending on build (bit 28) | §7.71 silicon (explained by §7.72 SEQACTIVE rule) | yes | efr32_rac_status | - |
| B37 | Open image needs no PROTIMER enable/clock/counter; "PROTIMER before RAC" ordering does not hold for the open image | §7.71 silicon | yes (audit) | bg22.c peripheral_write RAC.EN (~5410) still forces SHUTDOWN when RAC.EN is set before BUFC/PROTIMER EN; T: rac-enable-requires-dependency-order | **Closed:** PROTIMER dropped from the RAC.EN ordering rule (BUFC stays, [I]). qtest `rac-enable-requires-dependency-order` (no-PROTIMER case) |
| B38 | Live stage 3 PROTIMER: EN=1, CTRL=0, COUNTER static 0 | §7.71/7.73 silicon | yes (audit) | bg22.c:efr32_protimer_counter (~2316), 2 MHz ptimer free-runs | **Closed:** as A39 |
| B39 | PROTIMER compare/wrap flags (stock stack programs CTRL, PRECNT, START, CC0/1, IEN 0x03001804) | §7.78 [I], undocumented | no | - | Blocks stock BT stack boot in the model; no public register semantics |
| B40 | Bus-clock gate map: CLKEN1 bit0 AGC,1 MODEM,2 RFCRC,3 FRC,4 PROTIMER,5 RAC,6 SYNTH,7 RDSCRATCHPAD,8 RDMAILBOX0,9 RDMAILBOX1,10 PRORTC; bit 11 BUFC gates nothing; bits 12-15 nothing | §7.72 silicon | yes | bg22.c:efr32_radio_bus_clock (~1367), efr32_bus_error (~5613); RF: radio-block-clock-gates; T: gated-bus-clock-faults-and-drops | - |
| B41 | RADIOCLKCTRL.EN=0 faults every radio window, BUFC and SEQRAM (0xB0000000); debugger SEQRAM reads fail | §7.72, §7.74 silicon | yes (audit) | efr32_bus_error covers radio+BUFC; efr32_seqram_m33_read (~6527) ungated | **Closed:** the M33/debugger SEQRAM window faults (read) and drops writes while RADIOCLKCTRL.EN is clear (`efr32_seqram_gated`). qtest `rf/seqram-radio-clock-gate`. The secure 0xa0000000 alias is plain RAM (unmeasured) |
| B42 | Gated radio write: accepted by AP and dropped (RAC SR0 kept pre-gate value) | §7.72 silicon; §7.73 notes TIMER/PRS gated writes give sticky bus errors and radio not re-measured | partial | efr32_radio_mmio_write (~5952) returns OK, drops | Open question: may be a (sticky) bus error like TIMER; needs remeasure |
| B43 | Windows 0xB8008000 and 0xB8030000..>=0xB80FC000 fault always | §7.72 silicon | yes | EFR32_RADIO_HOLE_A/B (~5608) | - |
| B44 | CLKEN0.SYSCFG cleared faults SYSCFG at secure and NS addresses | §7.72 silicon | yes | efr32_bus_error SYSCFG branch | - |
| B45 | Secure 0xA8 radio aliases read 0 (no fault) while AHBRADIO is attributed non-secure | §7.72 silicon (AP) | partial | bg22.c SMU PPUSATD1 write (~5165) disables m33_radio_secure region | Disabled region likely faults instead of reading 0; unverified |
| B46 | Gated CPU access is a BusFault (AHB error seen by AP; CPU load not stepped for radio) | §7.72 [I]; TIMER0 CPU precise BusFault CFSR 0x8200 BFAR 0x50048064 [F §7.73] | yes | MEMTX_ERROR paths (~5881, 5942) | - |
| B47 | M0+ loads from CLKEN1-gated RAC/FRC HardFault (~22 cycles each); gating BUFC (bit 11) no effect | §7.74 silicon | yes | same bus_error applies to the M0+ | - |
| B48 | RACL.ACTIVE rise (TXEN) -> RACL.TX rise = 91.0/91.03 us, all samples equal (TXWARM incl. handler) | §7.72/7.75 silicon | partial | emergent: sequencer code + bus costs + LO-count; model 91.18-94.33, mean 92.65 (§7.93; 90.96-92.52 in §7.80) | Model mean +1.6 us, spread 3.2 us vs silicon zero spread |
| B49 | RACL.TX rise -> SYNCSENT 40.6-40.7 us | §7.72 silicon | yes | EFR32_RAC_TX_ENTRY_TO_SYNCSENT_NS 40650 | - |
| B50 | FRAMESENT -> RACL.TX fall 0.8-1.0 us | §7.72 silicon | yes | EFR32_RAC_FRAMESENT_TO_TX_EXIT_NS 900 | - |
| B51 | TX exit -> RX rise (TX2RX incl. handler) 123.4-123.7 us (123.53) | §7.72/7.75 silicon | partial | emergent; model 124.89-127.00, mean 125.81 (§7.93; 124.50-125.51 in §7.80) | Model +1.4 to +3.5 us long |
| B52 | MODEMH.EOF -> RACL.RX fall (RXFRAME exit) 3.2-3.4 us | §7.72 silicon | yes | EFR32_RAC_EOF_TO_RX_EXIT_NS 3300 | - |
| B53 | Peer FRAMESENT -> this receiver's EOF = d_tx + d_rx = 9.2-9.7 us (two peers) | §7.71 silicon | yes | EFR32_RF_FRAMESENT_TO_PEER_EOF_NS 9450 | d_tx/d_rx split unmeasured |
| B54 | MODEML.FRAMEDET -> EOF 131.4-133.2 us (frames assumed SCAN_REQs): FRAMEDET ~3.5 us after AA end | §7.72 silicon + [I] | yes ([I]) | EFR32_RAC_SYNC_TO_FRAMEDET_NS 3500 | Frame length assumption |
| B55 | FRAMESENT-SYNCSENT = (PDU+CRC)*8 us exactly; SYNCSENT = end of AA (40 us after packet start) [I] | §7.71 silicon/[I] | yes | efr32_rf_le1m_airtime_ns (~287) | Only LE 1M |
| B56 | TXEN -> ADV_IND SYNCSENT 131.7 us (131.65-131.76) | §7.71 silicon | yes (emergent) | TXWARM duration + 40.65 | Follows B48 spread |
| B57 | EOF -> SCAN_RSP SYNCSENT 180.1-180.9 us (stage 3 135.6 us hold); stock 180.2-181.1 | §7.71 silicon | partial | emergent; model 180.6 (§7.72), 181.55 (181.52-181.68) under core-clocks (§7.75) | Model +0.65..+1.45 us late under core-clocks; suspect EFR32_CORE_CLOCK_QUANTUM_NS 500 / M0+ poll granularity |
| B58 | SYNTHSTART -> INLOCK 7.6-9.95 us, median 8.9 (TX, n=216) | §7.72 silicon | partial | EFR32_SYNTH_LOCK_NS 8900 | Fixed median, no spread |
| B59 | RX ready: SWRXEN (RAC OFF) -> RACL.RX rise 166.55-166.76 us (n=899) | §7.80 silicon | yes (emergent) | RXWARM handler + LO-count; model 166.01-167.63 | Model spread ~1.6 us vs 0.2 us |
| B60 | Receiver with synth/LNA/IF-ADC unpowered (SYEN 0, RFPATHEN 0, IFADCDBG 0x0C, SYNTH.STATUS 0x4) detects nothing (0 RXFRAME in 90 windows) | §7.80 silicon | yes | bg22.c:efr32_rx_chain_powered (~1904); RF: released-rx-chain-off-detects-nothing | Which enable is individually required is [I] |
| B61 | SYNTH.FREQ = round(f*2^19/38.4 MHz) (0x01F46AAB/0x01F96AAB/0x0204AAAB = 2402/2426/2480); CHCTRL*CHSP adds [I] | §7.80 silicon / [I] | yes | bg22.c:efr32_rf_channel (~2087); RF: tx-offer-requires-matching-synth-channel | - |
| B62 | Receive is channel-dependent on silicon: CRC ok ch37 77 %, ch38 1 %, ch39 0 % (cause open) | §7.80 silicon | no | - | RF/analog, not modelable without a cause; out of scope |
| B63 | RX FIFO overflow: 512-octet FIFO full -> BUFC.IF 0x100, RAC in RXOVERFLOW (6) with no SEQIF event (IRQ6 never runs); RXDIS/SWRXEN off/BUFC CLEAR/FRC RXABORT/IF clear do not leave; FORCEDISABLE -> SHUTDOWN -> OFF | §7.80 silicon | yes | efr32_rac_rx_frame_end overflow branch (~1784), efr32_rac_set_state (no RXOVERFLOW event); RF: released-rx-fifo-overflow | Partial write of the overflowing frame is [I] |
| B64 | RXDIS in RXFRAME/RX2RX/RX2TX/TX2RX ignored | §7.72 [I]; RXFRAME confirmed §7.80 | yes (audit) | efr32_rac_seq_command | **RXFRAME case pinned:** qtest `rf/released-rx-frame-disables`. RX2RX/RX2TX/TX2RX stay [I] |
| B65 | M0+ clock = SYSCLK/(RHCLKPRESC+1): 38.4 (stage 3), 19.2 (HFXO), 9.89 (FSRCO); independent of HCLKPRESC and EM01GRPA | §7.74/7.75 silicon | yes | bg22.c:efr32_cpu_clocks_update (~5770); T: core-clocks-per-cpu-rates | - |
| B66 | M33 HCLK = 76.8 MHz = TIMER1 1:1 in stage 3 | §7.75 silicon | yes | efr32_cpu_clocks_update, efr32_hfrcodpll_hz | - |
| B67 | RADIOCLKCTRL.EN=0 freezes the M0+ with state kept | §7.74 silicon | yes | efr32_cpu_clocks_update (0 Hz); T: core-clocks-radio-clock-stops-seq | - |
| B68 | SYSCLKCTRL switch: no stall beyond ~0.1 us | §7.74 silicon | yes | immediate | - |
| B69 | M33 entering EM2 stops the M0+ (82 cycles in 0.5 s) | §7.74 silicon | no | - | No EM2 model |
| B70 | After EM2 wake HFXO.STATUS=0x40000000, DPLL.STATUS=0, SYSCLK on free-running HFRCO (-0.07 %) | §7.74 silicon | no | - | Not modelled |
| B71 | M0+ access costs (cycles): SEQRAM ld 2 / st 2.5, SRAM 3.25, radio 4.25 back-to-back (5 isolated), BUFC 3.25 (4 isolated), TIMER1 8.75, CMU 4.75; IRQ entry 16 | §7.74/7.75 silicon | yes (gaps) | efr32-interconnect.c timing table + idle wait state; M0+ SRAM window is I/O now. Probe harness on the model: radio 4.25 (isolated 5.1), BUFC 3.25, SRAM 3.25, TIMER1 8.75, CMU 4.75, IRQ entry 16 | SEQRAM store 2 not 2.5 (fetch/store conflict inside the Armv6-M translator, not a bus access) |
| B72 | M33 access costs from flash: SRAM 1, SEQRAM 1.88, BUFC 1.88, radio/CMU/GPIO 3.88, TIMER1 11.88; from SRAM code: SEQRAM 3.6-3.9, radio 5.6-5.9, TIMER1 13.1, SRAM 1.5 | §7.74/7.75 silicon | partial | EFR32_M33_*_EXTRA_MILLICYCLES (~5710) | Flash-code numbers only; SRAM-resident code costs and flash data loads (4.5) not modelled |
| B73 | M33 cycles per instruction class: ALU, ld/st 1, NOP 0.5, taken branch 2; flash data 4.5 (WS1), SRAM-resident code: SRAM 1.5, flash 4.0 | §7.75 silicon kernels | partial | target/arm/tcg/translate.c armv8m_main_extra_mc, icount_set_cpu_mem_costs (§7.93); qtest core-clocks-m33-access-costs | Placeholders: multiplies, divides (12), LDM/PUSH per word, IT folding, load-to-store, instruction-cache misses. Model runs silicon's crypto 6-22 % and radio paths 20-35 % short (§7.93) |
| B74 | M33 exception entry 17 cycles (18 from WFI), SRAM vectors | §7.74 silicon | yes | EFR32_M33_EXCEPTION_ENTRY_MILLICYCLES 17000 | Flash vectors [P]; WFI +1 not modelled |
| B75 | SEQRAM single port, M33 priority: each M33 SEQRAM access costs M0+ ~0.9 cycle; M33 SEQRAM 1.88 -> 2.38 while M0+ executes | §7.74 silicon | yes | efr32-interconnect.c SEQRAM port: M33 +0.5 from flash code only, M0+ loses 1.2 cycles per M33 SEQRAM access while it runs from SEQRAM, 0.35 in the short gaps between its bus accesses [I fitted]. Probe harness: M0+ SEQRAM ld 6.05 (6.01), ALU 3.35 (3.46), bus loops -1..-17 % | No qtest (probe harness, scratch) |
| B76 | Radio-register path contention (RAC/FRC/SYNTH/MODEM shared, BUFC separate): M0+ 4.25 -> 9.2-9.8 under M33 streaming; M33 3.88 -> 4.86; BUFC 3.25 -> 4.77 | §7.74 silicon | partial (gaps) | efr32-interconnect.c RADIO/BUFC ports, round-robin with handover [I fitted]. M0+ RAC under M33 RAC: 9.0 (9.19, SRAM-code M33), 8.2 (9.83, flash M33); BUFC 4.76 (4.77). M33 side +0.55 (+0.42 SRAM code), +0.87 (+0.98 flash) | M0+ under flash-resident M33 radio streaming 16 % short |
| B77 | SRAM contention: M0+ +0.7..+1.1 cycle/access under M33 SRAM traffic; M33 +0.15..+0.31 under M0+ | §7.74 silicon | partial | M0+ SRAM accesses go through the interconnect (base 3.25 now right); M33 SRAM and flash accesses are now costed by the translator (icount_set_cpu_mem_costs, §7.93) | Their occupancy still does not reach the interconnect, so M33 SRAM traffic does not slow the M0+ |
| B78 | Peripheral-bus (TIMER1/CMU) mutual contention +9 % .. 3.5x; M0+ +0.5 cycle/peripheral access while M33 does SRAM traffic or WFI | §7.74 silicon | partial (gaps) | efr32-interconnect.c HF APB port. M0+ side: TIMER/TIMER 21.0 (23.3), CMU/CMU 9.2 (9.1), CMU/TIMER 17.7 (19.3), TIMER/CMU 13.3 (13.3) | M33 side low (TIMER/TIMER +1.2 vs +4.2 cycles); the +0.5 under M33 SRAM traffic as B77 |
| B79 | M0+ IRQ entry 16 -> 23-59 cycles (median 45-51) while the M33 streams SEQRAM | §7.74 silicon | yes (gaps) | efr32_ic_seq_exception_millicycles: 14 + 31 x the M33's SEQRAM utilization in the last round. Probe: 16 alone, 48 busy (47), 38 from WFI (45, range 23-51) | - |
| B80 | Peripheral access latency follows HCLK (HCLKPRESC DIV4: TIMER1-bracketed M0+ chunk 484 -> 520 ticks) | §7.74 silicon | no | bus extras are fixed core cycles | - |
| B81 | LDMA into SEQRAM slows M0+ like M33 streaming (ALU 1.66x, SEQRAM ld 1.87x); LDMA.EN clear does not stop a busy self-linked channel | §7.74 silicon | partial (gaps2) | efr32-ldma.c: EN is not a run gate, CHDIS stops; LDMA error on a radio register | LDMA is not an interconnect manager: the contention is not reproduced |
| B82 | M0+ address map: SEQRAM at 0, radio at 0xA8.., BUFC 0xAA.., NS aliases 0xB8/0xBA/0xB0/0xA0 HardFault, SRAM shared at 0x2000_0000, 0x5xxx periph OK, 0x4004C024 no fault but constant | §7.74 silicon | yes | bg22.c:efr32_soc_realize seq_memory (~6788), efr32_seq_periph_s_read returns 0 | Secure-window writes dropped [U] |
| B83 | efr32_seq_delay pass costs 7 or 8 cycles depending on SEQRAM alignment (delay(n) ~1.18n or 1.35n us) | §7.75 silicon | partial | Armv6-M per-class cycle counting | Alignment-dependent fetch cost not modelled (always 7) |
| B84 | M0+ SysTick CALIB = NOREF+SKEW+RAC.STCTRL.STCAL (0xC0061A7F) [I] | §7.75 [I] | no | T: rac-stctrl-mask only | CALIB does not follow RAC.STCTRL |
| B85 | BURTC: COMP/CNT/START/STOP take effect at 3rd LF edge after write (2.02-3.00 P); old values read meanwhile; STOP lets that edge count; START first increment at 4th edge | §7.73 silicon (+unifying rule [I]) | yes | bg22.c:efr32_burtc_pend/process (~2877), EFR32_BURTC_SYNC_EDGES 3; T: burtc-compare-lead, burtc-retarget-window, burtc-compare-pending | - |
| B86 | BURTC IF.COMP sets on edge CNT COMP->COMP+1; lead d>=3 fires, d<=2 waits for wrap; retarget k<=2 still fires old | §7.73 silicon | yes | efr32_burtc_match_edge (~2832) | - |
| B87 | BURTC EN set -> SYNCBUSY clear 5.12-5.87 P; clear 3.50 P (falling-edge grid [I]) | §7.73 silicon/[I] | yes | EFR32_BURTC_EN_SET/CLR_FALLING_EDGES 6/4; T: burtc-en-sync | - |
| B88 | BURTC keeps counting with CLKEN0.BURTC cleared; bus access then errors (read and write) | §7.73 silicon | yes | efr32_timer_clock_gated BURTC branch; peripheral_mmio_write | - |
| B89 | BURTC IF -> NVIC pending within 0.6 us | §7.73 silicon | yes | efr32_burtc_update_irq | - |
| B90 | LFXO actual 32771.6 Hz (+110 ppm) vs HFXO-derived HCLK; HF tree -125 ppm vs LFXO | §7.73/7.74 silicon | partial | EFR32_LFXO_HZ 32768 nominal | Deliberately nominal (crystal-specific) |
| B91 | TIMER rate EM01GRPACLK/(PRESC+1); START/STOP act at the bus write (+-1 tick); CMD SYNCBUSY 38 HCLK cycles (0.49 us) | §7.73 silicon | partial | efr32_timer_hz (~3233), EFR32_TIMER_CMD_SYNC_HCLK_CYCLES 38 at EFR32_MEASURE_HCLK_HZ (~3569); T: timer-cmd-syncbusy | SYNCBUSY converted at fixed 76.8 MHz, not the current HCLK |
| B92 | TIMER OC=CNT fires at end of current tick; OC=CNT+1 one tick later; START with CNT==OC after one tick; OC=CNT-1 waits for wrap; no arming lead (fires iff d>=19 within a 26-tick window) | §7.73 silicon | yes | efr32_timer_schedule (~3291); T: timer0-compare-equal-fires-next-tick | - |
| B93 | TIMER EN rules: EN=0 writes to CTRL/CMD/TOP/TOPB/CNT/CCn_CTRL/OC/OCB/DTCTRL bus-error, reads 0, values kept; EN=1 writes to CFG/CCn_CFG/DTCFG bus-error | §7.73 silicon | yes | efr32_timer_write_rejected (~3221), efr32_timer_read; T: timer-sync-registers-read-zero-when-disabled, timer-bus-faults | - |
| B94 | TIMER keeps counting with its CLKEN cleared; gated access bus-errors (read, and write sticky) | §7.73 silicon | yes | efr32_timer_clock_gated | - |
| B95 | Gated read errors also for RTCC, PRS, PROTIMER, LETIMER0; PRS SWLEVEL gated write errors | §7.73 silicon | yes (gaps2) | PROTIMER via CLKEN1 (B40); RTCC CLKEN0 bit 30; efr32-prs.c prs_gated, efr32-letimer.c lt_gated; T: timer-bus-faults, prs-bus-gate, letimer-oneshot-pwm | LETIMER0 gated writes [I] |
| B96 | TIMER/PRS registers fault while CMU.CLKEN0 bit clear (probe finding) | §7.71 silicon | yes (gaps2) | TIMER; efr32-prs.c; T: prs-bus-gate | - |
| B97 | PRS async MODEM events need INSEL=PRSASYNCPULSE; CCn.ICF debugger reads don't pop FIFO (ICOF); TIMER input capture; consecutive SWPULSE stores 10 TIMER1 ticks apart | §7.71/7.73 silicon | partial (gaps2) | efr32_timer_ic_* (2-clock sampler rejects pulses, debugger reads by requester id); T: prs-timer-input-qualification, prs-swpulse-timer-capture, rf/prs-radio-probe-routes | the 10-tick store spacing is the interconnect's PRS store cost, not checked on the model |
| B98 | FSRCO 19.804-19.811 MHz | §7.73 silicon | yes (audit) | EFR32_FSRCO_MEASURED_HZ 19807000 for SYSCLK/HFXO timing; efr32_em01grpa_hz (~2331) uses EFR32_FSRCO_HZ 20000000 | **Closed:** EM01GRPACLK on FSRCO counts at 19.807 MHz. qtest `timer0-follows-em01grpaclk-dpll` |
| B99 | HFXO cold start: ENS 25, RDY 5455-5480, COREBIASOPTRDY 5663-5665 FSRCO cycles; STATUS 0x40000000 -> 0x40010003 (FORCEEN), 0x40030003 with HWREQ | §7.73 silicon | yes | EFR32_HFXO_*_FSRCO_CYCLES, efr32_hfxo_status (~2586); T: hfxo-ready-status | Warm restart [P] |
| B100 | DPLL relock 11588 HFXO cycles (301.8 us); disable ENS clear 38-41 cycles; STATUS bit31 = 0 | §7.73 silicon | yes | EFR32_DPLL_LOCK_HFXO_CYCLES, efr32_dpll_status; T: dpll-lock | ENS-after-EN, LFXO-ref lock [P] |
| B101 | LFXO TIMEOUT 4K: ENS -> RDY 124.6-125.4 ms | §7.73 silicon | yes | efr32_lfxo_ready_ns; T: lfxo-ready-timeout | ENS latency [P] |
| B102 | LFRCO FORCEEN: ENS 46-47, RDY 16048-16049 HCLK cycles; clear -> ENS clear 51-65 | §7.73 silicon | yes | EFR32_LFRCO_* (~2440) | - |
| B103 | CLKEN set -> peripheral accessible on the next store | §7.73 silicon | yes | immediate | - |
| B104 | RADIOAES hardware run = start + per-block: ECB/CMAC 80+55 cycles/block, CCM 105+56 (ECB 1 blk 133 = 1.73 us; CCM 27 oct 495/496 = 6.4 us; 251 oct 2063 = 26.9 us) | §7.81 / crypto.md §4 silicon | yes | efr32-radioaes.c RADIOAES_START_NS 1042, BLOCK_NS 716, CCM_START_NS 1367, CCM_BLOCK_NS 729 (~104) | Fixed ns at 76.8 MHz; not scaled if the clock differs ([I] which clock); qtest radioaes-ecb-and-key-sizes pins the ECB run time (gaps) |
| B105 | RADIOAES idle STATUS 0; INCL_IPS_HW_CFG reads 0x00000001 (header says 0x11) | crypto.md silicon | yes | efr32-radioaes.c INCL_IPS_HW_CFG_RESET, STATUS_IDLE | - |
| B106 | RADIOAES CCM with empty payload: hardware tag is not SP 800-38C's and decryption of its own output fails | crypto.md silicon | no | efr32-radioaes.c:do_ccm | Model presumably computes the standard result; not reproduced |
| B107 | RADIOAES bus clock CMU_CLKEN0.RADIOAES / RADIOCLKCTRL.EN gating | crypto.md [D] RM 8.5 | yes (gaps2) | efr32_bus_error; T: radioaes-clock-gate | RM-only evidence; fault rule [I] |
| B108 | TIMER0.CNT read by M33 0.156 us | §7.77 silicon | yes | M33 TIMER extra 10.875 -> 11.875 cycles = 0.155 us | - |
| B109 | RAC_RSM/RAC_SEQ M33 IRQs (37/38) come from RAC.IF & RAC.IEN (bits 0-3 / 16-23), not SEQIF & SEQIEN | §7.78 [D] public fields + stock behaviour | yes | efr32_rac_update_irqs | - |
| B110 | RDMAILBOX IF/IEN latch: mailbox 0 -> M33 IRQ 39, mailbox 1 -> M0+ IRQ 23 | §7.78 [I]/stock behaviour | yes | efr32_rdmailbox_update_irqs (~1473); T: rfmailbox-flags-latch | - |
| B111 | SYSCFG.CHIPREV.FAMILY follows DEVINFO.PART (BG22 = 53); flash/RAM sized from DEVINFO.MSIZE; access past flash end faults (ADDRFAULTEN) | §7.78 RM | yes | efr32_chiprev_from_part (~6378); T: part-352k-identity, part-352k-flash-end-faults | 352-512 KB hole behaviour unmeasured |
| B112 | MSC programming (WREN, ADDRB/INVADDR, ERASEPAGE, WDATA clears bits, PAGELOCK, MSC_LOCK) | §7.78 RM | yes | efr32_msc_* (~4732) | Erase/write durations and read stall [U] |
| B113 | RTCC counts at RTCCCLKCTRL/CNTPRESC, writes immediate, IF.CCn when CNT becomes OCVALUE [I] | §7.78 RM/[I] | yes | efr32_rtcc_* (~3688); T: rtcc-counts-and-compares | Edge timing unmeasured |
| B114 | EMU.RSTCAUSE: POR after power-up, SYSREQ on AIRCR.SYSRESETREQ, kept until RSTCAUSECLR | §7.78 RM/[I] | yes | T: emu-rstcause | - |
| B115 | IADC single conversion; conversion time | §7.78 RM | yes (gaps) | efr32_iadc_queue / efr32_iadc_complete: RM SS24.3.3 warm-up (TIMEBASE intervals, WARMUPMODE) and ((4 OSR)+2) ADC_CLK plus the measured first-result latency [F, 7.89]; T: iadc-single-conversion | silicon timing unmeasured (triggers, 16/20-bit and window compare modelled in gaps2: iadc-triggers, iadc-resolution-and-window) |
| B116 | Unidentified SEQRAM 0x4000 user (reads 0xe5ecc0de, 0x01010201, 0xaabbccdd) | §7.72 silicon | no | - | Owner unknown; nothing to model yet |
| B117 | CMU gates RDMAILBOX/RDSCRATCHPAD/PRORTC/SYSCFG stop the stock M0+ lockups (§7.59 via §7.70) | §7.70 silicon (mechanism unknown) | partial | gate map B40 now covers them | Which access faulted for stock not identified |

### C. Debug port, probes, reset, watchdog, crypto, OTA, suit board, TODO.md

Evidence kinds: Si silicon or bench, RM reference manual, DS datasheet, Z Silicon Labs open source, Dump read from the suit.

| ID | Finding | Evidence | Modelled? | Where (file:symbol, qtest) | Gap |
|---|---|---|---|---|---|
| C1 | SW-DP DPIDR = 0x6ba02477 (EK and suit) | debug-port-model.md SW-DP table; every ocd log (e.g. haltrx38.log l.14), Si | yes | efr32-dap.c:EFR32_DPIDR ~l.89; qtest dap/dp-identity | - |
| C2 | AP0 (M33 AHB-AP) IDR = 0x84770001 | debug-port-model.md, read from suit, Si | yes | efr32-dap.c:EFR32_AHBAP_IDR ~l.131; dap/ap-identity-memory | AP0 BASE (model 0xe00ff003, architectural), AP1 IDR (model 0), TARGETID/DLPIDR (model 0x1), ROM table PIDRs not captured |
| C3 | AP3 is a second AHB-AP (IDR 0x84770001) to the radio Cortex-M0+ r0p1, CPUID 0x410cc601, 4 bp / 2 wp, needs deferred examine | hardware-bringup.md (DAP scan), Si | no | efr32-dap.c has AP0/AP1 only (efr32_dap_realize ~l.737) | add AP3 + Armv6-M debug unit (TODO swd item) |
| C4 | M33 r0p4, FPB 8 code comparators, DWT 4 ("target has 8 breakpoints, 4 watchpoints") | debug-port-model.md; haltrx38.log l.16-17; recover2-fpb3.log l.23-24, Si | yes | efr32bg22-debug.c:EFR32BG22_FPB_CODE_COMPARATORS ~l.26; CPUID in dap-test EFR32_M33_CPUID; dap/halt-registers-reset | - |
| C5 | DCI GET_STATUS debug-lock word: lock disabled, device erase enabled, secure debug disabled (EK and suit) | debug-port-model.md AP1, Si | partial | efr32-dap.c:dci_execute ~l.205; dap/dci-status | other status words not captured (read 0); AP1 IDR unknown |
| C6 | DCI device erase (0x430f0000) erases main flash and resets; used for stage 2 programming | hardware-bringup.md stage 2; series2.cfg, Si/D | partial | efr32-dap.c:dci_device_erase ~l.190 | erase is instantaneous; no qtest exercises erase; duration and SE behaviour unmeasured |
| C7 | `reset halt` never halts: "external reset detected" then timeout; DEMCR.VC_CORERESET not honoured (cause open) | debug-port-model.md quirk 1; bg22-openocd-flash.cfg, Si | partial | efr32bg22-debug.c vc-corereset-honoured=false ~l.52; armv8m-debug.c:dbg_op_reset ~l.346; e2e only | mechanism unmeasured (DEMCR cleared vs catch not taken); no qtest |
| C8 | FPB comparators survive SYSRESETREQ and catch; DHCSR 0x03130003, PC = reset handler, SP = initial MSP (5/5 EK, suit) | debug-port-model.md quirk 2, Si | yes | armv8m-debug.c:dbg_op_reset; dap/halt-registers-reset | - |
| C9 | DHCSR.S_SDE = 1 on the suit (0x01100001, 0x03130003); FPB halts the Secure bootloader | debug-port-model.md quirk 1, Si | yes | efr32bg22-debug.c secure-debug-enabled; dap/halt-registers-reset asserts S_SDE | - |
| C10 | After SYSRESETREQ the APs (incl. DCI) fail briefly while the DP answers DPIDR 0x6ba02477 | debug-port-model.md quirk 4 (run-control.log), Si | partial | efr32-dap.c:dap_ap_blocked ~l.161, efr32_dap_reset_hold ~l.705, prop sysreset-blackout-ns default 0 | duration and ack (FAULT vs WAIT) unmeasured; default 0 means no window; no qtest of the window |
| C11 | C_HALT while the stock app runs is not honoured (suit: DHCSR 0x01100001, then "external reset detected") | debug-port-model.md quirk 9, Si | no | armv8m-debug.c:dbg_dhcsr_write always halts | mechanism unknown (TODO measurement) |
| C12 | DWT PCSR samples a running core; all ones when halted | debug-port-model.md quirk 10, Si (suit) + Arm ARM | yes | armv8m-debug.c:dbg_dwt_read ~l.684 | no qtest (e2e only) |
| C13 | C_MASKINTS masks interrupts while running (radio runs during a halt so a pending IRQ is taken on resume) | debug-port-model.md quirk 12, Arm ARM | yes | armv8m-debug.c:dbg_dhcsr_write ~l.433 | no qtest |
| C14 | DEBUGRUN = 0 stops TIMER0-4, BURTC, RTCC, WDOG0 during an M33 halt [I: headers/SDK only, unmeasured] | debug-port-model.md quirk 11; TODO debug-resume item | yes | efr32bg22.c:efr32_debug_frozen ~l.2693, efr32_m33_debug_halted ~l.4190; dap/debug-halt-stops-timers, dap/wdog-frozen-in-debug, dap/wdog-debugrun | unmeasured: stop/restart clock edge, STATUS.RUNNING during halt, whether PROTIMER/RAC keep running |
| C15 | J-Link OB: 1366:0105, "Silicon Labs J-Link OB compiled Oct 20 2020 14:45:20", HW 1.00, SWD capped 1900 kHz, VTref 3.318-3.322 V (66 sessions) | debug-port-model.md probes table; haltrx38.log l.8-13; recover2-auto.log l.7-10, Si | yes | jlink.c model table ~l.176-185; efr32-dap.c vtref-mv 3320 ~l.764; tests/unit/test-jlink-swd-io.c | USB descriptors and capability words not captured; SWCLK timing not modelled |
| C16 | J-Link EDU Mini 1366:0101, ~4 MHz | debug-port-model.md, SEGGER product text (D) | partial | jlink.c EDU Mini entry ~l.188 | firmware string, HW version placeholders |
| C17 | **The J-Link OB drives the EK's nRESET**: SRST asserted/100 ms/deasserted -> one fresh "Booting Zephyr" banner on VCOM | recover2.sh srsttest; recover2-srsttest.log l.3-4 ("srst pulsed", "boot banners seen: 1"); vcom-srsttest.log l.4-5; hardware-bringup.md ("probe SRST is required"), Si | yes (audit) | jlink.c JL_CMD_SET_RESET/CLEAR_RESET accepted and ignored ~l.489 and default branch comment ~l.578 "no nRESET is wired on the EK"; swd-remote-bitbang.c ~l.14 "no SRST is wired, as on the EK"; docs/debug-port-model.md l.265-266 | **Closed:** `jlink.c` CLEAR_RESET/SET_RESET and `swd-remote-bitbang.c` 'r'..'u' drive the SoC's RESETn (RSTCAUSE.PIN, M33 held until release). qtest `dap/nreset-holds-core`, e2e step 7. The J-Link mapping has no unit test (needs the USB path) |
| C18 | **SW-DP does not answer while nRESET is held**: OpenOCD "Error connecting DP: cannot read IDR" (button held and J-Link SRST) | recover-step1.log l.11; recover2-auto.log l.11; recover2-fpb2/3.log l.11; recover2-srsttest.log l.1; hardware-bringup.md "SWD disappears while reset is asserted", Si | yes (audit) | efr32-dap.c has no reset input; swd.h has none | **Closed:** `efr32-dap.c` efr32_dap_set_nreset: no SWDIO drive while held; [I] power-on DP state after. qtest `dap/nreset-holds-dp-silent`, e2e step 7 ("cannot read IDR") |
| C19 | Right after nRESET release the DP answers at once (DPIDR 0x6ba02477) but the first AHB-AP write(s) to DHCSR fail ("Failed to write memory at 0xe000edf0") for 2 retry iterations; the 3rd succeeds | recover2-fpb3.log l.14-21; recover2-fpb2.log l.14-17, Si | no | efr32-dap.c:dap_ap_blocked only for SYSRESETREQ | AP window after a pin reset (same mechanism as C10?); duration and ack not recorded (retry loop, no timestamps) |
| C20 | A DHCSR C_HALT (0xa05f0003) written in the first ms after pin-reset release halts the Non-secure app (PC 0x0000996e, before the FPB at 0x3220 fires); S_HALT within 50 polls | recover2-fpb3.log l.21-26, Si | partial | armv8m-debug.c:dbg_dhcsr_write; no pin reset path to rehearse it | needs C17/C18 to rehearse the recovery in QEMU |
| C21 | **RAC XORETIMECTRL/DIGCLKRETIMECTRL ENRETIME (stock RXSEARCH limits) set in the RX tune wedges the debug port within ms of every boot**; per TODO DPIDR readable, AHB-AP and DCI dead; cause unknown | radio-tables.md §7.80 ~l.7423; TODO.md ~l.497; recover2.sh header, Si | yes (audit) | efr32bg22.c has no XORETIME/DIGCLKRETIME semantics (grep: none) | **Closed (mechanism unknown, see "The debug-port wedge"):** `efr32bg22.c` efr32_rac_retime_update -> `efr32-dap.c` dap_debug_wedge. qtest `dap/retime-wedges-debug-port`, `dap/retime-on-hfxo-sysclk-keeps-debug-port`, e2e step 7 |
| C22 | Attached debugger: M33 WFI does not gate the core clock (DWT counts); EM2 never entered whatever EM2DBGEN | TODO Hardware/tooling "Debugger effects" (cross-core-findings), Si | no | - | SoC debug-attached state |
| C23 | Each SWD AP access costs the M0+ ~1 cycle on the same target (SEQRAM, radio regs), up to ~5 on the peripheral bus (TIMER1); OpenOCD at 4 MHz ~17.6k accesses/s; M0+ SRAM loads under WFI +71 % attached vs +8.6 % detached | TODO "Debugger effects", Si | no | - | per-access bus cost of debug accesses |
| C24 | Debug reads have no side effects (capture FIFOs do not pop); a debug write's bus error is reported on the next transaction | TODO "Debugger/chip quirks", Si | partial | efr32-dap.c:mem_ap_access -> STICKYERR then FAULT on the next access | no MemTxAttrs.debug path in efr32bg22.c for side-effect-free reads |
| C25 | A debugger-halted M0+ (not in WFI) stops RAC leaving OFF | TODO quirks list, Si | no | no AP3 | needs C3 |
| C26 | Hardware single-step of the M0+ freezes the live counter at 0xa801c020 polled by stock IRQ11 | hardware-bringup.md, Si | no | no AP3 | needs C3 |
| C27 | All-zero TARGET_HALTED context: OpenOCD cortex_m_poll artefact, not chip | debug-port-model.md quirk 5, Si + source | yes (reproduced) | n/a (OpenOCD patch) | - |
| C28 | `resume 0x00000000` executes the vector table and faults; programming a running radio image can time out after erase; "erasing vectors can change observable core/debug state" | hardware-bringup.md; debug-port-model.md quirk 8, Si | partial | first is architectural (modelled); the other two have no known mechanism | measurement in TODO |
| C29 | Probe flash timing: 129,856 B erase/write/verify 2.72 s at 1900 kHz; WORKAREASIZE 0 path 121.87 s | hardware-bringup.md, Si | no | jlink.c SET_SPEED recorded only | SWCLK timing not modelled |
| C30 | EK: EFR32BG22 C224 rev 9, 512 KiB, 8 KiB pages; DEVINFO module name BGM220PC22HNA2, MODULEINFO 0x00570100 | hardware-bringup.md, Si | yes | efr32bg22.c EFR32_DEVINFO_MODULENAME0/MODULEINFO; (gaps) INFO 0x07090000 (rev 9 = PRODREV), PART 0x021608B0 (C224), MEMINFO 8 KiB pages, byte 0x1FE = 0x80 (OpenOCD's Series-2 family, as on the suit); qtest bgm220-module-identity | DEVINFO CRC, DILEN, calibration words never read (0); SYSCFG.CHIPREV major/minor (C32) |
| C31 | Suit: BGM220PC22WGA2, die C222F352GM40, 352 KiB, no LFXO, HFXO CTUNE 0x77 valid; flash map 0/0x6000/0x16000/0x4C000/0x56000 | suit-board.md, Dump | yes | devinfo=/userdata= options; qtests part-352k-identity, part-352k-flash-end-faults | silicon behaviour for 0x58000-0x80000 and SYSCFG_CTRL.ADDRFAULTEN=0 unknown (model faults, ignores ADDRFAULTEN) |
| C32 | SYSCFG.CHIPREV MAJOR/MINOR, CHIPREVHW (RM 0xD01 vs header 0xD02) | TODO QEMU model, RM | partial | efr32bg22.c EFR32_SYSCFG_CHIPREV ~l.530 (family only, major/minor 0) | read on silicon |
| C33 | MSC: 8 KiB page erase, word program via WRITECTRL/ADDRB/WDATA, only clears bits, locks; MISCLOCKWORD only clearable by root/debug (RM 6.8.13) | debug-port-model.md MSC, RM | yes | efr32bg22.c:efr32_msc_* ~l.4745; dap/msc-program, msc-status-reset | - |
| C34 | MSC erase/write durations, BUSY window, flash read stall | TODO (unmeasured) | no | model completes at once | measure (DWT around ERASEPAGE/WDATA) |
| C35 | EMU.RSTCAUSE: POR at power-up, SYSREQ on SYSRESETREQ, kept until RSTCAUSECLR [I: accumulate vs replace unmeasured] | ota.md; efr32bg22.c comment ~l.471, RM | partial | efr32bg22.c rstcause ~l.471/~l.6563; qtest emu-rstcause | pin (C17), lockup, BOD, EM4, DCI causes not modelled; accumulate rule unmeasured |
| C36 | Gecko bootloader/AppLoader honour the RAM reset word only after SYSRESETREQ; 0x00000002 keeps AppLoader, 0xF00F0202 goes to its install path [E; silicon [I]] | ota.md, E | yes (E) | rstcause SYSREQ; emu-rstcause | confirm on a unit |
| C37 | WDOG period (2^(3+PERSEL)+1)/f: PERSEL 8 on 32768 Hz = 62.53 ms; WARNSEL quarters (75 % = 46.90 ms); IF is INTFLAG (clear does not clear IF.WARN) | watchdog.md, RM 27 | yes | efr32bg22.c:efr32_wdog_schedule ~l.4069, efr32_wdog_expired ~l.4090; board/wdog-config-lock-warning-clear | - |
| C38 | CFG write while EN = BusFault (RM 4.2.4.4.2); CMD is LFSYNC, clear pending 3-4 cycles (92-122 us), 2nd CMD write while SYNCBUSY.CMD = BusFault (RM 4.2.4.4.4) [RM only; TODO: unmeasured on silicon] | watchdog.md, RM | yes | efr32bg22.c:efr32_wdog_write_rejected ~l.4243, efr32_wdog_sync_done; board/wdog-config-lock-warning-clear | silicon check of the BusFault rule (TODO structural interconnect item) |
| C39 | WDOG0_LOCK (unlock 0xABE8) locks EN/CFG; CMU_WDOGLOCK (unlock 0x93F7) locks clock select, CMU_STATUS.WDOGLOCK | watchdog.md, RM 27.5.8, 8.5.4 | yes | efr32bg22.c EFR32_WDOG_LOCK_UNLOCK ~l.3987, EFR32_CMU_WDOGLOCK ~l.4004; same qtest | - |
| C40 | Timeout resets the device (except some EMU regs), RSTCAUSE.WDOG0; WDOG disabled and unlocked after any reset | watchdog.md, RM 12.5.14 | yes | efr32_wdog_expired -> qemu_system_reset_request; board/wdog-reset, dap/halt-registers-reset (RSTCAUSE.WDOG0) | TODO.md "WDOG0: EN reads 0, no timeout reset" is stale |
| C41 | EMU_RSTCTRL.WDOG0RMODE can block the WDOG reset; LOCKUPRMODE resets to 0 (lockup does not reset) | watchdog.md, RM 12.5.14 | yes (audit) | no RSTCTRL semantics, no lockup reset | **Closed:** EMU_RSTCTRL (0x10405) gates SYSREQ, WDOG0 and M33 Lockup resets; survives soft resets, restored by PIN. qtest `m33-lockup`, `rstctrl-blocks-sysresetreq` |
| C42 | EM2RUN/EM3RUN freeze in EM2/EM3; WDOG does not run in EM4 (RM 27.3.3); warning is an EM2 wake source | watchdog.md, RM | partial | comment ~l.3955: model always runs in sleep (EM2RUN=EM3RUN=1), LOG_UNIMP otherwise | EM depth not modelled |
| C43 | Stock firmware: WDOG0 PERSEL 8 on 32.768 kHz LFRCO = 62.5 ms | stim-simulation.md finding 4, Dump | yes | same WDOG model (stock reproduces 62.3 ms in E) | - |
| C44 | Radio block EN needs CMU.RADIOCLKCTRL.EN and the block's CLKEN1 first | hardware-bringup.md, Si | yes | efr32bg22.c:efr32_bus_error ~l.5626; radio-enable-requires-bus-clocks | - |
| C45 | Gated radio read = bus error, gated write dropped; CLKEN1.BUFC gates nothing visible; holes 0xa8008000, 0xa8030000-0xa80fffff always bus error (§7.72) | efr32bg22.c comment ~l.5609, Si | yes | efr32_bus_error; gated-bus-clock-faults-and-drops, rf/radio-block-clock-gates | timers and (gaps2) PRS fault on gated writes; TODO: re-measure radio writes |
| C46 | RAC in SHUTDOWN (13): FORCEDISABLE leaves it in SHUTDOWN | hardware-bringup.md, Si | yes | efr32bg22.c:efr32_rac_force_disabled ~l.1541; rac-force-disable | - |
| C47 | Enabling RAC before BUFC/PROTIMER latches SHUTDOWN (vendor-sequencer path) - **but** §7.71: the open image works with PROTIMER.EN never set (9 SCAN_RSPs received), only RAC.STATUS bit 28 differs | hardware-bringup.md; radio-tables.md §7.71 ~l.5884; TODO "Radio prerequisites", Si | yes (audit) | efr32bg22.c RAC_EN write ~l.5442 latches SHUTDOWN whenever PROTIMER.EN=0, unconditionally; qtest rac-enable-requires-dependency-order | **Closed:** as B37 |
| C48 | CPUWAITDIS re-write does not restart the M0+; only held->released starts it | hardware-bringup.md, Si | yes | radio-sequencer-release-is-idempotent, radio-sequencer-executes-open-code | - |
| C49 | SMU AHBRADIO attribution: at reset A/AA active, B/BA RAZ/WI; clearing PPUSATD1.AHBRADIO swaps | hardware-bringup.md, Si | yes | efr32bg22.c SMU_PPUSATD1 ~l.5178; radio-nonsecure-alias, register-aliases | SMU peripheral access fault (PPU index 47) not modelled |
| C50 | M0+ store to B alias 0xB8022028 -> HardFault at PC 0x106; M0+ uses A/AA | hardware-bringup.md, Si | partial | seq_memory maps no 0xb8 window (efr32bg22.c ~l.6801-6835), so the access is unassigned | no qtest; fault type not asserted |
| C51 | Per-channel SYNTH.FREQ 0x01f46aab/0x01f96aab/0x0204aaab; RAIL writes the 2 MHz index to FRC.WCNTCMP2 and SYNTH.CHSP, CHCTRL = 0 | hardware-bringup.md, Si | yes | RF medium snapshots CHSP; rf/tx-offer-requires-matching-synth-channel | - |
| C52 | BUFC WRITEDATA: one 32-bit store per octet advances write offset | hardware-bringup.md, Si | yes | bufc-data-port | - |
| C53 | RXOVERFLOW sticks; only FORCEDISABLE (via SHUTDOWN) leaves it; FIFO full -> BUF1OF, no RXDONE | radio-tables §7.80, TODO, Si | yes | rf/released-rx-fifo-overflow | - |
| C54 | RX2TX -> TX start 180.6 us modelled vs 180.5 us measured; HFXO 276 us, DPLL 302 us | TODO QEMU model, Si | yes | rf/* turnaround tests; dpll-lock, hfxo-ready-status | HFXO warm restart placeholder |
| C55 | M0+ handler run time: TXWARM 91 us silicon vs 12.6 us model; TX2RX 123.5 vs 39 us | TODO QEMU model, Si | partial | per-core clocks; now 92.65 / 125.81 us (B48, B51, §7.93) | Sequencer timed wake 187 vs 136 TIMER0 counts (§7.93) |
| C56 | M0+ clock = SYSCLK/(RHCLKPRESC+1); M33 wins arbitration; per-access costs (§7.74) | TODO, Si | partial | core-clocks-per-cpu-rates, core-clocks-radio-clock-stops-seq; (gaps) efr32-interconnect.c | contention modelled per B75-B79; M33 SRAM traffic (B77) not |
| C57 | SYNTH LO-count BUSY window | TODO says unmodelled | yes | efr32bg22.c EFR32_SYNTH_LOCNTSTATUS ~l.108; rf/locnt-busy-window | TODO line is stale |
| C58 | RTCC compare edge: model per RM figure (IF when CNT becomes OCVALUE); BURTC fires one edge later; BURTC.COMP reads old value while pending (RM says pending) | TODO, RM vs Si | partial | rtcc-counts-and-compares; burtc-compare-pending | RTCC edge unmeasured; CCV0MATCH/PRECNT/capture unmodelled |
| C59 | EM2/EM3 effect on M0+, SEQRAM/radio retention (SYSCFG_RADIORAMRETNCTRL), wake latencies | TODO, DS/RM | no | - | unmeasured |
| C60 | RADIOAES run times at HCLK 76.8 MHz: ECB/CMAC 80 + 55 cycles per block, CCM 105 + 56 (ECB 1 block 133 cyc 1.73 us; CCM 27 B 495) | crypto.md §4, Si | yes | efr32-radioaes.c RADIOAES_START_NS ~l.104; no qtest (tests/crypto-port 330/330 in QEMU) | durations are fixed ns, valid only at 76.8 MHz HCLK |
| C61 | Idle STATUS = 0; INCL_IPS_HW_CFG reads 0x00000001 (header says 0x11) | crypto.md §4, Si | yes | efr32-radioaes.c INCL_IPS_HW_CFG_RESET ~l.59 | no qtest |
| C62 | Empty-payload CCM via SL descriptor sequence gives a non-SP 800-38C tag; decrypt of own output fails | crypto.md §3, Si | no | efr32-radioaes.c (no special case found) | decide whether to reproduce |
| C63 | No 192-bit keys in this BA411E | crypto.md §3, Z; BA411E_HW_CFG_1 0x05010127 read on silicon (g_Keysize 5) | yes (gaps) | efr32-radioaes.c: supported key sizes from HW_CFG_1; a 192-bit operation is reported (LOG_GUEST_ERROR / UNIMP) and computes nothing (pusher writes zeros); qtest radioaes-ecb-and-key-sizes | what silicon does with mode 192 is unmeasured |
| C64 | Bridge PB0/PB1 high sides, PB2/PB3 low sides; off = both low sides on; gate polarity high = on [I] | stock-stim-path.md, suit-board.md, Dump | yes | owo-suit.c bridge; board/gpio-drive-and-exti | polarity [I] |
| C65 | Pins high-Z from reset until configured; gate state while floating unknown [M] | suit-board.md, stim-simulation.md finding 8 | partial | owo-suit.c gate-float-on (default off) [U] | measure on a unit |
| C66 | DAC: USART1 SPI mode 0 8 MHz, 24-bit frame 0x3F hi lo, CS PA5, /CLR = expander P5; part LTC2634-class [I], full scale 2.5 V [I] | stock-stim-path.md, Dump | yes | owo-suit-parts.c LTC2634_* ~l.41 | part and full scale [I] |
| C67 | Channel switches: USART0, 32-bit chain, latch on PC3, readback with CS high; latch type [I] | stock-stim-path.md, Dump | yes | owo-suit-parts.c; board/usart-spi-switch-readback | latch type [I] |
| C68 | Expander PCF8575 at 0x40/0x20 on I2C0 PB4/PA7, /INT PC4; outputs weakly high from reset (datasheet); P2/P3 meaning [M] | suit-board.md, stock-stim-path.md, Dump/DS | yes | owo-suit-parts.c PCF8575; efr32-i2c.c; board/i2c-expander | TODO "I2C0 controller unmodelled" is stale |
| C69 | Unknown I2C device at 0x69 | stim-simulation.md, Dump | partial | owo-suit.c unknown-0x69-present (acks only) | identity [U] |
| C70 | Sense on PD2, battery on PD3 via 33/133, IADC 0xB2/0xB3, VDDX ref 3300 mV, OSR 2x | stock-stim-path.md (corrected), Dump | yes | owo-suit.c battery-div-num/den ~l.1006; board/iadc-scan-board-battery | (gaps) IADC conversion/warm-up time from the RM [I] (B115) |
| C71 | Boost PWM TIMER1 CC0 on PC7, 4 kHz 50 % | stock-stim-path.md, Dump | yes | board/timer-pwm-pin | rail physics [U] |
| C72 | External DAC/expander keep state across MCU reset | stim-simulation.md (E), physics [I] | yes | owo-suit.c (parts not reset with SoC) | - |
| C73 | USART frames take real time at the programmed baud; full RX buffer -> RXOF "as on silicon" | sim/README.md, RM | partial | efr32-usart.c ~l.40; usart-tx-polling, usart-frame-time | TODO: IRQ-line flag split / IF level semantics to calibrate on silicon; sync mode SPI exists for board; (gaps2) frame time no longer overflows above 8.4 MHz PCLK |
| C74 | LFRCO precision mode about +-500 ppm on the suit (no LFXO); BURTC timings were measured on the kit's LFXO | suit-board.md, DS | n/a | - | re-measure on LFRCO |
