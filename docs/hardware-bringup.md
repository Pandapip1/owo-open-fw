# BG22 hardware bring-up plan

This is the required order for testing on the `bg22_ek4108a`. Do not advance to
the next stage until the current stage has been flashed, byte-verified, and
observed running. Bluetooth stages require an on-air advertisement in addition
to serial output. The OWO stimulation output overlay must remain disabled
throughout this plan.

1. **Silicon Labs SDK baseline (proprietary radio blobs)**
   - Build a minimal, non-Zephyr Bluetooth demo with the standard Silicon Labs
     SDK APIs.
   - Use open host tools (GNU Arm Embedded and OpenOCD) wherever possible.
   - Flash, verify, capture its serial boot result, and observe its BLE
     advertisement.
2. **Stock Zephyr baseline (proprietary radio blobs)**
   - Build the smallest stock Zephyr Bluetooth beacon for the board.
   - Flash, verify, capture successful controller initialization, and observe
     its BLE advertisement.
3. **Custom/open-driver Zephyr smoke test**
   - Build the smallest Zephyr application that uses this repository's
     custom/open drivers.
   - Keep all stimulation outputs disabled.
   - Flash, verify, and confirm the intended driver behavior on the board.

For every difference between real hardware and QEMU, record the evidence here,
then fix the simulator and add regression coverage before hardware bring-up is
declared complete. Dependency and QEMU downloads should run in the background.

## Evidence log

- Board probe: EFR32BG22 C224 revision 9, 512 KiB flash, 8 KiB erase pages,
  approximately 3.32 V target voltage.
- Factory flash backup (before the first write):
  `hardware-backups/bg22-ek4108a-000440200548-factory-2026-10-26.bin`, SHA-256
  `e9304dd343f8ad707daa062858f387ae0e0012e63c4f060c09102e03df1010e6`.
- Restoring and booting that backup produced neither serial output nor a BLE
  advertisement. Inspection shows a bootloader at address zero and erased
  application regions (including `0x6000`, `0x10000`, and `0x16000`), so the
  backup is a recovery image, not a factory demo control.
- OpenOCD 0.12 release can attach but cannot program this Cortex-M33 Series-2
  part. OpenOCD master at `cb0c224e936091156ecf3025ba05657408ed64c8`
  is now a pinned `packages.openocd` flake derivation (and dev-shell input) on
  Linux and Darwin. It detects the part with `target/silabs/xg21.cfg`, whose
  flash base is zero. The SRAM-accelerated EFM32 loader works, but the M33 can
  appear as `TARGET_UNKNOWN` after the application enters a low-power state;
  OpenOCD's ordinary `halt` then does not establish a polled `HALTED` state for
  the loader. Also, `flash write_image erase` can leave this part needing a
  second halt between page erase and SRAM execution. The reproducible
  `scripts/flash-bg22-openocd.sh` path issues the architectural DHCSR halt and
  then executes a checked SRAM sentinel fixture before any erase. It repeats
  that execution proof after explicitly erasing main flash and before starting
  the asynchronous write helper. If the post-erase fast path fails, it restores
  the requested image with AP writes rather than leaving the board blank. On
  the onboard J-Link at its 1900-kHz limit, the initial characterization path
  erased, wrote, and verified the 129,856-byte stage-2 image in 2.72 seconds;
  this measurement predates the two defensive execution probes. The old
  `WORKAREASIZE=0` recovery path took 121.87 seconds. Probe SRST must still be
  followed by `efm32.cpu arp_examine` because SWD disappears while reset is
  asserted.
  An all-zero PC/xPSR/MSP reported as `TARGET_HALTED` is specifically not an
  execution proof. A live stage-3 image reproduced that state while its MPU
  configuration could prevent arbitrary SRAM execution. The checked fixture
  therefore disables the selected-domain MPU, forces a valid Thumb xPSR,
  disables interrupts, writes `0x51a7c0de` to `0x200007f0`, and stops on its
  own breakpoint. Hardware validation observed that marker and
  `PC=0x200007d8`; the same proof also passed after all main-flash pages were
  erased. Fault injection immediately after that blank-flash proof exercised
  the complete slow fallback, which wrote and verified stage 3 before the
  wrapper returned a failure status. A subsequent uninjected stage-3 fast
  erase/write/verify passed and reset the board.
- The initial stock Zephyr experiment was performed before the three stages
  above were clarified. It is not a completed stage. With no libc arena the
  Silicon Labs controller returned `SL_STATUS_ALLOCATION_FAILED`; with an
  8 KiB arena it entered `RAIL_ASSERT_INVALID_MODULE_ACTION` during
  `RAIL_BLE_Init`. No advertisement was observed. This result must be revisited
  only in stage 2, after the SDK baseline passes.
- Stage 1 now builds and links with Nix-provided GCC 13.3.1, CMake, and Ninja,
  using SiSDK 2025.6.3 Bluetooth/RAIL archives. OpenOCD writes and byte-verifies
  the resulting image. Direct boot reaches RAIL initialization, proving the
  application, linker, reset, and startup paths are executing, but the radio
  currently asserts with code `0x31` before advertising.
- Hardware identity is subtler than the OpenOCD/board target suggests.
  OpenOCD reports an EFR32BG22 C224 die, while immutable `DEVINFO` module-name
  words decode to `BGM220PC22HNA2` and `MODULEINFO` is `0x00570100`. The
  bare-SoC RAIL archive therefore rejects the part with
  `RAIL_ASSERT_INVALID_MODULE_ACTION`. A module-targeted rebuild uses
  `BGM220PC22HNA`, the BGM22 startup/system layer,
  `librail_config_bgm220pc22hna_gcc.a`, and
  `librail_module_efr32xg22_gcc_release.a`; it still asserts at the module
  configuration check (source tag `0xa8`). This hardware identity and module
  enforcement are not represented in QEMU and require a simulator test.
