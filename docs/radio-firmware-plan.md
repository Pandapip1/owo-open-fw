# Open radio firmware: inventory, plan, status

**Stage 3 runs on silicon: the open M33 driver and the open sequencer
advertise, scan, answer at T_IFS and hold encrypted, LE Secure Connections
paired connections on a BG22 kit (`docs/radio-tables.md` §7.62, §7.64, §7.71,
§7.80, §7.84). The product image uses it (`boards/owo_product.dtsi`); it has
not run on a suit.**

## Stage-3 dependency rule

The radio coprocessor firmware is part of `modules/efr32-radio` and is built
from reviewed ARMv6-M source by the open GNU Arm Embedded toolchain.  Stage 3
must not depend on a RAIL archive, a downloaded sequencer object, or an image
left in retention RAM.  Proprietary binaries are permitted only as stage-1/2
reference oracles and differential-test inputs; observations promoted into the
open implementation require named behavior, provenance, and simulator
coverage.  A byte-for-byte unexplained reimplementation is not an acceptable
open driver.

The generated `radio-sequencer.bin` is an intermediate build product, not an
input blob: the build graph derives it only from `sequencer/radio_seq.S` and
`sequencer/link.ld` (since §7.97 from `radio_seq.c`, `radio_seq_core.S` and `seq_regs.h`; the
all-assembly `radio_seq.S` with `CONFIG_EFR32_RADIO_SEQ_ASM`), then converts it to the const array linked into the M33
image.  A clean stage-3 rebuild on 2026-10-27 produced the 400-byte image with
SHA-256 `21f65696a8009288554589d157cec10953dd7dec7a1db4d259b4fe533840b102`;
the product build for `bg22_ek4108a` at `918708d` (2026-11-05) produces a
4734-byte image (SHA-256
`3cdb9dcec6de910bb85d450678b79788097b4393e410c86ca1ddad4f31b4a6d0`).
The final link map contains `radio-sequencer-image.c.obj` and contains no RAIL,
`liblinklayer`, or vendor sequencer object.  The Silicon Labs HAL archive
members pulled into that image are source-built clock, GPIO, timer, USART,
power, system, DMA, and memory-manager objects; none is a radio library or
coprocessor image.  This map audit is necessary but not sufficient: every
future radio-side source addition must still have a named contract and the
hardware/simulator evidence described above.

Goal (user-confirmed): replace every Silicon Labs radio blob on the EFR32BG22
with open code: the M33 link layer and direct radio driver **and** the private
radio-coprocessor firmware are one source-reviewed driver deliverable. There
must be no unexplained binary anywhere in its build or runtime dependency
chain. This file records what was found, what is possible without undocumented
hardware knowledge, and the order to do it in. Facts are tagged **[F]**
(observed in a file or trace, source given), **[I]** (inferred), **[?]**
(unknown).

## 1. What runs where

```
Zephyr host (open: GAP/SMP/GATT/L2CAP)
   | HCI (drivers/bluetooth/hci/hci_silabs_efr32.c)            [F] zephyr drivers/bluetooth/hci/hci_silabs_efr32.c
Silabs link layer  liblinklayer.a  (M33, closed)               [F] blobs/.../bgstack/ll/build/gcc/xg22/release/liblinklayer.a
   | RAIL API (public headers: rail.h, sl_rail.h, rail_ble.h)    [F] platform/radio/rail_lib/common/*.h
RAIL  librail_*_efr32xg22_*.a  (M33, closed)                   [F] blobs/.../rail_lib/autogen/librail_release/
   | memory-mapped radio blocks, 0xA8000000.. (undocumented)
Radio hardware  +  RAC sequencer (code in SEQRAM 0xA0000000)   [F] qemu trace, see §2
```

