# BG22 hardware/QEMU differential debugger

`compare.py` connects simultaneously to OpenOCD's GDB server for the physical
board and QEMU's GDB stub. It can continue both M33s to the same software
breakpoint and compare named MMIO or RAM regions at that event boundary. It
uses Python's standard library and the common GDB Remote Serial Protocol, so
the workflow does not depend on proprietary J-Link host software.

This intentionally does not emulate a J-Link USB probe. The probe transport
does not contribute radio behavior, while placing the same debugger client
above OpenOCD and QEMU compares the target-visible contract directly. Use the
physical J-Link/OpenOCD path when reset, flash, or SWD behavior itself is the
subject of a test.

The AP scan documented in the sequencer-probe README established that OpenOCD
can attach to the application M33 on AP0 and the physical M0+ on AP3. Event
lockstep remains useful for M33 command launch, TXWARM, TX, and completion;
`--steps N` additionally single-steps the physical AP3 endpoint and QEMU's M0+
thread and reports the first complete-register divergence. Select a QEMU core
with `--qemu-thread p01.02` (confirm the identifier from its stop reply).
Reset leaves general-purpose registers architecturally unspecified; use
`--normalize-registers` at a matched checkpoint to copy QEMU's complete
register packet to hardware before instruction comparison. `--set-pc ADDRESS`
can then rewind both Cortex-M cores to a side-effect-safe fixture entry point;
for example, 16 steps from probe reset address `0x100` stop immediately before
its `WFI`, which should not itself be single-stepped without an interrupt.

Example, with both targets already halted and their servers listening:

```sh
python3 tests/hardware-lockstep/compare.py \
  --hardware 127.0.0.1:3333 --qemu 127.0.0.1:3334 \
  --continue-to 0x1234 \
  --region rac=0xb8020000:0x200 \
  --region frc=0xb8004000:0x200 \
  --region seq-trace=0xb0003000:0x100 \
  --region seq-scratch=0xb0004fdc:0x14
```

Use an address from the exact common ELF passed to both targets. A comparison
exit status of zero means all requested bytes matched, one means a structured
diff was produced, and two means the debugger operation failed.

If reset behavior requires one endpoint to be caught manually (for example,
OpenOCD must re-examine this board after SRST), stop it at the checkpoint first
and pass `--current hardware`. The other endpoint will still continue to the
requested address before the paired snapshot is taken.

For M0+ instruction lockstep, build `m33-seq-launch.S` with its linker script
and use its binary as QEMU's machine flash. Load the sequencer probe with a
generic loader device. The launcher performs SEQRESET/CPUWAITDIS from running
M33 code, after device reset and image loading; issuing CPUWAITDIS from another
generic loader has ambiguous reset ordering and can make the M0+ sample an
uninitialized vector table.

`m33-seq-txwarm.S` is the companion event fixture. It enables the radio bus
clocks, releases a preloaded M0+ image, and requests TXEN so QEMU asserts the
sequencer's external IRQ8/TXWARM input. It was used with the open probe to
regress the hardware-observed rule that rewriting an already-asserted
`CPUWAITDIS` control preset must not reset the running sequencer.

`trace_stores.py` is the AP3 characterization companion.  Given an objdump
listing, handler address, and GDB endpoint, it hardware-breaks at the handler,
single-steps until exception return, decodes Thumb store effective addresses,
and emits the observed MMIO writes as JSON.  It deliberately traces the live
core instead of inferring writes from static endpoints.  The 2026-10-27 stock
capture measured 1,564 steps/141 stores for IRQ8 and 178 steps/16 stores for
IRQ9.

For startup rather than an exception handler, pass `--stop-address ADDRESS`.
The tracer then stops before executing that address instead of waiting for an
exception-number transition.  This bounded the stock reset path from `0x2ade`
to the first WFI at `0x2bb6` without attempting to single-step the sleeping
core.

`radio-setup-trace-partial.json` is a real `trace_stores.py` capture of the
*open* stack's own M33, from `radio_phy_set()`'s entry through 159 real MMIO
stores (not reaching `RAC.CMD.TXEN` -- the capture is bounded by
`--max-steps`, not a real stopping point). Captured with `--mask-interrupts`
(masks `PRIMASK_NS`), which later traces found is unsafe for a span this
long: anything in the path that blocks on a `SysTick`-driven kernel tick
(`z_thread_halt`, `kernel/sched.c`) spins forever once interrupts are
masked, which is why this specific capture stops where it does rather than
reaching `TXEN`. Still valuable as-is: it is authoritative, directly-executed
evidence for the entire base PHY config table and `radio_setup()`'s own
writes, covering by far the largest, most transcription-error-prone part of
replicating this sequence (owo-open-fw docs/radio-tables.md SS7.37). A few
of its later entries (four consecutive `0xb8006104..0xb8006110 = 0x4000`
writes, and one `0xb0004ffc = 0x220bc` write) are to addresses this project
has not identified yet -- keep them as cited, captured values if reused,
not synthesized ones, until that's resolved. Retracing the remainder
(through to `TXEN`) needs `--current` resumed from this capture's exact
stopping PC, *without* `--mask-interrupts`, and tracking which store events
have `"exception": 0` (thread mode) versus nonzero (so real interrupt
activity during the longer un-masked trace can be filtered back out) --
not yet done, blocked on this session hitting unrelated host-system memory
pressure that made real-hardware single-stepping impractically slow, not a
hardware or technique problem.