- **Stage 1 passed on hardware.** Silicon Labs' open-source Arduino build
  frontend and its `ble_minimal` example were built with the vendor-published
  aarch64 GNU Arm 12.2 toolchain (fetched through the Nix store). The example
  was modified only to remove LED/output actions and change its serial banner.
  Its proprietary BGM220 Bluetooth/RAIL archives and matching bootloader were
  linked into `owo-arduino-stage1.ino.with_bootloader.hex`. OpenOCD wrote
  262144 bytes and byte-verified 255508 populated bytes. BlueZ then discovered
  `Silicon Labs Arduino` at `60:A4:23:C9:9C:90`, providing runtime and on-air
  confirmation. The SHA-256 of the application-only HEX is
  `2b2111ac9d16757b72e9e5fb59871c3b96a3e841fc4d28aea956c528c59de9f1`.
- The complete stage-1/stage-2 proprietary-baseline workspace is preserved
  outside this repository at
  `/home/gavin/Projects/owo-bg22-proprietary-baselines`.  In particular,
  `build-stage2/zephyr/zephyr.{elf,hex,bin}` are the stage-2 images that were
  flashed and observed above; the directory also retains the fetched SDK and
  blob inputs needed to reproduce those controls.
- The successful stage-1 reference targets BRD4314A/BGM220 VCOM routing, not
  BRD4108A routing, so `/dev/ttyACM0` remained silent even though BLE was live.
  Treat this as a pin-routing difference in the borrowed reference, not a
  failed runtime check; stage 2 should use the board's exact DTS UART routing.
- The J-Link OB reset behavior differs from the simple QEMU reset path:
  `resume 0x00000000` incorrectly executes the vector table and faults, while a
  Cortex-M reset or probe SRST is required. Programming a running radio image
  can also time out after erase, requiring a fresh debug session. The eventual
  flashing helper and simulator regression must encode the correct reset and
  launch contract.
- **Stage 2 passed on hardware.** Stock Zephyr 4.4.0's Bluetooth beacon sample
  was built for the exact `bgm220_ek4314a/bgm220pc22hna` target with the
  proprietary Silicon Labs link-layer, module RAIL, and BGM220PC22HNA radio
  configuration archives. The stock sample configuration initially failed
  controller initialization with status 25 because it left
  `CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE` at zero. Setting the arena to 8192
  bytes produced a 129856-byte flash image using 29624 bytes of RAM. OpenOCD
  wrote and byte-verified the image after a Series-2 DCI device erase. UART
  then reported successful EFR32 HCI initialization and advertising, and an
  independent BlueZ LE scan discovered `Test beacon` at
  `60:A4:23:C9:9C:90` (RSSI observed between -64 and -49 dBm). This completes
  the stock-Zephyr/proprietary-blob gate before stage 3.
- BlueZ's default `scan on` did not surface the non-connectable beacon during
  the initial observation windows; `scan le` did. This is a host scan-mode
  distinction, not evidence of a target/QEMU hardware divergence.
- The first stage-3 hardware build silently omitted the custom LLL because its
  Kconfig dependency named only `SOC_SERIES_EFR32BG22`; the module board uses
  `SOC_SERIES_BGM22`. QEMU's bare-die board masked that distinction. The
  dependency now accepts both series and the exact BGM220 hardware build has
  `CONFIG_BT_LLL_VENDOR_EFR32=y`. This is covered by building the board-specific
  smoke configuration.
- The next stage-3 run faulted at the first radio-block accesses. QEMU had
  accepted radio `EN` writes from reset, while silicon requires both
  `CMU.RADIOCLKCTRL.EN` and each block's `CMU.CLKEN1` gate first. The open HAL
  now enables HCLKRADIO plus AGC, MODEM, RFCRC, FRC, PROTIMER, RAC, SYNTH, and
  BUFC clocks before touching those blocks. QEMU now rejects pre-clock `EN`
  writes; `/arm/efr32bg22/radio-enable-requires-bus-clocks` proves that the
  block remains disabled with neither clock or only CLKEN1 and becomes writable
  only after both prerequisites. All 70 EFR32 qtests pass.
- A diagnostic secure-alias experiment produced an SMU peripheral access fault
  for AHBRADIO (PPU index 47). Zephyr intentionally assigns these peripherals
  to the nonsecure aliases during early Series-2 setup, so the open HAL uses
  the `0xB8xx_xxxx`/`0xBAxx_xxxx` views. QEMU still aliases secure and
  nonsecure radio windows and does not model the SMU attribution unit; this is
  a documented simulator limitation rather than evidence that secure aliases
  are valid for this firmware.
- QEMU's synthetic DEVINFO originally lacked the module identity that caused
  the stage-1 bare-die RAIL failure. It now exposes the observed
  `BGM220PC22HNA2` module-name words and `MODULEINFO=0x00570100`, with
  `/arm/efr32bg22/bgm220-module-identity` regression coverage.
- **Stage-3 controller smoke passed on hardware.** The exact
  `bgm220_ek4314a/bgm220pc22hna` build links Zephyr's split controller and this
  repository's `efr32-radio` LLL; neither RAIL nor the proprietary Bluetooth
  controller is present in the ELF, and no stimulation module is linked. A
  4096-byte common-libc arena fits the 32-KiB part (31504 bytes used) and avoids
  the PSA CMAC allocation error. After OpenOCD erase, write, and verification,
  UART remained fault-free for 15 seconds and reported `bt_enable: 0` and
  `adv_start: 0`. The same image class under QEMU reaches `RAC TXEN`, and the
  bounded smoke script reports both success markers.
- Stage 3 does **not** yet claim an on-air packet. An independent LE scan did
  not observe the controller's random identity, and the open HAL documents
  that FRC consumption of the staged BUFC FIFO and the complete sequencer
  handoff remain uncharacterized. Controller initialization and scheduling are
  runtime-confirmed; RF transmission remains the outstanding hardware gate if
  stage 3 is required to advertise over the air.
- The trace build exposed another QEMU/hardware divergence at `radio_reset()`.
  Silicon retained `RAC.STATUS.STATE=13` (SHUTDOWN), and asserting
  `CTRL.FORCEDISABLE` there left it in SHUTDOWN forever; QEMU had incorrectly
  completed that write by changing the state to OFF. QEMU now models state 13
  and `/arm/efr32bg22/rac-force-disable` checks it. The open HAL treats OFF and
  retained SHUTDOWN as quiescent and avoids the unavailable proprietary
  sequencer hand-off. Hardware subsequently reaches PHY setup, FRC/BUFC packet
  staging, the PROTIMER deadline, and `RAC.CMD.TXEN`.
