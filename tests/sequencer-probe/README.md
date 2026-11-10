# BG22 radio-sequencer interrupt probe

This directory builds a tiny, fully open ARMv6-M image for the radio's private
sequencer core. It records the most recent exception number, `RAC.STATUS`,
`RAC.IF`, and `RAC.SEQIF` in `SEQ.SPARE0..3` (`0x4fe0..0x4fec`). A 16-entry
ring at local `0x3000` retains records in the order
`{exception, SEQIF, STATUS, IF}`; its monotonically increasing index is at
`0x4fdc`. The handler acknowledges the observed RAC IF/SEQIF sources to expose
any following hardware state event. It is a bring-up probe, not production
firmware.

The probe is C (`probe.c`), converted from assembly with the same register
writes in the same order. Two behaviours of that assembly are kept on purpose,
because silicon results were taken with them: the IRQ 8 gate wait polls bit 2
of `0xa8020040`, not the gate bit it pulses, and after IRQ 8 the handler
acknowledges `RAC.IF` with that poll mask (`4`) instead of the `IF` it read.

Build inside the repository's Nix environment:

```sh
nix develop -c tests/sequencer-probe/build.sh
```

Set `STOP_EXCEPTION` to trap at a recorded handler boundary for a stable
register capture, for example `STOP_EXCEPTION=25` for external IRQ 9/TX.

Load `build/probe.bin` at the M33 debug alias `0xB0000000` while the M33 is
halted, pulse `RAC.CTRL.SEQRESET`, and release the sequencer with
`RAC.CTRL.CPUWAITDIS`. The probe's first scratch value is `0x52535421` until an
exception occurs; external interrupt `n` is reported as `16 + n`.

The reset path first clears the public RAC sequencer state-event bank
(`SEQIF[29:16]`). A live breakpoint on the exact stage-2 image established
that this M0-side write is the boundary where RAC changes from SHUTDOWN to
OFF; the subsequent interrupt-enable write does not cause that transition.
This is peripheral state acknowledgement only. It does not dispatch, preempt,
or otherwise replace an M33 RTOS job.

Replacing SEQRAM stops the proprietary controller until the board is reset and
its normal initialization reloads the original image.

## Direct sequencer debugging

The BG22 exposes the radio Cortex-M0+ through CoreSight AP3 even though the
upstream OpenOCD xG21 target currently declares only the application M33 on
AP0 and the Debug Challenge Interface on AP1. The live AP3 ROM table identifies
an Arm Cortex-M0+ ROM, SCS, DWT, breakpoint unit, and CTI. Use the additive
target configuration with the repository's pinned OpenOCD:

```sh
result/bin/openocd -s result/share/openocd/scripts \
  -f interface/jlink.cfg -c 'transport select swd' \
  -f tests/sequencer-probe/xg22-dual-core.cfg
```

This provides M33 GDB on port 3333 and the radio M0+ on port 3334. The M0+
must be released from sequencer reset/CPU-wait before it can halt. A live test
of the open probe reported Cortex-M0+ r0p1, four breakpoints, two watchpoints,
`PC=0x120`, `SP=0x3e00`, and CPUID `0x410cc601`.

## Reliable M33 reset-to-breakpoint

Arming a hardware breakpoint that must fire exactly once execution reaches a
chosen address, with no race against however far the firmware has already
run, needs a real reset -- not OpenOCD's `reset halt`/`reset run`, which hang
or lose the race on this board (see `scripts/bg22-openocd-flash.cfg` for the
full account). `bg22_reset_to_breakpoint` in that file is the fix: arm
`bp <addr> 2 hw` while halted (after `bg22_require_sram_execution`), then call
the proc, which pulses `AIRCR.SYSRESETREQ` and polls until the pre-armed
breakpoint lands.

Verified on hardware, 5/5 each, starting from the halted state the previous
run left behind:
- `main()` at `0x0001a9a6` in `build-stage3/zephyr/zephyr.elf`, caught ~150ms
  after the reset write.