* **[F]** `liblinklayer.a` (xg22) is 169 objects, about 265 kB. It contains the whole LL (`ll_*`, `sli_ll_*`, HCI parsers, `ll_crypto`, DRBG). Its undefined `sl_rail_*` symbols show that it reaches the radio only through RAIL: `sl_rail_start_tx/rx`, `sl_rail_set_rx_transitions`, `sl_rail_set_tx_transitions`, `sl_rail_set_state_timing`, `sl_rail_start_scheduled_tx/rx`, `sl_rail_set_tx_fifo`, `sl_rail_write_tx_fifo`, `sl_rail_peek_rx_packet`, `sl_rail_config_address_filter`, `sl_rail_ble_config_phy_1_mbps`, multi-timers, `sl_rail_get_time`, … (full list: `nm -u` of the archive).
* **[F]** RAIL for xg22 (`librail_efr32xg22_gcc_release.a`) is about 75 kB of code in `rail_*`, `rfhal_*` and `generic_phy*` objects; `rfhal_{bufc,protimer,synth,rac,mailbox,radio,rfsense,pa,...}.o` are the M33-side register drivers. The sequencer image is a separate object, `generic_efr32xg22_release_seq.o` (14.5 kB, data only).
* **[F]** Radio configuration for BLE is *public source* (Zlib licence): `platform/radio/rail_lib/plugin/sl_rail_util_built_in_phys/efr32xg22/sl_rail_ble_config.c` (1 Mbit/s "viterbi", 2 Mbit/s, coded PHY). Format: `[count<<16 | 16-bit offset from 0xA8000000][count values]` runs (e.g. `0x0001400C, 0x00018101`), with the SET/CLR alias selected by the top nibble of the header (**[I]** from the series-2 alias convention, which the QEMU server also uses). This is the only complete register/value list for the PHY that is not extracted from a binary.
* **[F]** Zephyr's EFR32 HCI driver is the only seam to the controller: `sl_btctrl_init()`, `sl_btctrl_process_events()`, `sl_btctrl_hci_transmit_complete()`, `sl_btctrl_raise_events()` (`hci_silabs_efr32.c`). An open controller replaces that file and the two Silabs libraries behind it. **Done:** the product disables `bt_hci_silabs` and selects Zephyr's `zephyr,bt-hci-ll-sw-split` controller with the `modules/efr32-radio` lower layer (`boards/owo_product.dtsi`, `docs/zephyr-ll-port.md`).

## 2. The sequencer