- QEMU's simpler clock-event backend also hid two Cortex-M33 stack failures.
  Immediate diagnostic formatting overflowed the 448-byte TX launch stack, so
  the hardware-only trace configuration uses 768 bytes. More importantly, the
  production completion job's 512-byte default overflowed in
  `counter_silabs_timer_ce_arm_abs`; its module default is now 768 bytes.
  With those corrections the traced board remains alive after TXEN, but RAC
  reports `0x4d000000`, no TX completion arrives, and a simultaneous BlueZ LE
  scan still sees no project advertiser. PA setup, complete FRC framing, and
  the open sequencer transition remain the current on-air blockers.
- A direct open-sequencer execution test now narrows that last blocker. With
  the proven stage-2 image initialized, OpenOCD asserted `RAC.CTRL.SEQRESET`,
  replaced SEQRAM's vector table with SP `0x3e00` and reset vector `0x101`, and
  installed a four-instruction ARMv6-M program at local address `0x100`. After
  setting `RAC.CTRL.CPUWAITDIS`, that program stored `0x12345678` at local
  `SEQ.SPARE0` (`0x4fe0`); a separate debugger attachment read back exactly
  `0x12345678` and `RAC.STATUS=0x58000000`. Thus arbitrary open Thumb code can be
  loaded and executed by the radio M0+ core. The remaining task is the actual
  FRC/RAC TX event program and its simulator contract, not access to the core.
- The sequencer characterization probe currently includes literal register
  values captured from silicon. This is deliberate and temporary for reverse
  engineering. They must not be promoted into the final module source until
  represented by named register fields or typed PHY configuration records with
  provenance; unresolved values must remain visibly named opaque presets. It
  is expected that compilation may reduce those abstractions back to the same
  constants in the final machine code.
- Hardware/QEMU differential debugging now uses the same minimal GDB Remote
  Serial Protocol client against OpenOCD on silicon and QEMU's gdbstub. This
  avoids attributing J-Link USB/SWD transport behavior to the simulated SoC.
  M33 launch points and observable RAC, FRC, BUFC, sequencer scratch, and trace
  state can be compared at each boundary. `tests/hardware-lockstep/compare.py`
  emits word-level event divergences as structured JSON and also supports
  direct M0+ instruction stepping through AP3; the first mismatch becomes the
  required simulator regression.
- The original assumption that only event-level observation of the private
  sequencer was possible was disproved by a complete live DAP scan. AP3 is a
  second AHB3 MEM-AP (`IDR=0x84770001`) whose ROM table contains an Arm
  Cortex-M0+ ROM, SCS, DWT, breakpoint unit, and CTI. OpenOCD successfully
  examined it as Cortex-M0+ r0p1 with four breakpoints and two watchpoints,
  then halted the released open probe at `PC=0x120`, `SP=0x3e00`; CPUID was
  `0x410cc601`. `tests/sequencer-probe/xg22-dual-core.cfg` now declares that
  target and exposes it on the second GDB port. Hardware/QEMU work can therefore
  use instruction lockstep for the M0+ in addition to M33 event lockstep.
- AP3 subsequently trapped the known-good stage-2 controller directly at its
  external IRQ8 vector (`PC=0x2578`, `SP=0x3e10`). The handler's first helpers
  pulse RAC bit 18 through the SET/CLR aliases around a base-register wait;
  endpoint snapshots had hidden those write-only operations. The open probe
  now preserves this ordering. This is a hardware/probe discrepancy, not yet a
  QEMU discrepancy: the corresponding simulator behavior still needs a test
  once the pulse's state-machine effect is characterized.
- Running that corrected probe under QEMU exposed the next concrete simulator
  divergence: every write containing `RAC.CTRL.CPUWAITDIS` reset the M0+, while
  silicon's live IRQ8 handler rewrites `0x04000380` without restarting itself.
  QEMU now resets and starts the sequencer only on the held-to-released
  transition. `/arm/efr32bg22/radio-sequencer-release-is-idempotent` increments
  a reset-entry counter and proves a repeated CPUWAITDIS preset does not
  increment it. With the fix, the full probe records IRQ8 followed by IRQ9 in
  QEMU instead of resetting at IRQ8, and all 72 EFR32 qtests pass.
- Injecting the corrected probe into a live stage-3 run changed the initial RAC
  status from the clean-run `0x0d000000` to `0x1d400000` and the post-TXEN
  status from `0x4d000000` to `0x5d400000`, confirming that the released open
  sequencer changes the hardware state path. A simultaneous 18-second LE scan
  still did not observe the controller identity. The recovered IRQ8 gate pulse
  is therefore necessary characterization but not sufficient for RF output;
  the missing PA setup, complete FRC framing, and/or later IRQ8/IRQ9 handler
  operations remain in scope. The first attempted injection used ordinary
  `reset halt` and was rejected after its halt timeout; no image was loaded in
  that run. The successful attempt used DHCSR halt/poll with a valid live M33
  context before writing SEQRAM.
- AP3 single-step store tracing now gives an instruction-by-instruction bound
  on the stock sequencer handlers.  External IRQ8 returns after 1,564 steps and
  performs 141 radio-MMIO stores; IRQ9 returns after 178 steps and performs 16.
  Replaying every observed store, in order, from an open characterization image
  still changed the stage-3 RAC path from `0x0d/0x4d` to `0x1d4/0x5d4` without
  producing an on-air advertisement.  Consequently the endpoint pulse and the
  handler stores are not a self-contained transmitter program: prerequisite
  M33 FRC framing, PHY/PA setup, read-dependent waits, or a combination remains
  necessary.  The replay is retained only as a trace fixture, not production
  driver code.
