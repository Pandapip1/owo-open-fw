# QEMU simulation of the OWO suit (EFR32BG22)

**Unvalidated on hardware. Bench-test the output stage on a dummy load first.**

* `vendor/qemu/` — pinned QEMU v9.2.0 fork adding the `efr32bg22` machine
  (Cortex-M33, IDAU, persistent flash, and an in-process native MMIO model).
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
* Stock 7.9.07 boots through the bootloader to its idle loop, feeds WDOG0 and leaves the
  H-bridge off (PB2/PB3 high), matching `docs/stock-stim-path.md`.
* Radio blocks have an in-process native register model with source- and trace-backed RAC,
  BUFC, PROTIMER, FRC, MODEM, AGC, SYNTH, and RFCRC register handling. BLE packet traffic,
  sequencer behavior, and air delivery remain unmodelled, so stock-firmware stimulation cannot
  yet be triggered from the outside.