* **[F]** A historical QEMU boot of the stock 7.9.07 image (`flash-stock.bin`) through the retired out-of-process bridge wrote 16 kB to SEQRAM at 0xA0000000 (non-secure alias 0xB0000000) and a smaller block (about 1 kB used) to FRCRAM at 0xA0004000. A memory dump after boot shows blocks 0..3 (0xA0000000–0xA0003FFF) full and 4 sparsely used. (Method: QMP `pmemsave`; the dump is kept out of the repository.) The native-C model now clears the clock/EMU/MSC reset latches, executes normal stock service code, and reaches initial radio MMIO: QMP samples reached `flash-stock.bin` file offsets `0x3b686` (SysTick handler) and `0x44214` (application service), with changing PC values in one run. A 20-second native-QEMU trace of the stock image (event `efr32bg22_radio_write` in `vendor/qemu/hw/arm/trace-events`; capture deliberately discarded after inspection) recorded 19 writes and their executing firmware PCs. The eight value-`1` writes at `+0x004` are at `flash-stock.bin` offsets `0x26de2..0x26e08`: FRC, AGC, RFCRC, MODEM, RAC, and SYNTH pages, plus the observed BUFC and PROTIMER pages. This is independently direct binary evidence: the helper at `0x26dc0` materializes `1` at `0x26de0`, and its target literal pool is at `0x27238..0x27254`. They are register accesses, not a claim about all eight blocks' enable semantics. The later writes are `0xA8006084 = 0x0000ffff` at `0x2a6cc`, `0xA8005084 = 0x000017f8` at `0x2a6d0`, and RAC alias address `0xA802204c = 0x40000000` at `0x2a6fe`; their target literals are at `0x2a740..0x2a750`. The `0x17f8` value is retained directly in the historical trace at `evidence/stock/radio-trace.log:83-84`; no behavior is inferred from it. The public xG22 rev-B1 register model identifies the latter as the CLR window of full-width `RAC.SR3` (`host_py_rm_studio_internal/full/efr32xg22/revB1/RAC_S_register.py:505-513`). The bounded capture did not write `RAC.CMD` at `0xA8020010`. This validates that native execution has reached early radio setup, but not sequencer loading, a RAC command, or an end-to-end radio transaction. The old bridge trace remains historical register evidence rather than current native-model end-to-end validation.
* **[F]** Native startup was revalidated after restoring the standard ARMv7M board-reset registration. `flash-stock.bin` SHA-256 is `76c73b7c786305ba459ff3642f77cbffbf4f9edec5bc8b07acb09974b5bdc729`; its little-endian words at file offsets `0x0` and `0x4` are `0x20008000` and `0x000018b9`. A paused native run consequently reports SP `0x20008000` and Thumb PC `0x000018b8`, rather than executing the zero-filled reset state. A bounded 20-second run then reproduced the eight enable writes at PCs `0x26de2..0x26e08` and the later writes at `0x2a6cc`, `0x2a6d0`, and `0x2a6fe`, without reaching `RAC.CMD`. Its raw write at `0x2a6d0` was `0xA8005084 = 0x000000f8`, which differs from the retained historical trace's `0x000017f8`; the trace logs the guest's input value before the model applies its register mask. Both values are retained as observations only, not as a semantic claim about that register. The reset fix is QEMU integration, not radio behavior: upstream `hw/arm/armv7m.c:627-631` requires each board to register a reset callback, and the native board now does so after copying its flash image.
* **[F]** A separate bounded native-control-flow pass produced the same 19 writes and showed why the later historical RAIL sequence cannot yet be attributed to a missing early radio register: the extended initializer starts at `flash-stock.bin` offset `0x27f50`, runs the `0x26cc8` setup helper, and is called from `0x1e4f4`; that call is reached through `0x26738`/`0x26728` from the wrapper at `0x1dfc2..0x1dff0`. The wrapper itself has no MMIO access. Native execution reaches the separate `0x2a63c` configuration path (its writes include the three PCs above), but not that wrapper chain, and QMP samples during the bounded run remained in application dispatch at `0x44212/0x4422e`, not a peripheral poll. Therefore PROTIMER setup, which happens inside the skipped initializer, cannot be its initial gate. The missing stimulus is an upstream M33 control-path event or state; no RAC, PROTIMER, or IRQ effect is synthesized from this negative result.
* **[F]** Static disassembly refines the final sentence above: there is no direct external-transport activation of that initializer. The wrapper at `0x1dfc2` has one direct boot-registration call, at `flash-stock.bin` offset `0x302f4`; its chain is `0x302bc → 0x1dfc2 → 0x26728 → 0x1e4aa → 0x27f50`. The boot setup entry at `0x47b80` reaches this registration chain through `0x1da20`, `0x1da76`, `0x23ccc`, and `0x2392e`. At `0x1dfc2`, the wrapper stores its object through literal `0x1e1e0 = 0x200032e4` and invokes the next two helpers with that object and `r1 = 0`; it contains no MMIO. Thus an invented UART/HCI stimulus would be misleading; the remaining question is the earlier boot-path condition that reaches that wrapper.
* **[F]** A concrete branch in the static path is not radio MMIO, but it is **not** the registration-chain gate. The call at `flash-stock.bin+0x1da2e` reaches the helper at `+0x201e8`, which loads literal `0x0fe0801c` from `+0x20200`, masks bit 12, and returns its Boolean result. At `+0x1da30`, a zero result falls through to the `0x404` return at `+0x1da36`, skipping SRAM BTSMART callback/control setup in `+0x1da3c..+0x1da70`. Public xG22 headers identify the address as `DEVINFO.SWCAPA0`: software-restriction field `BTSMART[1:0]` is bits 12..13 and has documented reset `0`; QEMU maps it at backing offset `+0x801c` and initializes that default. Crucially, the sole caller at `+0x47b86` ignores this return, then unconditionally reaches `+0x1da72 → +0x2031c → +0x1da76`; only independent checks inside later helpers decide whether `+0x23ccc → +0x2392e → +0x302bc → +0x1dfc2` occurs. Thus the value must not be synthesized as a radio-activation fix; it does not establish a production device's factory-programmed capability value either.
* **[F]** The first guard after `+0x1da76`, at `+0x1da82`, tests SRAM `0x20002bd8`; the caller has already stored the ROM configuration-object literal `0x000166e0` there at `+0x1da2e`, so it is not a QEMU peripheral dependency. The first potentially failing checked call is `+0x1dab0 → +0x2033c` (result tested at `+0x1dab4`). Its indirect target is a flash-resident allocator adapter: at `+0x1da8e` the code reads byte `0x20003bc7`, then loads literal `0x000202b3` from `+0x1dd78`; `+0x1da9a → +0x203d6` stores that Thumb entry at `0x20002d9c` (literal `+0x20460`) along with the byte input and size. The indirect call in `+0x2033c` consequently reaches `flash-stock.bin+0x202b2`, which rounds its allocation size and calls further flash helpers. A bounded eight-second native-QEMU probe saved only `0x20002d98..+0x0f` and `0x20003bc0..+0x3f`, then discarded the capture: it observed the expected `0x000202b3` pointer and zero configuration byte. This does not establish the allocator's internal state or a failure, but it rules out a missing executable SRAM trampoline. The now-observed failure is nevertheless later in the same software/control path, not a guessed register reset value.