- A breakpoint-aligned M33 snapshot removed one false lead from the comparison.
  Stage 3 writes `RFCRC.INIT=0x00aaaaaa` correctly after
  `radio_crc_configure()`; the neighboring stage-2-only `0x00aaaaaa` word is
  live CRC state after transmission, not a missing seed.  A second snapshot at
  the stock M0+'s IRQ8 entry confirms that the largest pre-handler differences
  are FRC runtime/framing state, AGC/SYNTH/RAC calibration, and PA-related radio
  state.  This replaces the earlier unmatched 500-ms startup comparison, which
  observed stage 3 before its first advertising event had configured the PHY.
- Tracing the stock sequencer from reset entry `0x2ade` to its first WFI at
  `0x2bb6` covered 2,123 instructions and 86 MMIO stores.  It did not write
  `FRC.FCD0..3`; neither do the IRQ8/IRQ9 handlers.  Those descriptor changes
  therefore belong to the earlier BLE frame-configuration path.  The stock
  path clears the public `SKIPWHITE` field on all four descriptors, whereas
  stage 3 had retained the protocol-neutral PHY baseline's asserted field.
  `radio_pkt_configure()` now clears that named field after the LLL establishes
  a BLE framing contract.  A live readback confirmed
  `FCD0..3 = 0x0cff, 0x0cff, 0x0dff, 0x0dff`, matching stock in that field.
  QEMU controller smoke and all 72 machine qtests pass.  A subsequent
  20-second LE scan still did not observe the advertiser, so whitening was a
  real framing defect but not the sole on-air blocker; PA setup and the
  remaining FRC/sequencer prerequisites stay open.
- Loading the complete known-good stage-2 M0+ image under the updated stage-3
  M33 driver also produced no advertisement in a 20-second LE scan.  This is a
  strong isolation result: the remaining prerequisite is in M33-side
  FRC/BUFC/PHY setup rather than only in the open handler replay.  The public
  table's omitted `SEQ.PHYINFO` pointer was checked next, but both secure and
  nonsecure `+0x4ffc` views are zero while this stage-2 image advertises, so it
  is not the missing handoff for this controller build.
- A BUFC snapshot at matched boundaries shows TX channel 0 uses the same
  control mode and prefetch command in both images.  Stock additionally has a
  `0x0fff` channel threshold; applying that named field alone with the known
  sequencer did not restore RF output.  Applying the remaining safe writable
  FRC differences (`WCNTCMP0`, `FECCTRL`, and `RXCTRL`) as a temporary
  characterization set was likewise negative.  No packet/status words,
  interrupt masks, channel seed, or sniff settings were transplanted.  These
  negative tests keep raw snapshot values out of the production source and
  narrow the next investigation to the actual FRC-to-BUFC transfer contract
  and its event ordering.
- A clean stage-3 snapshot after repeated `RAC.CMD.TXEN` attempts puts the
  missing boundary before modulation or PA activity: BUFC channel 0 still has
  `READOFFSET=0` and `WRITEOFFSET=11`, while FRC `DATABUFFER`, `WCNT`, and
  packet-buffer status/count remain zero.  The staged PDU is therefore never
  consumed by FRC.  Static analysis of the exact working stage-2 ELF found no
  M33 store to `FRC.DATABUFFER`; every directly addressed `FRC.CMD` store
  writes `0x201`, which the public SVD names `RXABORT | CONVINIT`.  The three
  sites are at PCs `0xe294`, `0xef14`, and `0x10f7c` (the first two are the
  same RAIL helper emitted from `generic_phy.c` and `generic_phy_shared.c`).
  This is now the next named command to test at the transfer boundary; it is
  evidence for command ordering, not yet evidence that either bit alone owns
  the TX FIFO handoff.
- Hardware-breakpoint tracing of the working stage-2 M33 from the common TX
  preparation helper at `0x10598` to the scheduled-transmit return at
  `0x18dee` covered 1,123 instructions and 14 MMIO stores.  The channel-37
  path writes `SYNTH.FREQ=0x01f46aab`, then `SYNTH.CHCTRL=0` and `CHSP=0`;
  it also updates the named `MODEM.SRCCHF`, `MODEM.DIGMIXCTRL`,
  `RAC.DIGCLKRETIMECTRL`, and `RAC.XORETIMECTRL` registers.  In contrast, a
  clean stage-3 run left `SYNTH.FREQ=0` and both RAC retiming controls zero.
  This is a real simulator/HAL mismatch: QEMU currently treats logical
  `CHCTRL.CHNO` as sufficient channel tuning, while silicon's working path
  explicitly programs the carrier-frequency word.
- Two debugger-only characterization runs did not restore an advertisement.
  The first injected only the observed channel-37 `SYNTH.FREQ` value.  The
  second additionally matched the stock `DIGMIXCTRL` low-frequency word and
  the two named RAC retiming controls (`0x430` and `0x770`).  Twenty-second LE
  scans in both cases found neither the stage-3 identity nor its advertised
  name.  These values therefore remain trace evidence rather than source-code
  configuration; the unconsumed BUFC/FRC state still places the blocker at
  the launch/data-transfer boundary.  Production code must derive these
  fields from named PHY/channel parameters rather than embedding the observed
  words.
- Static disassembly of the stock immediate-TX primitive found a separate
  pre-`TXEN` handshake at `0xe35c`: it sets `RAC.SR0` bit 7, clears `SR0` bit
  3, and clears bit 5 of sequencer shared word `0xb0003ec4`.  A debugger-only
  replay established the requested state (`SR0=0x80`, shared word zero), but a
  further 20-second LE scan was negative.  BUFC still reported write offset
  11 and read offset zero, and the FRC data/status words remained zero.  This
  handshake is therefore another necessary stock-path observation, not a
  sufficient transfer trigger or an acceptable set of unexplained production
  constants.
- A DWT watchpoint on stock channel-0 `BUFC.WRITEDATA` identified the actual
  per-packet producer path. The working controller issues one 32-bit store
  containing each octet to the data port; immediately after the first store,
  both the hardware write and read offsets were one. Stage 3 had instead
  copied the PDU to backing RAM, assigned `WRITEOFFSET`, and issued PREFETCH.
  `radio_pkt_tx_set()` now owns a copy, and the existing scheduler-owned TX job
  publishes it through `WRITEDATA` immediately before `TXEN`; no radio work was
  moved into an ISR. QEMU already models the data port. Controller smoke and
  all 72 BG22 qtests pass with this path. Hardware advances the write offset to
  11, but still leaves the read offset at zero and emits no advertisement.
