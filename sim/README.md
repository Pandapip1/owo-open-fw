# QEMU simulation of the OWO suit (EFR32BG22)

**Unvalidated on hardware. Bench-test the output stage on a dummy load first.**

* `vendor/qemu/` — pinned QEMU v9.2.0 fork adding the `efr32bg22` machine
  (Cortex-M33, IDAU, persistent flash, MMIO bridge to an out-of-process Python peripheral
  server in `tools/efr32bg22/`).
* `sim/bootloader/` — hand-configured build of the real Gecko Bootloader main stage
  (sources from gecko_sdk, see `Makefile`; app base moved to 0x16000 to match the suit).
  SHA-256 is a software shim and ECDSA verification is a stub: signatures are NOT checked.
* `sim/scripts/` — `mkflash.py` composes bootloader + app into a flash image,
  `run-qemu.sh` / `iter.sh` run it, `disasm_stock.py` disassembles the stock image.

## Status
* Stock 7.9.07 boots through the bootloader to its idle loop, feeds WDOG0 and leaves the
  H-bridge off (PB2/PB3 high), matching `docs/stock-stim-path.md`.
* Radio blocks (0xA8000000..) are undocumented in the public SDK; they are plain register
  files plus one inferred busy flag. BLE traffic cannot be simulated, so stimulation on the
  stock firmware cannot yet be triggered from the outside.
