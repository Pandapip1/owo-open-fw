# PROTIMER enable probe

`probe.c` is standalone Cortex-M33 code, independent of Zephyr and the real
driver. It is not a bootable image (no vector table): it is meant to be
injected into SRAM and run from a debugger-forced halt-point inside the
already-running production firmware, right before that firmware's own
`RAC.CMD.TXEN` write, then handed back to the real firmware to continue.

This exists to test one specific question cheaply and safely: does merely
enabling and clocking `PROTIMER` (no interrupt servicing, no scheduling
logic) change `FRC`'s TX frame-coder stall at all? See
`docs/radio-tables.md` §7.32/§7.33 for the full result (a clean negative)
and what it does and does not rule out. The other probes here are its
follow-ups (§7.34-7.36), each described in its own header.

Every probe enters through `PROBE_ENTRY` (`probe.h`): a short asm stub,
linked first at the injection address, that runs the C body and stores the
completion marker, then halts on `bkpt`. The stub keeps the halted host's
`r0`-`r3`, `r12`, `lr` and flags intact, so the host resumes at its
halt-point with the registers it stopped with. The original assembly probes
did not: they returned `r0`/`r1` holding the marker address and value, and
the VTOR probes also `r2` = 0 and `r3` = the last vector-table word. The
host's halted instruction is its own `TXEN` store (`str r1, [r3, #16]`, see
below), so unless the registers were restored by hand (the procedure below
does not), those runs stored the marker value (`TXEN` bit clear) or wrote to
the wrong address instead of issuing `TXEN`. Results from §7.32-7.36 taken
with the assembly probes should be read with that in mind; the MMIO writes
of each probe are unchanged (compared store by store in QEMU).

## Building

```sh
./build.sh            # writes build/<probe>.elf and .bin for every probe
```

## Running (reconstructing the exact test)

The real firmware's `RAC.CMD.TXEN` store address changes across rebuilds;
confirm it first:

```sh
arm-none-eabi-objdump -d build/radio-port-hardware-2/zephyr/zephyr.elf \
  | grep -A2 '<radio_tx_enable>:' # find the `str r1, [r3, #16]` TXEN write
```

Then, with the real firmware already flashed and OpenOCD's config sourced
(`scripts/bg22-openocd-flash.cfg`):

```tcl
init
bg22_require_sram_execution
bp 0x<TXEN_STORE_ADDR> 2 hw
bg22_reset_to_breakpoint
rbp 0x<TXEN_STORE_ADDR>

# Zephyr's MPU marks SRAM execute-never; bg22_reset_to_breakpoint's real
# SYSRESETREQ re-enables it after bg22_require_sram_execution's own disable
# from before the reset, so it must be disabled again here, or the injected
# code faults immediately (MemManage, observed and root-caused -- see
# docs/radio-tables.md SS7.33).
mww 0xe000ed94 0

load_image build/probe.bin 0x200007d0 bin
reg xpsr 0x01000000
resume 0x200007d0
wait_halt 5000
# marker at 0x200007f0 should read 0x51a7c0de: confirms the probe ran to
# completion without faulting.

reg xpsr 0x01000000
resume 0x<TXEN_STORE_ADDR>
sleep 450
bg22_require_sram_execution
# read BUFC READOFFSET/WRITEOFFSET, FRC.STATUS, RAC.STATUS as usual.
```

## Pitfalls hit building this (kept here so they aren't rediscovered)

- **Secure vs. non-secure address aliases matter even for debugger-injected
  code.** The probe runs inheriting the host firmware's own CPU security
  state (Non-secure). Using `PROTIMER_BASE = 0xa801c000` (secure) completed
  with no fault but every register write silently had no effect (read back
  as the register's power-on value). The non-secure alias
  `0xb801c000` (the same one the real driver uses for every other radio
  register) is required.
- **The MPU re-enables itself across `bg22_reset_to_breakpoint`'s real
  reset.** Disabling it once before the reset (inside
  `bg22_require_sram_execution`) does not persist; it must be disabled again
  (`mww 0xe000ed94 0`) after catching the post-reset breakpoint and before
  injecting/running anything in SRAM, or the CPU takes an immediate
  `MemManage` fault trying to fetch from there.
- **`reg pc <addr>` + plain `resume` is not equivalent to `resume <addr>`.**
  The former did not reliably reach the injected code's own `bkpt`
  (`wait_halt` timed out even with an 8s budget) in this setup; the latter
  works. Use `resume <addr>`.
- Single-stepping (`step`) through injected code at this SRAM address was
  unreliable here (`Could not read register 'pc'` on the very first step,
  even from a confirmed-clean core) -- useful for nothing worse than
  confirming it isn't the right diagnostic tool for this target; fall back
  to `resume`+`wait_halt` plus a forced halt (`mww 0xe000edf0 0xa05f0003`)
  if you need to catch a hang mid-flight instead.