- Matched snapshots immediately before the first `WRITEDATA` store sharpen the
  next prerequisite: stock has `RAC.STATUS.STATE=OFF`, while stage 3 has
  `STATE=SHUTDOWN`. Writing the public stock control fields `CPUWAITDIS`,
  `ACTIVEPOL`, `PAENPOL`, and `LNAENPOL` changed the sequencer-running flag but
  did not leave SHUTDOWN. Repeating the test with the captured stock M0+ image
  and the observed ordered SEQRESET/FORCEDISABLE/release/clear sequence also
  remained in SHUTDOWN and did not consume the FIFO. That only showed the
  sequence was insufficient in the tested state. Static analysis later proved
  the polarity setup and CPU-wait release are unconditional initializer steps,
  so the HAL now retains them with named fields and ordering; the reset prefix
  remains conditional on RAC not already being OFF.
- A matched capture at the earlier `RAC.EN` boundary found an enable-ordering
  divergence hidden by QEMU. Working stage 2 has both `BUFC.EN=1` and
  `PROTIMER.EN=1` before it enables RAC; stage 3 enabled RAC first and only
  enabled those dependencies afterwards. The open HAL now uses the complete
  observed stock order (AGC, BUFC, MODEM, RFCRC, FRC, PROTIMER, RAC, SYNTH).
  QEMU now latches RAC in SHUTDOWN when RAC is enabled before either
  dependency, and
  `/arm/efr32bg22/rac-enable-requires-dependency-order` proves that enabling
  them late does not erase the failure. All 73 BG22 qtests pass. Reordered
  stage-3 hardware still produced no observable advertisement, so this was a
  real simulator-fidelity defect but is not the remaining sequencer-release
  prerequisite by itself.
- A dual-core hardware breakpoint at the working stage-2 M0+ external-IRQ8
  entry (`PC=0x2578`) established the PA state at the actual TXWARM boundary:
  `PAENCTRL=0x10000000`, `PATRIM1=0x00038997`, `PATRIM3=0x00322d2a`,
  `PACTRL=0x10`, `RX=0x10`, and `TX=0`. This disproved the older
  characterization probe's state-dependent PA presets. `RAIL_ConfigTxPower`
  changed no RAC register in a before/after capture, and the first observed
  `RAIL_SetTxPowerDbm` call entered with requested power zero and likewise
  made no immediate RAC change. PA selection/calibration is therefore carried
  through later RAIL/sequencer state rather than one M33 register write. The
  corrected values remain confined to probe fixtures, not the production HAL.
  The first corrected replay attempt selected AP3 rather than the halted M33
  for the image load, so it did not reproduce the earlier functional handoff;
  its negative scan is not evidence about the PA hypothesis.
- The earlier negative "known-good sequencer" replay used a stale/different
  runtime dump: its reset vector is `0x2adf`, while the exact stage-2 image at
  the successful pre-release boundary has reset vector `0x2a2d` and differs
  throughout shared FRCRAM. Capturing that exact 20 KiB image and replaying
  the ordered M33 initialization/release under stage 3 changes RAC from
  SHUTDOWN to OFF (`STATUS=0x10000000`). This proves the M33 release protocol
  and image-state coupling. Resuming stage 3 still returns RAC to SHUTDOWN on
  its TX attempts, leaves BUFC at write 11/read zero, and produces no scan
  result, so it does not yet satisfy the open on-air gate. The next lockstep
  boundary is the exact `0x2a2c` M0+ reset path, not the stale `0x2ade` image.
- Zephyr's platform setup attributes `AHBRADIO` to the non-secure peripheral
  window, so the M33 HAL correctly uses the B/BA aliases.  That does not imply
  that the private radio M0+ can use those aliases.  An otherwise identical
  open replay image whose first store targeted `0xB8022028` immediately took
  ARMv6-M HardFault 3 at PC `0x106`; its stacked instruction is the first
  `RAC.SEQIF_CLR` store.  Restoring the M0+ image's A/AA accesses while loading
  it through the M33-visible B sequencer-RAM window succeeds: the marker
  reaches an external IRQ, RAC leaves SHUTDOWN, and channel-0 `READOFFSET`
  advances from 0 to the full staged length 11.  Thus the two cores require
  different address views of shared radio state.  QEMU previously aliased
  these views without modeling this initiator-dependent access rule; the
  simulator fix and regression tests described below now enforce it.
- That successful open-M0 replay still produced no identifiable stage-3
  advertisement in a 20-second LE scan.  FIFO consumption is therefore a
  newly confirmed hardware boundary, not yet the stage-3 on-air gate.  The M0+
  fixture only performs bounded peripheral actions and acknowledges its
  interrupt; it does not choose, dispatch, reprioritize, or preempt M33 work.
  All launch and completion policy remains in persistent Zephyr jobs.
- Watchpoints on the working stage-2 channel preparation establish all three
  advertising values: channel 37/2402 MHz writes `SYNTH.FREQ=0x01f46aab`,
  channel 38/2426 MHz writes `0x01f96aab`, and channel 39/2480 MHz writes
  `0x0204aaab`.  The HAL now selects those named values in its scheduled M33
  preparation job; data-channel encoding remains disabled rather than guessed.
  With the open M0 reset/IRQ replay, silicon subsequently consumed all 11
  bytes and traversed TX to RXSEARCH, but a 20-second scan did not receive the
  exact staged AdvA.  A diagnostic match of the remaining differing public
  FRC/MODEM/RAC/PROTIMER fields was also negative, so those captured values
  were not copied into production source.
