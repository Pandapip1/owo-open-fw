# ACTIVETXFCD / PROTIMER IRQ-handshake probe

A from-scratch, standalone bootable Cortex-M33 image (no Zephyr), with its
own real vector table from reset. Tests the final remaining sub-hypothesis
from the `ACTIVETXFCD` investigation (`docs/radio-tables.md` §7.25-§7.40):
does `FRC` need the CPU to actually take and service a real `PROTIMER`
(IRQ36) interrupt before it will advance `BUFC`'s TX `READOFFSET` past the
2-byte header? **Result: no.** See §7.40 for the full writeup -- real,
continuous `IRQ36` servicing for the entire test window did not change
anything; this is a clean, confirmed negative, not a confounded one.

`replay-table.inc` is generated (not hand-transcribed) from
`tests/hardware-lockstep/radio-setup-trace-partial.json` and
`radio-setup-trace-to-txen.json` -- real execution traces of the actual
firmware, filtered to thread-mode stores and with ARM system-control-space
addresses (`0xE000xxxx`: NVIC/SCB bookkeeping tied to Zephyr's own
scheduler) excluded, since those are meaningless and unsafe to replay on a
bare-metal image with no real scheduler (caught the hard way: the first
build took a real `PendSV` into the fault stub the instant the included
`SCB.ICSR=PENDSVSET` write executed). If the trace JSONs ever change,
regenerate this file the same way rather than hand-editing it.

## Building

```sh
./build.sh            # writes build/probe.elf, build/probe.bin
```

## Running

Requires the real open M0+ sequencer image. `seq-image.bin` (kept, 816
bytes) is already exactly this -- extracted directly from the production
firmware's own live SEQRAM contents (`dump_image` of `0xb0000000`, size
`0x330`), not rebuilt from source. Its first word equals
`EFR32_SEQ_STACK_TOP` (`0x3E00`) as a sanity check. If the sequencer
object ever changes, re-extract the same way (confirm the new size via
the `efr32_radio_seq_image_size` symbol) rather than hand-editing this
file:

```sh
nix develop --command bash -c '
openocd -f scripts/bg22-openocd-flash.cfg -f board/efm32.cfg \
  -c "init" -c "bg22_require_sram_execution" \
  -c "dump_image tests/activetxfcd-probe/seq-image.bin 0xb0000000 0x330" \
  -c "shutdown"
'
```

Then flash this probe as the *entire* flash image (replacing the real
firmware temporarily -- reflash it afterward, this does not coexist):

```sh
./scripts/flash-bg22-openocd.sh tests/activetxfcd-probe/build/probe.bin
```

Then, with OpenOCD's config sourced, catch the probe's own reset handler
(before it releases the sequencer from reset), load the sequencer image,
and let it run:

```tcl
init
bg22_require_sram_execution
bp 0x100 2 hw
bg22_reset_to_breakpoint
rbp 0x100
load_image tests/activetxfcd-probe/seq-image.bin 0xb0000000 bin
reg xpsr 0x01000000
resume 0x100
sleep 450
# force a halt and read BUFC READOFFSET/WRITEOFFSET, FRC.STATUS,
# RAC.STATUS, PROTIMER.IF as usual.
```

Unlike every probe in `tests/protimer-probe/`, this one does **not** need
the `0x20000000`/scratch-address RAM-collision care those required --
this image owns the entire 32KB SRAM itself, with nothing else running.

**Reflash the real firmware afterward** -- this probe replaces it on
flash entirely while it runs.