* **[F]** A bounded native `-d exec,nochain` trace corrects the earlier reachability uncertainty: stock execution enters `0x47b82 → 0x1da20 → 0x1da72 → 0x1da76`, completes the allocator-adapter call at `+0x2033c` (returning through `+0x1dab4/+0x1dab8`), and calls `+0x22674` from `+0x1dacc`. It next calls `+0x20058` at `+0x1dadc`; that mapper returns nonzero at `+0x1dae0`, so control branches to cleanup `+0x1da86 → +0x1dc5e`, never reaching `+0x1dae4`, `+0x23ccc`, `+0x2392e`, `+0x302bc`, or `+0x1dfc2` in that boot. Static disassembly shows `+0x20058` maps an incoming error (zero stays zero), while `+0x22674` has no direct MMIO before the return and dispatches through configuration/callback data. Native GDB identifies its runtime object/configuration inputs as `0x200010c0`/`0x20000e18`; the first descriptor callback is flash `+0x47a82`, and it completes successfully (the expected object fields are initialized). The later scheduler helper at `+0x21cfe`, reached through `+0x22630`, clears object `+0x20`, tests its `+0x1c` entries through indirect predicates, and finds zero qualifying entries. Its zero result selects the literal error `0xf000e003` at `+0x22616/+0x22bbc`; `+0x20058` maps that to `0x483`. The demonstrated gate is therefore scheduler/configuration-table state after the allocation/control path, not an unmodelled RAC, PROTIMER, HFXO, or IRQ event. No hardware state is synthesized from it.
* **[F]** That scheduler input is now concretely identified as an erased persistent-flash table, not merely unspecified software state. Its runtime base is `0x0004c000`, it has 40 entries at `0x400` bytes each, so its documented 40-record extent ends at `0x55fff` (last entry starts at `0x55c00`); `flash-stock.bin` is in fact `0xffffffff` from `0x4c000` through EOF at `0x7ffff`. The loop at `+0x21cfe..+0x21d32` computes each address through `+0x21142..+0x2114a`, copies five words through the plain descriptor function at `+0x47aa6..+0x47ad6`, then classifies its first word at `+0x2e3c2..+0x2e410`. The predicate accepts only class 0/1; the classifier expects high 16 bits `0xb29a` and low 16 bits `1`, so erased entries all fail and deterministically produce `0xf000e003`. For a matching header it validates words `+4` and `+8` with a 27-bit check against each word's top five bits, then classifies word `+12` only by whether each half is `0xffff`; those fields have no established semantic names. No peripheral read occurs along this lookup/copy/classify path. This establishes missing persistent provisioning/configuration data (potentially factory or application NVM) as the native-stock startup divergence; table contents must not be invented or written without provenance.
* **[F]** No supplied local artifact provides that provenance. `flash-stock.bin` (SHA-256 `76c73b7c786305ba459ff3642f77cbffbf4f9edec5bc8b07acb09974b5bdc729`) has the erased span above; `stock-7.9.07.bin` (SHA-256 `87b8be3d3211fe563e4bdf9913ee80d2773b63a669350cd014ae8a71fb99472e`) is only 212,499 bytes and so cannot contain the runtime table. Its sole aligned little-endian `0xb29a0001` occurrence is at file offset `0x18300`, in executable code rather than a `0x400`-aligned table record. `btl.bin` (SHA-256 `1c06a88bf0decf16a6bf94f0254702abf2dff4f0c317f6e3fa66981953878a53`) has no candidate. `/Users/gavin/Downloads/stock-images.tgz` (SHA-256 `36dbf853ab622917ffba665d084a0770922bfa628a1dc1f40155ef3af6123597`) contains exactly those three files. The simulator must therefore not seed or model this table as known provisioned data.
  QEMU instead accepts an explicit, optional raw overlay: `-M efr32bg22,flash=...,flash-overlay=...,flash-overlay-offset=0x4c000`. It overlays only user-supplied bytes at the stated physical address after loading `flash`; omitting the property leaves the underlying erased image unchanged. This supports validation with a legitimately captured provisioning image without merging blobs or asserting any table contents.