- A complete hardware single-step trace from the working `RAIL_StartTx` entry
  to its `RAC.CMD.TXEN` instruction completed in 1,152 instructions and
  exposed a channel handoff that the earlier direct-address static analysis
  missed.  For advertising channel 38, RAIL writes physical 2 MHz index 12 to
  both `FRC.WCNTCMP2` (`+0x28`) and `SYNTH.CHSP` (`+0x44`), while leaving
  `SYNTH.CHCTRL` (`+0x40`) zero.  The open HAL had instead put the index in
  `CHCTRL` and omitted the other two writes.  The HAL now follows the observed
  three-register contract.  QEMU's modeled RF medium now snapshots `CHSP`,
  and its dual-radio regression deliberately gives the peers different
  `CHCTRL` values while proving that mismatched then matched `CHSP` values
  reject then deliver the packet.  All 73 machine qtests and all 17 RF-medium
  qtests pass.  The corrected stage-3 image still did not expose its exact
  AdvA in a 30-second LE scan, so this was a real simulator/HAL divergence but
  not the remaining on-air defect.
- QEMU now models the alias rule found on hardware.  At reset the M33 secure
  A/AA radio window is active and B/BA is RAZ/WI; clearing
  `SMU.PPUSATD1.AHBRADIO` switches the M33 to B/BA and makes A/AA RAZ/WI.
  The private M0 retains its independent A/AA route.  The focused alias tests,
  including state retention across attribution changes, pass as part of all
  73 BG22 qtests.
- Per-IRQ counters in the open M0+ fixture showed that one stage-3 launch
  reaches external IRQ0/1/2/4/8/9/11/13/14.  Breakpoint-aligned stock-handler
  traces now cover IRQ0 (11 MMIO stores), IRQ1 (112 stores), IRQ4 (36 stores),
  natural IRQ11 (129 stores), natural IRQ13 (47 stores), and natural IRQ14
  (one zero-valued `RAC.SEQIF_CLR` store), in addition to IRQ2/8/9.  IRQ1 and
  IRQ4 were manually pended and are characterization only.  Blindly replaying
  IRQ0/1/4 changed the interrupt sequence and reduced FIFO consumption from
  11 bytes to 2; blindly replaying natural IRQ11 caused repeated IRQ11 and
  left RAC off.  Natural IRQ13 retained full FIFO consumption but still did
  not produce its exact advertisement in a 20-second scan.  These negatives
  prove that handler stores are state-dependent and must become named branch
  logic, not unconditional magic-number tables.  None of these M0+ handlers
  selects or dispatches an M33 job; Zephyr remains the sole scheduler.
- Stock IRQ11 polls a live counter at `0xa801c020` until it changes.  Hardware
  debug single-step freezes that counter, so a naive trace loops forever at
  `0x1df8`.  `trace_stores.py --run-through 0x1df8=0x1e02` now resumes only
  across that read-only poll and re-halts immediately afterward.  This is a
  debugger/hardware observation aid, not a firmware timing shortcut.
- A stronger isolation test ran the complete captured stock M0+ image under
  the stage-3 Zephyr/custom-driver M33 firmware, with only the four previously
  proven inaccessible RF-mailbox stores NOPed.  It consumed all 11 staged PDU
  bytes and returned RAC to idle, but a 20-second scan did not see the exact
  staged address.  Therefore missing open-M0 handler behavior is not the only
  blocker: the M33-side PHY/FRC/MODEM/BUFC configuration still diverges.
- Matched M33 watchpoints now stop working stage 2 and stage 3 at the exact
  `RAC.CMD.TXEN` write.  The resulting snapshots expose concrete configuration
  differences, including FRC `WCNTCMP0`, `RXCTRL`, `SNIFFCTRL`, multiple MODEM
  BLE-demodulator fields, and SYNTH channel/calibration fields.  A temporary
  stage-2 PA preset (`PAENCTRL`, `PATRIM1`, `PATRIM3`, `PACTRL`) is now applied
  by the open HAL using provenance-named constants; this enabled the PA path
  but did not by itself make the packet decodable.  The dBm/calibration mapping
  and remaining breakpoint-aligned configuration differences remain the next
  source-backed reconstruction boundary.
- Matched private-core IRQ8/IRQ9 stops narrow several apparent FRC differences
  to effects rather than missing setup. `OTACNT +0x070` is read-only; its
  working-image TX count is progress made by that transmission, not a value
  firmware programmed. `PKTBUFSTATUS +0x0d0` and `PKTBUF0..3 +0x0d4..0x0e0`
  are likewise read-only: the working values are the count and first sixteen
  bytes already present in the FRC packet pipeline, while their stage-3 zeros
  are consequences of the earlier abort. None is promoted to a write.
  `SNIFFCTRL +0x084` is real configuration, but the working `0x000200f8`
  decodes to the named TX-data, RX-data, RSSI, state, auxiliary-data, and
  sync-word observation taps. The proprietary initializer sets it to expose
  those signals to the FRC sniff path; no observed consumer or public field
  contract makes that observation path a TX prerequisite. It therefore
  remains unpromoted rather than becoming a magic `0x200f8` write. If later
  evidence establishes a required consumer, the driver will select the
  individual named taps and QEMU will model that consumer.

## Necessary is not the same as sufficient

An isolated replay that does not produce an observable advertisement proves
only that the replayed operation is not sufficient.  It is not grounds for
removing an operation that static control flow places on the working path.
Bring-up now classifies each difference by both provenance and purpose:

