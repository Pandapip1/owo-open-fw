# QEMU simulation of the OWO suit (EFR32BG22)

**Unvalidated on hardware. Bench-test the output stage on a dummy load first.**

* `vendor/qemu/` — pinned QEMU v9.2.0 fork adding the `efr32bg22` machine
  (Cortex-M33, IDAU, a preloaded RAM-backed flash image, and an in-process
  native MMIO model). Flash contents survive CPU reset in one process but are
  not written back to the input image or retained across separate QEMU runs.
* `sim/bootloader/` — hand-configured build of the real Gecko Bootloader main stage
  (sources from gecko_sdk, see `Makefile`; app base moved to 0x16000 to match the suit).
  SHA-256 is a software shim and ECDSA verification is a stub: signatures are NOT checked.
* `sim/scripts/` — `mkflash.py` composes bootloader + app into a flash image,
  `run-qemu.sh` / `iter.sh` run it, `disasm_stock.py` disassembles the stock image.

## Building and running the simulator

Enter the project environment first:

```sh
nix develop
mkdir -p build/qemu
(cd build/qemu && ../../vendor/qemu/configure --target-list=arm-softmmu --disable-docs --disable-capstone --enable-slirp)
ninja -C build/qemu qemu-system-arm
```

The build directory is deliberately retained as the incremental QEMU cache. The launcher uses
`build/qemu/qemu-system-arm` by default; set `QEMU_BIN` only to run a different binary. It writes
QEMU's own transient runtime log under `build/qemu-runtime/`; remove that directory after inspection.

For native RAC command diagnostics, enable QEMU's built-in trace event (not an external
peripheral server):

```sh
build/qemu/qemu-system-arm -trace efr32bg22_rac_command,file=build/qemu-runtime/rac.trace ...
```

It records written command words and every modeled RAC state transition, including timer-driven
completion. The trace file is transient and belongs under `build/qemu-runtime/`.

## Status
* The current in-process model cleanly transfers from the bootloader reset vector to the stock
  application's reset vector. A bounded native-QEMU capture reached early radio initialization:
  it recorded 19 radio writes, including the eight `+0x004 = 1` setup accesses documented in
  `docs/radio-firmware-plan.md`. This does not establish sequencer loading, a RAC command, or a
  radio transaction. The retained `evidence/stock/radio-trace.log` remains historical register
  evidence from an older external socket bridge and must not be treated as native-model
  end-to-end validation.
* Native CPU tracing found and cleared one early boot dependency: stock application code at image
  file offsets `0x5e90..0x5e9c` (`0x0001be90..0x0001be9c` when loaded) enables DCDC control
  through the SET alias and waits for `DCDC.IF.REGULATION`. The public xG22 header names that
  bit 5 (`efr32bg22_dcdc.h:276-279`); the QEMU DCDC shim now presents it after control enable.
  A subsequent trace reaches the later HFXO setup sequence. Its read-only ready status is modeled
  with the public `RDY`, `COREBIASOPTRDY`, `ENS`, `HWREQ`, and `ISWARM` fields
  (`efr32bg22_hfxo.h:354-380`). This is boot-progress evidence only,
  not a claim that the native model reaches radio configuration beyond the bounded early setup
  capture or completes a radio transaction.
* Radio blocks have an in-process native register model with source- and trace-backed RAC,
  BUFC, PROTIMER, FRC, MODEM, AGC, SYNTH, and RFCRC register handling. BLE packet traffic,
  sequencer behavior, and air delivery remain unmodelled, so stock-firmware stimulation cannot
  yet be triggered from the outside. Identifying the on-chip dependency between native early
  setup and the historical extended RAIL configuration remains a separate validation item.