* **[F]** The separately available Simplicity SDK does not add that provenance. Its xG22 RAIL archive (`librail_efr32xg22_gcc_release.a`, SHA-256 `c94fa52d538db6faa9150a327edff04f00980947897e7c7d09d12686190477fb`) contains M33-side RAIL/radio objects but no little-endian `0xb29a0001` occurrence; the scan covered 228 SDK binary/archive candidates. The BGM220 board-specific `librail_config_bgm220pc22hna_gcc.a` present locally is only a 130-byte Git-LFS pointer for a 64,584-byte object, so it supplies neither image bytes nor a table. The public BLE PHY arrays use a different `[count<<16 | register-offset]` format (for example `0x0001400c` at `ble_efr32xg22_configurator_out.c:226`), and are not evidence for the 40-record application table. No safe default overlay can therefore be generated from the local SDK.
* **[F]** A separate native startup loop was resolved without inventing provisioning data. Before the TIMER2 model, the stock image repeatedly executed at file offset `0x24baa` while accessing `0x4005000c` (TIMER2 `CMD`: base `0x40050000`, offset `+0x00c`, from the public BG22 timer layout). QEMU now implements the observed 16-bit 38.4 MHz TIMER2 `EN`/`CMD`/`STATUS`/`CNT`/`TOP` subset (`hw/arm/efr32bg22.c`, qtest `/efr32bg22/timer2-counter-contract`); the current 64-case core SoC qtest suite covers that contract and clears the loop. A bounded three-CPU-second post-change run advances through application code instead of repeatedly polling TIMER2 but does not reach `RAC.CMD`. The erased `0x4c000` provisioning table above therefore remains the demonstrated startup divergence; TIMER2 was an independent peripheral-model gate, not an excuse to synthesize that table.
* **[F]** The branch at `0x1bb10` is reached through a small stock clock-source decoder: at `flash-stock.bin` offset `0x1baf0` it reads the `0x40008070` literal at `0x1bb74`, masks bits `0..2`, and accepts only defined source codes. Native QEMU had left that `CMU.SYSCLKCTRL` latch at zero. The public-derived reset table retained in the prior model gives it reset value `1` (FSRCO), which native QEMU now restores. This is a reset-value correction, not a claim about CMU switching latency or other clock-tree behavior.
* **[F]** The stock's early radio-adjacent clock path is now bounded more tightly. The helper at `flash-stock.bin` file offsets `0x26e0c..0x26e22` reaches the clock-source decoder at `0x1baf0..0x1bb10`; that decoder uses literal `0x1bb74 = 0x40008070` (`CMU.SYSCLKCTRL`). The same helper reads literal `0x27258 = 0xb0004fe0` and tests bit 16, the radio-HCLK prescaler bit in the non-secure CMU window. The public BG22 header identifies `SYSCLKCTRL` at the CMU register layout and identifies `CLKSEL` bits `0..2`, reset/default FSRCO, and `RHCLKPRESC` at bit 16 (`vendor/hal_silabs/simplicity_sdk/platform/Device/SiliconLabs/EFR32BG22/Include/efr32bg22_cmu.h:63,663-708`). These are binary access and public field-layout facts only: they do not establish CMU transition latency, clock gating, a RAC transition precondition, or RAC timing.
* **[F]** The immediately adjacent stock PROTIMER setup is at `flash-stock.bin` file offsets `0x29974..0x29a36`; it uses literal `0x29d54 = 0xa801c008`. The retained historical trace independently records its first four secure-window writes at `evidence/stock/radio-trace.log:64-67`: `+0x008 = 0x00011102`, `+0x030 = 0x00001233`, `+0x038 = 0x80015558`, and `+0x00c = 1`. This ties those accesses to the stock control-flow window, but does not identify PROTIMER field/command semantics or prove that it gates, schedules, or times any RAC transition.
* **[F]** Later SRAM-resident startup code loads `MSC.STATUS` through the `0x4003001c` literal at RAM offset `0x0c50` (the copied code reads it at `0x20000b08`). The public-derived reset table gives that status register `0x08000008`; native QEMU had supplied zero and now restores the reset latch. This does not model MSC erase/program commands or infer a flash transaction.
* **[F]** Before that radio initialization, the stock image polls the HFXO enabled status. At `flash-stock.bin` file offset `0x1b0b0` it loads `HFXO.STATUS` through `0x4000c010 + 0x48` and loops on the enabled bit; a native-QEMU QMP register sample while the old always-ready model was active stopped at PC `0x0001b0b2`. At file offset `0x1b13c`, it then masks that status with the literal `0x40010003` at `0x1b524`, requiring `FSMLOCK`, `ENS`, and the low ready bits. The model therefore reports HFXO ready only when `HFXO.CTRL.FORCEEN` is asserted or `CMU.SYSCLKCTRL` selects HFXO, and models `CMD.MANUALOVERRIDE` (bit 1) releasing `FSMLOCK`, matching the prior source-backed simulator rule. This is a clock-gating prerequisite only; it does not model RF calibration or radio timing.
* **[F]** SEQRAM starts with a Cortex-M style vector table: word 0 = 0x00003E90 (initial stack pointer inside SEQRAM), word 1 = 0x0000329D (reset vector, Thumb bit set), words 2–6 and 11–15 share one default handler, and the vectors from index 16 on point to distinct handlers. Reset code begins at 0x329C. So the sequencer is a small 32-bit microcontroller running Thumb code from the same 16 kB RAM it uses for stack and data.
* **[F]** Open code execution on that core is hardware-confirmed. A replacement
  vector table (SP `0x3e00`, reset `0x101`) and four ARMv6-M instructions loaded
  through the M33 debug view wrote the sentinel `0x12345678` to local
  `SEQ.SPARE0` at `0x4fe0` after `RAC.CTRL.CPUWAITDIS` was set. A fresh OpenOCD
  attachment read the sentinel back unchanged. This proves the loader, reset
  vector, local address space, and execution gate without copying vendor code;
  it does not yet implement a radio event or transmit a frame. (Since then
  the sequencer runs the TXWARM/TX/TXPD and RXWARM handlers, the
  turnarounds and the staged replies: §7.61, §7.62, §7.80, §7.82, §7.87.)
