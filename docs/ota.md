# Over-the-air updates

Goal: the open firmware can be installed with the suit's normal Bluetooth update path, and an
installed open firmware can be replaced the same way (including by the stock firmware).

**Status: built and linked; never run on a suit.** Everything marked [I] is an inference.

## How the stock path works
- The app writes `0x00` to the OTA Control characteristic (`F7BF3564-FB6D-4E53-88A4-5E37E0326063`) of
  the OTA service (`1D14D6EE-FD63-4FA1-BFA4-8F47B42119F0`). The application then reboots into the
  Gecko Bootloader; its AppLoader advertises as `OTA...`, receives the GBL over the OTA Data
  characteristic and writes it. "The rest of the OTA upgrade is managed by AppLoader and no further
  actions are needed from the user application"
  ([Silicon Labs, Bluetooth OTA upgrade](https://docs.silabs.com/shared-content/1.0.6/using-gecko-bootloader-with-bluetooth-apps/03-bluetooth-ota-upgrade),
  "What the Application Does on 0x00"). The application must not contain the OTA Data characteristic.
- So the open firmware needs three things, all done here:

| Need | How | Where |
|------|-----|-------|
| Be linked where the bootloader expects the app (0x16000, as in every stock GBL) | flash partitions in devicetree + `CONFIG_USE_DT_CODE_PARTITION` | suit: the board (`boards/owo/owo_suit/owo_suit.dts`); kit: `boards/bg22_ek4108a.overlay`; both `boards/<board>.conf` |
| Offer the OTA service with its Control point | GATT service, write-only, authenticated | `src/ble.rs` |
| Reboot into the bootloader when told | Zephyr's **boot-mode retention** API; the board's devicetree places the retention area at the start of RAM with the bootloader's reset-reason word as its prefix (`0x0202` BOOTLOAD, `0xF00F` valid) | `boards/owo_product.dtsi` (included by every board overlay), `src/ota.rs` |

  The reset-reason word is what `bootloader_rebootAndInstall()` writes before resetting
  (`platform/bootloader/api/btl_interface.c`, `btl_reset_info.h`, Gecko SDK 4.5.1). Using the
  retention API means the board description, not code, decides where the word lives.
- On the control write the firmware stops all output first (`stop_all`), lets the ATT response go out
  (200 ms), then resets.

## Packaging
`scripts/make-gbl.py app.bin out.gbl` writes the same GBL structure as the stock files (HEADER_V3,
APPLICATION type 0x20, one ERASEPROG tag at 0x16000, END with CRC-32). `--selftest stock.gbl`
rebuilds a stock file from its own image: it is byte-identical for all ten stock images. The image
is `zephyr.bin` of the build (linked at 0x16000: initial SP in RAM, reset vector in flash, checked
by the script).

## What is not known
- Whether the suit's bootloader accepts our file. The stock files carry no signature and the app
  distributes them to everybody, so an unsigned update is probably accepted **[I]**; the bootloader's
  configuration was never read, and application secure boot (set through OTP) would reject an
  unsigned application **[D]** (see `owo-open-firmware-feasibility.md` section 2).
- **Our image has no ApplicationProperties_t (the pointer in vector-table word 13 is 0), and on the
  suit that keeps it from starting [E].** In QEMU, running the unit's own bootloader and AppLoader
  (flash read from the suit) with the `owo_suit` image at 0x16000, the AppLoader stays in its OTA
  mode and the application never runs. The same image with an ApplicationProperties_t appended
  (magic, struct version 1.1, no signature, type Bluetooth application) and word 13 pointing at
  it boots: the bootloader hands over and the open firmware advertises. The stock application
  carries exactly such a struct (unsigned, type Bluetooth application). So the earlier reading
  ("assume valid" in `bootload_verifyApplication`) does not hold for this unit's AppLoader. The
  product must carry the struct before it is installed on a suit (open item in `TODO.md`).
- That the RAM word survives the reset into the bootloader exactly as in `bootloader_rebootAndInstall()`
  (the retention area is outside Zephyr's RAM, so the C runtime does not clear it).
- Solved: the suit's flash map (352 KiB: bootloader, AppLoader at 0x6000, application 0x16000 to
  0x4C000, stock NVM3 0x4C000-0x56000, a free last page) was read from a unit; see
  `suit-board.md`. The kit build keeps the kit's 512 KB map.
- If an image without a working OTA trigger is installed, recovery needs SWD.