| Operation | Why the proprietary firmware performs it | Driver disposition |
| --- | --- | --- |
| `FRC.CMD = RXABORT | CONVINIT` | Quiesce any stale receive operation and initialize the FRC convolution state before using the configured PHY. | Retained as a named initialization command. Its negative isolated test does not make it optional. |
| `RAC.CTRL.ACTIVEPOL`, `PAENPOL`, and `LNAENPOL` | Select the active-high electrical polarity used by the board's RAC activity and external PA/LNA control signals. | Retained as three named fields; the former opaque `0x380` preset is gone from the C source. |
| `RAC.CTRL.CPUWAITDIS` | Release the private radio M0+ only after its sequencer image and shared data are resident. | Retained and ordered after block enable/image installation. It is a release gate, not a TX trigger. |
| Per-launch `SR0` set/clear and shared-word bit clear | Satisfy the M33-to-sequencer launch ABI immediately before `TXEN`. Static M0+ analysis now shows that shared bit 5 is an RX-after-TX inhibit: the helper at image offset `0x9c8` pulses its SR0 gate, tests that bit, and writes `RAC.RXENSRCEN.SWRXEN` only when it is clear. | Retained because the exact working call path proves the sequence. Shared bit 5 now has a behavior-derived name. The public meaning of M33's SR0 bits 7 and 3 is still unresolved, so those names describe the launch phase rather than inventing semantics; their M0+ branches remain an active RE item. |
| Channel-0 `BUFC.WRITEDATA` stores | Publish each staged PDU octet into the hardware FIFO at the scheduler-owned launch boundary. | Retained. A watchpoint and the disassembled `RAIL_WriteTxFifo` path agree on both operation and ordering. |
| `BUFC.CMD.PREFETCH` | Prime a FIFO when `RAIL_SetTxFifo` installs a buffer with a nonzero initial byte count. | Not used by the per-packet path because the working controller calls `RAIL_WriteTxFifo`, whose byte-port loop does not issue it. This exclusion follows call-path semantics, not the negative debugger replay. |
| `RAC.CTRL.FORCEDISABLE` reset prefix | Quiesce an active RAC before sequencer replacement or radio reset. | Retained conditionally when RAC is not already OFF. Applying it to an already-OFF, incompletely released stage-3 RAC drove silicon into SHUTDOWN, so unconditional use would change the operation's purpose. |
| PA trim/control snapshot | Establish the board/part analog PA bias and trim at the observed TX-warm boundary. | Temporarily retained as one provenance-named board preset. It is not a dBm mapping; the final implementation must derive it from DEVINFO/board calibration and named fields. |
| Remaining FRC/MODEM/SYNTH/RAC/PROTIMER snapshot differences | Collectively configure framing, BLE modulation/demodulation, synthesizer tuning, analog retiming/calibration, and scheduled event timing. | A negative bulk replay does not disprove any individual setting. Snapshot-only values are not copied blindly: each will be retained once its writer, branch conditions, phase, and field purpose are recovered. Public generated PHY-table writes are represented as typed table application rather than scattered register literals. |

This distinction also applies to simulator work: QEMU must model the reason an
operation exists and its ordering/observable effects.  It must not merely
accept a captured value so that one trace happens to pass.

## Stage-3 coprocessor provenance boundary (2026-10-27)

Stage 2.5 is a deliberately non-shipping diagnostic exception to the boundary
below. It combines the vendored Zephyr/open M33 controller and driver with an
explicit external proprietary coprocessor capture. The blob remains under the
persistent `Projects/owo-bg22-proprietary-baselines` evidence directory and is
selected only through the non-default CMake cache option
`EFR32_RADIO_PROPRIETARY_SEQ_IMAGE`; it is never copied into this repository.
The resulting build must be labelled stage 2.5 in paths and reports and can
only distinguish M33-side configuration from coprocessor behavior. It cannot
be promoted, shipped, or counted as stage 3.

The first attempted stage-2.5 run used the 20,480-byte runtime capture
`stage2-seq-frcram-2a2c.bin` (SHA-256
`096de38f7f0262c4af9e2caeeaf78cf71f20c2f2c88c9cad34c8dc89e3e52c2f`),
whose vector table actually has SP `0x3e30` and Thumb reset vector `0x2adf`,
not the previously reported `0x2a2d`. It also contains mutable FRCRAM state,
so this run is retained as a negative experiment but is not a clean test of
the proprietary coprocessor firmware. The build
completed from the vendored Zephyr tree and open M33 module at 177,800 flash
bytes and 32,664 RAM bytes. The generated image declares
`efr32_radio_seq_image_is_open = false`, and configure emits the expected
stage-2.5 warning. OpenOCD's SRAM preflight and fast erase/write/verify all
passed before starting the board.

A 25-second independent BlueZ LE scan did not observe the staged AdvA
`E9:CE:28:2D:07:20`. This repeats the earlier debugger-injection result in a
reproducible firmware build: the matching proprietary coprocessor state is not
sufficient with the current open M33 register/packet contract. It does not
show that the proprietary behavior is unnecessary. The identical stage-2.5
flash image under QEMU reports `bt_enable: 0`, `adv_start: 0`, and reaches
`RAC.CMD.TXEN`; hardware also reaches that broad state boundary in stage 3,
so this is not an on-air success. QEMU's existing eager TX snapshot still
omits the silicon FRC continuation/abort behavior and remains the simulator
divergence to fix once that contract is decoded.

The corrected isolation build extracts the immutable sequencer section
`.rodata.sli_rail_6c7715fcf8a536bb641f7a33742a842f` directly from the vendored
SDK's `generic_efr32xg22_release_seq.o`. The 12,140-byte image has SHA-256
`b775ea32a025dd829f4c86503d3d812f149ca9441884082ee533354b242d08d4`, SP
`0x3e30`, and Thumb reset vector `0x2a2d`; the containing object has SHA-256
`b57b5fa71557874355d1994fc565f05917722b9f5f9056d375aecde157097b64`.
It remains outside the repository under
`Projects/owo-bg22-proprietary-baselines/stage2.5-sdk-sequencer/`. The corrected
Zephyr image uses 169,460 flash bytes and 32,664 RAM bytes. OpenOCD again
passed SRAM preflight, erase, write, and verification. A 30-second BlueZ LE
scan did not observe `E9:CE:28:2D:07:20`. The identical corrected image in
QEMU reports `bt_enable: 0`, `adv_start: 0`, and `RAC TXEN observed`. Thus the
clean stage-2.5 result is negative on silicon while QEMU still declares the
launch successful too early; the remaining on-air blocker is not explained
solely by substituting the proprietary coprocessor code.

The private radio Cortex-M0+ firmware is part of the open driver, not a
precondition supplied by RAIL, a debugger, or retained SEQRAM. Its reviewed
source and linker script live in `modules/efr32-radio/sequencer/`. CMake links
that source independently for ARMv6-M, converts the result to a byte array, and
links that array into the M33 application. `radio_reset()` validates the
embedded initial SP and Thumb reset vector, asserts `RAC.CTRL.SEQRESET`, copies
the image to `0xb0000000`, reads every byte back, and only then performs the
CPU-wait release. The current image is 400 bytes with SHA-256
`21f65696a8009288554589d157cec10953dd7dec7a1db4d259b4fe533840b102`.
The stage-3 link map contains no RAIL or proprietary sequencer archive member.