* **[F]** Instruction mix over 0x100..0x3300: 303 `bl`, `ldr`/`str`/`movs`/`lsls` dominate, `mrs`, `cpsid`, `cpsie`, `wfi` present; zero `cbz`, `it`, `ldrex`, `movw/movt`, `ubfx`, `udiv`. The code assembles and disassembles cleanly with the ARMv6-M instruction set (`arm-none-eabi-objdump -M force-thumb`). **[I]** It is ARMv6-M compatible code (a Cortex-M0+ class core). The core's identity is **[?]**: no public document available offline states it.
* **[F]** The image in the stock application is not byte-identical to the one in the SDK's `generic_efr32xg22_release_seq.o` (a 32-byte prefix search of all its large data sections finds nothing in the stock firmware). The two are different RAIL versions; the M33↔sequencer contract changes between versions. The open sequencer therefore has to be built against *one* M33-side driver, not against "the" Silabs interface.
* **[F]** Peripheral blocks referenced by literals in the sequencer image (counting aligned 32-bit constants in 0xA8000000–0xAAFFFFFF): 0xA8020000 (and its alias pages 0xA8021000/0xA8022000) by far the most, then 0xA8004000, 0xA8014000, 0xA801C000, 0xA8016000, 0xA8006000, 0xA8018000, 0xAA000000. The sequencer therefore programs the same radio blocks as the M33 does, and shares them with it. The now-source-backed map names `0xA8004000` as FRC and `0xAA000000` as BUFC; the earlier BUFC-at-FRC-base label here was stale. Other block names still require trace or public-table corroboration. The observed interrupt names are AGC, BUFC, FRC_PRI, FRC, MODEM, PROTIMER, RAC_RSM, RAC_SEQ, RDMAILBOX, RFSENSE, PRORTC, SYNTH (device header IRQ list).
* **[?]** The M33↔sequencer protocol (RDMAILBOX, `rfhal_mailbox*.o`, `seq_globals.o`), the sequencer's interrupt sources, and the RAC state machine are undocumented in everything available offline. The open driver does not use stock's protocol: its own shared words are `include/hal/efr32/seq_abi.h`; the `SEQIF` state-event IRQs and the RAC state machine were measured on silicon (§7.62, §7.72).

## 3. What has to be replaced

| Layer | Stock | Open replacement (status) | Hardware knowledge needed |
|---|---|---|---|
| Host | Zephyr, open | nothing | none |
| HCI driver | `hci_silabs_efr32.c` over `sl_btctrl` | **done**: Zephyr's in-controller HCI (`zephyr,bt-hci-ll-sw-split`) | none |
| Link layer | `liblinklayer.a` | **done**: Zephyr's split controller, ULL plus the OpenISA-derived LLL (`modules/efr32-radio`, §5 "Alternative"); `crates/ble-ll` is not used by the firmware | none (timing budget aside) |
| Radio driver (M33) | RAIL | **done**: `modules/efr32-radio/hal/radio.c`, the controller's `radio_*` HAL | RAC/FRC/BUFC/MODEM/SYNTH/AGC/PROTIMER registers |
| PHY configuration | `sl_rail_ble_config.c` tables (public) + RAIL calibration | public tables applied by `radio_phy_set()`; 1M on silicon, 2M/coded tables imported but not supported (§7.79); TX VCO calibration cached per channel (§7.61, §7.67); demodulator rates per channel (§7.83) | meaning of each register, calibration sequence |
| Sequencer firmware | closed image in SEQRAM | **done**: `sequencer/radio_seq.c` + `radio_seq_core.S`, open ARMv6-M source (§7.97) | RAC state machine, mailbox, every register it touches |
| DEVINFO calibration | read by RAIL at boot | not read by the open driver; whether it needs to is **[?]** | DEVINFO fields (names in `efr32bg22_devinfo.h`, semantics for radio not documented) |

QEMU currently has no Bluetooth controller, bus, backend, or virtio-BT device
implementation to reuse: its only Bluetooth-named material is virtio UAPI IDs
and labels. The native EFR32 model therefore keeps RF exchange internal. A
future standard host endpoint must model the firmware's actual HCI transport;
QEMU's `CharBackend` is an appropriate seam for H:4 only if firmware/board
evidence establishes that HCI-over-that-real-peripheral transport. A generic
16550 or private bridge would not be a faithful substitute. **Done that way:**
the USART model's line side is a chardev, and the `vhci` backend
(`vendor/qemu/chardev/char-vhci.c`) hands the H:4 stream of Zephyr's
`hci_uart` sample on our controller (`tests/hci-uart-port`) to the Linux host
(`sim/README.md`).