- `z_cstart()` at `0x00016d50` in the same image (reached well before
  `main()`), caught ~50ms after the reset write.

This chip's `silabs_s2_dci_read_se_status` (from `series2.cfg`) reports
"Debug lock: Disabled" but "Secure debug: Disabled" -- general SWD is fully
open, but the Armv8-M Secure state this part always resets into cannot be
halted by any mechanism (DHCSR, vector catch, or a breakpoint match) while
Secure debug is off. That is the confirmed reason `reset halt` hangs, and why
a literal halt at the true (Secure) reset vector is not obtainable here.
What *is* confirmed to survive `AIRCR.SYSRESETREQ` intact is the breakpoint
comparator itself, which is why arming it first and resetting second is
reliable without ever needing to catch the Secure entry point.

## Temporary characterization constants

This probe intentionally contains raw register values captured while tracing
the proprietary sequencer. They are evidence and bring-up fixtures, not an
acceptable production interface. Before any of this behavior moves into the
open radio module, replace each literal with named register/field definitions
or a typed PHY configuration record and document its public-register source or
the hardware phase that established it. Values whose meaning remains unknown
must stay explicitly named as opaque observed presets; do not silently promote
them into the production driver as unexplained magic numbers. The generated
machine code may of course contain the same immediate values.

On 2026-10-27 a hardware breakpoint on AP3 caught the stock controller at the
external IRQ8 vector (`0x2578`). Its prologue calls helpers at `0x7ac` and
`0x7be`: these write bit 18 to the RAC SET alias at `0xa8021040`, wait for bit
2 of the base word at `0xa8020040` to clear, and write bit 18 to the CLR alias
at `0xa8022040`. The probe reproduces this ordered, write-only pulse. A normal
register snapshot cannot reveal it, which explains why the earlier endpoint
comparison omitted it.

The same exact IRQ8-entry halt also corrected the probe's earlier PA snapshot.
At that boundary the working stage-2 image has `RAC.PAENCTRL=0x10000000`,
`PATRIM1=0x00038997`, `PATRIM3=0x00322d2a`, `PACTRL=0x10`, `RX=0x10`, and
`TX=0`. The probe and replay fixture retain those values only as explicitly
named observations. In particular, they are not a power-to-register mapping
and must not be presented as a dBm mapping.  The hardware HAL temporarily
applies this one provenance-named preset for every requested power so the PA
path can be tested; selectable power still requires the field definitions and
DEVINFO/board calibration source.

`generate-replay.py` can turn the JSON emitted by
`../hardware-lockstep/trace_stores.py` into `replay-probe.S`.  Generated
replays carry values traced from the stock controller, so they are kept in
`owo-bg22-proprietary-baselines/sequencer-probe-replay/`, not here; build one
with `SOURCE=<that path>/replay-probe.S ./build.sh build-replay`.  The generated
program is intentionally a blind, characterized-only store replay: comments
retain the source handler PC and trace step for every write, but it does not
reproduce reads, waits, or branches.  On hardware the complete IRQ8/IRQ9 replay
altered RAC state in the same direction as the smaller probe but still emitted
no observable advertisement.  That negative result is evidence that copying
the handler's raw writes is insufficient, and is another reason not to move
these constants into the production module.

For the hand-curated replay fixture, repeat `--extra-irq IRQ=TRACE` and pass
`--extra-output replay-captured-irqs.inc` to regenerate only captured tables.
They are deliberately not dispatched merely because a trace exists: hardware
showed that unconditional IRQ0/1/4/11 replays perturb the state machine.
`STOP_EXCEPTION=29` (for example) builds a fixture that halts at IRQ13 before
acknowledgment, allowing the stock handler to be traced against the natural
pending state.  If a stock handler polls a timer that freezes under debug,
`trace_stores.py --run-through FROM=TO` resumes only across that read-only
loop and stops on the supplied hardware breakpoint.