This provenance boundary applies to the whole dependency chain, not merely the
M33 application. The private-core image is production driver source and must
remain reproducibly buildable from the flake's open toolchain. Captured vendor
images, disassembly, and register traces may be retained as labelled
differential evidence, but may never be linked, copied into SEQRAM, downloaded
at runtime, or translated byte-for-byte into an unexplained replacement.

This establishes image ownership, not radio completeness. The initial open
M0+ handles only bounded state-event acknowledgement and publishes identity
`OSQ1` at local `SEQ.SPARE0`; it performs no scheduling or follow-up radio
selection. The first integrated release incorrectly generalized `SEQRESET` to
the SET alias and left startup unproven. The corrected path writes the reset
request through base `RAC.CTRL`, copies and verifies the image, then replaces
base `CTRL` with `CPUWAITDIS`, the transition used by the successful standalone
silicon launcher. A reset-aligned M33 breakpoint subsequently stopped at
`radio_reset()+0x13a` (`0x15f22` in that build), which is reachable only after
the M33 reads `OSQ1`. This proves that the source-built M0+ reset handler ran on
real hardware. A later AP3 halt still reported an unknown debug state, so that
warning is an attachment-state limitation rather than evidence against image
execution. The expected advertiser remained absent in the simultaneous scan;
the next gap is functional coprocessor/peripheral behavior after reset.

The first such gap was concrete: the initial open reset handler executed
`cpsie i` but never enabled an external interrupt in the M0+ NVIC. Static
analysis of the working reset path confirms an `NVIC.ISER` write, while live
AP3 traces identify external IRQ8 and IRQ9 as TXWARM and TX. The open image now
enables exactly those two bounded state-service interrupts; it does not enable
the other observed vectors or turn either interrupt into scheduling policy.
At the reset-aligned first `RAC.CMD.TXEN`, hardware then reported
`RAC.STATUS=0x09400000` (public state 9, TX), `SEQIF=0x3`, and `SR0=0x80`,
instead of remaining in SHUTDOWN. This is a real simulator/hardware divergence:
QEMU's synthetic TX path had advanced directly to TX without requiring the
private core's NVIC service. The subsequent 25-second LE scan still did not see
the expected advertiser, so TX state is necessary but not evidence of a
correctly framed or modulated packet. The next reconstruction boundary is the
source-explained, read-dependent work performed by the proprietary IRQ8/IRQ9
handlers.

A reset-aligned measurement at the second advertising launch narrows that
boundary further. `BUFC.WRITE=11` and `BUFC.READ=2`: all 11 advertisement bytes
were staged, but silicon consumed only the two-byte BLE header before returning
RAC to OFF. `FRC.IF=0x40020c04` did not contain the known TXDONE bit. The
matched stage-2 and stage-3 FRC snapshots already agree on dynamic-length mode,
an offset of one byte, an eight-bit length field, minimum length one, maximum
length, and BLE frame descriptors. Therefore the next work is to identify the
source-explainable FRC continuation contract (including the read-dependent
IRQ8/IRQ9 path), not to copy an opaque handler or guess another preset. QEMU's
current whole-FIFO snapshot at TXEN is correspondingly too optimistic; it must
model header parsing, payload continuation, and completion/error behavior once
that contract is established, with malformed and incomplete-frame regressions.

The open M0+ now records source-named, read-only snapshots at IRQ8/TXWARM and
IRQ9/TX: `FRC.IF`, `FRC.SEQIF`, and channel-0 BUFC write/read offsets. The
first diagnostic revision incorrectly treated BUFC channel fields as global;
channel 0 begins at private-core `0xaa00000c`. Those two readings were
discarded, and the source now derives `WRITE=base+8` and `READ=base+12`. The
corrected hardware result is:

```
              FRC.IF     FRC.SEQIF  BUFC.WRITE  BUFC.READ
IRQ8/TXWARM   40020c04   40020c04   11          0
IRQ9/TX       40020c04   40020c04   11          2
```

The public rev-B2 model decodes `0x40020c04` as `TXABORTED`, `WCNTCMP1`,
`WCNTCMP2`, `WCNTCMP3`, and `TXWORD`. A matched AP3 breakpoint on working
stage 2 at the first IRQ8 instruction (`0x2578`) reads `FRC.SEQIF=0`.
Therefore stage 3 is already aborted before either open M0+ state handler can
influence the transaction; blindly replaying the vendor IRQ8 body cannot be
the primary fix. The fault boundary is M33-side FRC/BUFC framing or the
pre-IRQ coprocessor contract.

One difference was independently explainable and remains fixed. Static
working-image code writes `FRC.WCNTCMP0 = configured_capacity - 1`; its BLE
capture and public FCD `WORDS` fields use 255. Stage 3 now seeds that named
dynamic-length-pending sentinel instead of zero. Hardware subsequently
replaced it with 10 for the 11-byte frame, proving that the length byte was
parsed, but `TXABORTED` and the two-byte stop remained. The bootstrap is
correct but insufficient and is not represented as the whole fix.

The same discipline was applied to `FRC.FECCTRL.BLOCKWHITEMODE`. Static code
at working-image `0x2c1e..0x2c24` conditionally clears the public three-bit
field and selects mode 1, and the matched working BLE snapshot contains that
value. This is the active-frame whitening selection corresponding to
`FCD.SKIPWHITE=0` and the BLE channel whitening seed, so stage 3 now names and
applies it without overwriting other FEC fields. Hardware confirmed
`FECCTRL=1`, but the IRQ8/IRQ9 trace and two-byte stop were unchanged. It is a
correctly explained prerequisite, not the abort fix.

QEMU's corresponding regression deliberately seeds stale SEQRAM vectors,
asserts `SEQRESET` through base `CTRL`, overwrites the vectors, replaces base
`CTRL` with `CPUWAITDIS`, and proves that only the replacement image executes.
All 73 machine qtests pass. Any simulator path that executes a retained image
without this overwrite is a hardware-model bug.