## 4. Minimum viable open radio

BLE 1M PHY, legacy connectable advertising on 37/38/39, SCAN_REQ/SCAN_RSP, one connection in the peripheral role, GATT writes in, notifications out, channel selection #1 and #2, supervision timeout, encryption (the suit's firmware enforces MITM pairing: `CONFIG_BT_SMP_ENFORCE_MITM`, so LE encryption is **required** for the real device; it is not in the crate, and the controller has it on RADIOAES, §7.81). **All met on silicon** (§7.84); the controller also stages the scanner's SCAN_REQ and the initiator's CONNECT_IND (QEMU only, §7.87). No 2M/coded PHY, no extended advertising, no CTE, one connection.

### Scheduler ownership invariant

The radio peripheral and its private M0+ sequencer may implement bounded
low-level state-machine actions, but neither owns software scheduling policy.
All M33 launch, completion, timeout, retry, and link-layer work remains in
persistent RTOS jobs. Hardware IRQ handlers only acknowledge/mask a source,
publish state, and signal an existing job's RTOS event; they do not call the
link layer, dispatch a job, alter its priority, or issue a replacement TX/RX
schedule. Signalling eligibility does not choose execution order: the RTOS
scheduler remains the sole authority over when that job runs and what it may
preempt. The radio M0+ must likewise not select, dispatch, preempt, reprioritize,
or synthesize M33 jobs. Characterization probes that run on the M0+ establish
the peripheral hand-off only and are not a production scheduler path.

## 5. Staged plan

| Stage | Work | Test that can run now / next |
|---|---|---|
| 0 **done** (reference only) | `crates/ble-ll`: CRC-24, whitening, CSA #1/#2, advertising/connect PDUs, data header, SN/NESN, LL control subset, window widening/transmit window timing, instants, supervision, HCI framing/event builders, `Peripheral<R: Radio>` driver | `cargo test` in the crate: 30 tests (§6) |
| 1 **dropped** (Zephyr's controller, below) | `hci_efr32_ll.c` replacing `hci_silabs_efr32.c`; HCI command layer for the commands Zephyr's host sends at init (list below); ACL ↔ `queue_data` flow control | native_sim: Zephyr host + our controller + a software central (the mock radio of `tests/peripheral.rs`, scripted by a second instance of the crate in central role or by pre-recorded PDUs) |
| 2 **dropped** (Zephyr's controller; AES-CCM on RADIOAES, §7.81) | Encryption: LL_ENC_REQ/RSP, START_ENC, AES-CCM per Vol 6 Part B §5.1.3 | sample data from Core Spec Vol 6 Part C (encryption sample data) |
| 3a **reference only** | Radio driver v0 on top of RAIL, used only to capture and explain behavior while replacing `liblinklayer.a` | Differential QEMU/hardware evidence; this configuration is not the stage-3 deliverable |
| 3b **done** | Capture ground truth: register dumps/traces from a real EFR32BG22 dev kit over SWD for TX/RX/IFS sequences | Hardware plus matched QEMU snapshots |
| 3c **done; validation continues** | One open driver that programs the radio blocks directly and embeds its source-built ARMv6-M coprocessor firmware; no RAIL or proprietary image in the dependency chain | QEMU M33+M0+ model, machine qtests, real-hardware FIFO/state measurements, and on-air BLE validation: TX on air §7.62, RX §7.64, scan responses §7.68, T_IFS §7.71, connections §7.80/§7.84. Open: 2M/coded PHYs, the scanner and initiator on silicon (§7.87), the suit itself |

Diagnostic stage **2.5** sits between the proven stock Zephyr baseline and 3c:
it builds the vendored Zephyr tree with the open controller/M33 driver, but
explicitly embeds the captured proprietary coprocessor image. Its sole purpose
is fault isolation. The cache option is named
`EFR32_RADIO_PROPRIETARY_SEQ_IMAGE`, emits a configure-time warning, and has no
default; ordinary/stage-3 builds always compile the open ARMv6-M source. A
successful 2.5 result cannot satisfy the open-driver goal. It was used in
§7.58–§7.59 (the stock coprocessor's bus clocks).

For stage 1 (dropped), HCI commands the host needs at init (from `subsys/bluetooth/host/hci_core.c`): Reset, Read Local Features, Read Local Version Info, Read Supported Commands, Set Event Mask, Read Buffer Size / LE Read Buffer Size, LE Read Local Features, LE Read Supported States, LE Set Event Mask, LE Write LE Host Support, LE Read Max Data Length / Write Default Data Length (if enabled), LE Set Default PHY (if enabled), LE Read Filter Accept List size, LE Rand, LE LTK Request (Neg) Reply, LE Set Advertising Parameters/Data/Scan Response/Enable, Disconnect, Read Remote Version Info, LE Read Remote Features, Host Number Of Completed Packets / Set Controller To Host Flow Control (if enabled). Opcode values for the first group are in `hci.rs` and checked against `hci_types.h` by tests.

### Alternative that removes most of stages 0–2 (chosen)
Zephyr ships an open, Apache-2.0 link layer (`subsys/bluetooth/controller/ll_sw`, split controller `ull`/`lll`) with encryption, data length, PHY update, channel selection and a full HCI. It needs a port of its radio HAL (`hal/radio.h`-style `radio_*` calls, a ticker timebase, CCM and RNG) for the EFR32 radio, which is exactly the same missing hardware knowledge as stages 3–5. Using it would trade this Rust crate for C code that is already feature-complete. The crate's value is then limited to being a small, testable reference and a possible Rust-only firmware. Recommendation: decide after stage 3 shows how much of the radio can be driven; both options share the `Radio`-level work. **Decided** (user, task #27): Zephyr's split controller, with the OpenISA LLL derived and patched by `modules/efr32-radio` (`docs/zephyr-ll-port.md`).

## 6. What the crate covers and how it was checked

Verified against external sources (all offline):
* **Channel selection #2**: Core v5.2 Vol 6 Part C §3.1/§3.2 sample data (channel identifier 0x305F, `prn_e` 56857 at event 0, channels 25/20/6/21 and 23/9/34), as quoted in `zephyr/subsys/bluetooth/controller/ll_sw/lll_chan.c` (`lll_chan_sel_2_ut`). The implementation here was written from the spec text and the tests passed first time.
* Opcode numbers, error codes, HCI opcodes/event codes: compared with `zephyr/subsys/bluetooth/controller/ll_sw/pdu.h` and `zephyr/include/zephyr/bluetooth/hci_types.h`.
* Timing constants: SCA table, `EVENT_JITTER_US` 16, T_IFS 150 µs, `WIN_DELAY_LEGACY` 1250 µs, window-widening cap `interval/2 - T_IFS` (zephyr `lll_clock.c`, `lll_vendor.h`, `pdu.h`, `ull_peripheral.c`).

Self-consistency only (no external vector available offline):
* **CRC-24** bit-serial LFSR vs the reflected fast form (equal on random data), appended-CRC detects every single-bit error, empty-message value.
* **Whitening**: involution, 127-bit period (x^7+x^4+1 is primitive), channels differ.
* Peripheral driver against a mock radio and a scripted central: scan response after IFS, 3-channel advertising event with random delay, first-event transmit window, SN/NESN acknowledgement/retransmission/duplicate suppression, bad-CRC silence with growing widening, LL_TERMINATE_IND, feature/unknown responses, supervision timeout.

### What is not verified
1. CRC-24 and whitening bit/byte order against a real on-air capture or a published packet; the transmission order of the CRC register (position 23 first) is from memory of the spec figure.
2. Transmit-window handling for LL_CONNECTION_UPDATE_IND (no extra 1.25 ms delay, window offset measured from the old grid): implemented from the spec description, no vector.
3. Local sleep-clock accuracy (`Config::local_ppm`): board property of the suit, unknown. The stock firmware's value was not extracted.
4. The 150 µs `tx_after_rx` deadline and `EVENT_JITTER_US` margin are assumptions of the trait; whether an M33 interrupt handler can meet them without hardware auto-turnaround is unknown (RAIL's `set_rx_transitions` suggests the radio does the turnaround itself, which the trait mirrors). For the controller this was settled on silicon: the M33 cannot, so the sequencer stages the replies (§7.82).
5. Peer behaviour with real centrals (phones): untested.
6. LL_TERMINATE_IND sent by us is not waited for with an acknowledgement; peripheral latency is not used (we listen every event, which is legal); no encryption.

## 7. Blockers

* The radio register map and the RAC/mailbox semantics. They are not in the SDK headers (only BUFC for xG21 is public), not in the libraries as names (symbols are present, register names are not), and not in anything available offline. Without hardware traces, stages 3 (partly), 5 and 6 cannot be validated; QEMU can only replay what is learned. **Resolved for the BLE path:** the public register models name the fields, and the behaviour the driver relies on was measured on silicon (`docs/radio-tables.md` §1–§4, §7).
* Hardware access for stage 4 (SWD read-out of radio registers during advertising and a connection). **Resolved:** SWD on BG22 kits (§7.6 onward), during connections since §7.80.
* Licence hygiene: no Silabs bytes or disassembly are stored in this repository; the PHY tables above are Zlib-licensed SDK source and may be used with the notice retained.
