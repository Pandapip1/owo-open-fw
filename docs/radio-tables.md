# EFR32BG22 radio: register/table knowledge needed by the Zephyr LL port

**Unvalidated on hardware.** Labels: **[F]** = fact from a cited public source or directly observed in a stock-firmware trace in the QEMU
model; **[I]** = inference, not confirmed. No blob bytes or disassembly are reproduced here. The sibling task (QEMU radio model,
`docs/radio-block.md`) had not committed when this was written; this file does not duplicate it and should be reconciled with it.

## 1. Block map
| Block | Base | Basis |
|---|---|---|
| PRORTC | 0xA8000000 | [I] literal pools in RAIL objects (`rfhal_*`) |
| FRC | 0xA8004000 | [F] public xG22 configurator table address encoding and labels |
| AGC | 0xA800C000 | [F] same |
| RFCRC | 0xA8010000 | [F] same |
| MODEM | 0xA8014000 | [F] same |
| SYNTH | 0xA8018000 | [F] same |
| PROTIMER | 0xA801C000 | [I] |
| RAC | 0xA8020000 | [F] public table plus stock disassembly literals |
| RF mailbox | 0xA8028000, 0xA802C000 | [I] |
| BUFC | 0xAA000000 | [F] layout matches the xG21 public header; matched by stock init trace |

Every block has `+0x004 = EN` [I]. IRQ lines (AGC 31 ... SYNTH 42) are [F] from the device header
(`gecko_sdk/platform/Device/SiliconLabs/EFR32BG22/Include/efr32bg22c224f512im40.h`).

## 2. RAIL PHY configuration table format (`ble_efr32xg22_configurator_out.c`, Zlib licence) [F]
The public xG22 configurator source names every target register and makes the encoding unambiguous. Arrays contain `uint32_t` entries and end in `0xFFFFFFFF`.
Each entry header is `[op:4][block:4][count:8][offset:16]`, followed by `count` data words for consecutive 32-bit registers:

| Field | Meaning |
|---|---|
| `op = 0` | write |
| `op = 1` | read-modify-write AND |
| `op = 3` | read-modify-write OR |
| `block` | 64 KiB page added to `0xA8000000` |
| `count` | number of following data words/registers |
| `offset` | byte offset within the selected 64 KiB page |

Examples: `0x0001400C` writes one word to `FRC.DFLCTRL` (`0xA800400C`); `0x1001C020` ANDs one word into `AGC.CTRL0`; `0x3101...` ORs an AGC register; `0x02020098` writes two words beginning at `RAC.SYNTHREGCTRL`. This closes T3's table-format and block-label portions. Selecting/copying the required BLE 1M table into the open HAL and validating its exact SDK-version values remain implementation work.

## 3. Per-function blockers (IDs used in `modules/efr32-radio/hal/radio.c`)
| ID | Needs | Status | Functions |
|---|---|---|---|
| T1 | block enables/reset, which IRQ ends an event | **[F] RAC has separate RSM and SEQ flag/enable banks; event policy still unknown** | `radio_setup/reset/sleep/wake/is_off`, `LL_RADIO_IRQn` |
| T2 | RAC command + state/status registers | **[F] core command/state map known; sequencer behavior still abstract** | `radio_rx_enable`, `radio_tx_enable`, `radio_disable`, `radio_is_*`, `radio_status_reset`, TIFS switch |
| T3 | PHY table apply (MODEM/FRC/AGC/SYNTH data) | **[F] format decoded and blocks labeled; HAL/table import not implemented** | `radio_phy_set` |
| T4 | SYNTH channel/frequency registers | **[F] implemented from public BLE channel config and CHCTRL field** | `radio_freq_chan_set` |
| T5 | FRC descriptors: sync word, CRC poly/init, whitening, length/header | **[F] RFCRC and BLE whitening seed implemented; public BLE 1M FRC baseline and descriptor fields decoded, but sequencer handoff/buffer behavior remains incomplete** | `radio_aa_set`, `radio_crc_configure`, `radio_whiten_iv_set`, `radio_pkt_configure`, `radio_crc_is_valid` |
| T6 | BUFC RX/TX buffer setup | **[F]** known | `radio_pkt_rx_set`, `radio_pkt_tx_set` |
| T7 | PA power registers | unknown (RAIL PA tables are public per-board) | `radio_tx_power_*` |
| T8 | PROTIMER timebase/compare/capture, 32 kHz RTC sync | **[F] base, command offset, counter/period and compare-bank access observed; field semantics incomplete** | `radio_tmr_*`, `radio_tmr_tifs_*` |
| T9 | AGC RSSI read | **[F] live/frame value registers and fixed-point fields known; readiness/measurement policy unknown** | `radio_rssi_*` |

Software-only (nothing blocked, not yet written): filter, bc, gpio, ccm, ar.
Timing constants in `lll/lll_vendor.h` and the `*_delay_get` functions are OpenISA placeholders [I], to be measured once T2/T8 exist.

## 4. BUFC [F]
The per-buffer field order matches the public xG21 header, but the xG22 trace establishes a different layout: four buffers begin at `+0x00C`, use a `0x40` stride, and have `THRESHOLDCTRL`/`CMD` at buffer-relative `+0x24`/`+0x28`. Global `IF`/`IEN` are at `+0x114`/`+0x118`. For example, stock configures buffer 3 with `CTRL +0x0CC`, `ADDR +0x0D0`, `THRESHOLDCTRL +0x0F0`, then `CMD +0x0F4`; the other three sequences occur at bases `+0x00C`, `+0x04C`, and `+0x08C`. These offsets are now covered by QEMU patch 0006 tests. QEMU patch 0015 adds a bounded bridge guest-memory-read round trip, so a future trace-backed BUFC DMA model can read `BUF.ADDR`; no BUFC DMA trigger or transfer semantics are inferred or modelled yet. Source: the xG21 `efr32xg21_bufc.h` header (Silicon Labs Gecko SDK, public) and the stock 7.9.07 QEMU trace.

The stock trace gives one tighter ordering fact for buffer 3: at `radio-trace.log:283-287` (virtual time 99,318,096 ns), it writes `BUFC+0x0f4 = 1`, reads `BUFC+0x0d0 = 0x20002f50` and `+0x0d4 = 0`, writes global `BUFC+0x100 = 0`, then writes `BUFC+0x0f4 = 4`. The same five-access sequence repeats at `radio-trace.log:293-297` (99,320,816 ns). This demonstrates that firmware consumes the programmed address around these command values, but it does **not** establish which command starts DMA, direction, byte count, or completion condition; the QEMU model therefore still does not perform a DMA transfer.

The public BG21 header (`platform/Device/SiliconLabs/EFR32BG21/Include/efr32bg21_bufc.h:224-243`) gives the compatible `BUF.CMD` mask `0x0f`, with bit 0 named `CLEAR` and bit 1 named `PREFETCH`. QEMU patch 0016 retains that masked register value and implements only `CLEAR`, which agrees with the trace's first command. Bit 2 (`0x4`) is within the mask but unnamed in that header; it is retained for software visibility with no invented side effect.

## 4.1 RAC core [F]
The public Simplicity SDK generated revB2 register model
(`platform/radio/efr32_multiphy_configurator/host_py_rm_studio_internal/full/efr32xg22/revB2/RAC_S_{register,field}.py`)
matches the stock disassembly and trace exactly. At `0xA8020000`: `EN +0x004`, `RXENSRCEN +0x008`, read-only `STATUS +0x00C`, write-only `CMD +0x010`, `CTRL +0x014`, `FORCESTATE +0x018`, `IF/IEN +0x01C/+0x020`, and `SEQIF/SEQIEN +0x028/+0x02C`. SET/CLR/TGL aliases are at `+0x1000/+0x2000/+0x3000`.

`CMD` bit 0 is `TXEN`, bit 1 `FORCETX`, bit 5 `TXDIS` (`0x20`), bit 7 `RXCAL` (`0x80`), and bit 8 `RXDIS` (`0x100`). `RXENSRCEN.SWRXEN` occupies bits 0..7. `STATUS.STATE` is bits 24..27; the matching sequencer flags establish OFF=0, RXWARM=1, RXSEARCH=2, RXFRAME=3, RXPD=4, RX2RX=5, RXOVERFLOW=6, RX2TX=7, TXWARM=8, TX=9, TXPD=10, TX2RX=11, TX2TX=12, SHUTDOWN=13. `STATUS` also exposes `SEQACTIVE`, `DEMODENS`, `TXENS`, and `RXENS` in bits 28..31.

This identifies every previously observed stock write: the recovered image reaches radio initialization at about 98.5 ms virtual time, polls `STATUS.STATE`, writes `RXCAL`, `RXDIS`, and `TXDIS`, and waits for TX state 9 before disabling. QEMU patch 0007 replaces patch 0006's synthetic register contract with this map. Its packet modulation and fixed 150 us completion are still simulator abstractions, not hardware timing claims.

## 4.2 SYNTH, FRC, and AGC fields [F]
The same public revB2 model identifies the remaining core data-path fields. SYNTH has 28-bit `FREQ` at `+0x034`, 21-bit `IFFREQ` at `+0x038`, `DIVCTRL` at `+0x03C`, 6-bit `CHCTRL.CHNO` at `+0x040`, and 18-bit `CHSP` at `+0x044`. The public BLE 1M channel config specifies base 2402 MHz, 2 MHz spacing, physical channels 0..39. Zephyr passes MHz relative to 2400 (`2`, `26`, `80` for advertising), so `radio_freq_chan_set` now validates that input and writes `CHNO = (chan - 2) / 2`. QEMU patch 0008 preserves these fields with their public masks. The PHY table remains responsible for `FREQ` and `CHSP`.

FRC exposes dynamic-frame-length configuration at `DFLCTRL +0x00C`, `MAXLENGTH +0x010`, frame/length/address word-count comparators at `+0x020..+0x028`, whitening control/polynomial/init at `+0x030/+0x034/+0x038`, RX control at `+0x058`, and four frame configuration descriptors `FCD0..3` later in the block. RFCRC is a separately generated block at `0xA8010000`.

The public xG22 BLE 1M base table records `DFLCTRL = 0x00018101`, `WCNTCMP0/1 = 0/1`, whitening control/poly/init all zero, `CONVRAMADDR = 0x4000`, and `FCD1/2/3 = 0x4CFF/0x4DFF/0x4DFF` ([F] source locator: `ble_efr32xg22_configurator_out.c:225-242`). Its per-PHY table adds `MAXLENGTH = 0x4101`, `FECCTRL = 0`, `CTRL = 0x407A0`, and `FCD0 = 0x4CFF` (`...:474-481`); it also writes `SEQ.PHYINFO`, whose effect is not inferred here. QEMU patch 0012 retains the public masks for the decoded `MAXLENGTH`, `CTRL`, and `FCD0..3` registers; patch 0014 additionally retains the base-table `WCNTCMP`, RAM-address, FEC, trailing-data, convolution, puncture, clock-gate, and supplemental-length latches using the generated public masks (`FRC_S_register.py:190-229,332-401,474-577,966-979,1146-1229`). Neither patch assigns them packet-pipeline effects. The generated field definitions (`FRC_S_field.py:197-274`, `:3357-3434`) decode `DFLCTRL` as `DFLMODE=1`, `DFLOFFSET=1`, `DFLBITS=8`, `MINLENGTH=1` (the other named fields zero). They decode each `0x4CFF` descriptor as `WORDS=255`, `BUFFER=0`, `INCLUDECRC=1`, `CALCCRC=1`, `SKIPWHITE=1`, with the remaining named fields zero; `0x4DFF` differs only by `BUFFER=1`. This establishes public configuration values, not their packet-pipeline or buffer semantics.

The public RAIL archive adds a narrow runtime observation for sync-word programming. In `librail_efr32xg22_gcc_release.a(rail_ble_rf_hal.o)`, section `.text.sli_rail_51695f5143b2a4311e2362c17f8fac96` starts at object file offset `0x334`. At section-relative `+0x4a..+0x5c`, it constructs a three-word configuration record with target `0xA8014068` and a source word loaded from its third argument at `+0x0c`. The generated model names that 32-bit target `MODEM.SYNC0.SYNC0` (`MODEM_S_field.py:1167-1174`); the public BLE table independently labels the same offset `MODEM.SYNC0` (`ble_efr32xg22_configurator_out.c:275-276`). QEMU patch 0011 retains this register and its 32-bit public mask. This does not identify that source word as an access address, specify byte order, or establish when the sequencer consumes it, so `radio_aa_set` remains intentionally unimplemented.

That same archive section gives a direct whitening setup: at file offsets `0x3ea..0x3f2` (section-relative `+0xb6..+0xbe`) it forms `FRC.WHITEPOLY = 0x44` at `0xA8004034`; at file offsets `0x404..0x40c` (`+0xd0..+0xd8`) it ORs bit 6 into an input byte and stores the result as the data word for `FRC.WHITEINIT` at `0xA8004038`. The generated xG22 model confirms 16-bit writable `WHITEPOLY` and `WHITEINIT` fields at those offsets (`FRC_S_register.py:293-328`). Zephyr's LLL calls `radio_whiten_iv_set(chan)` after its BLE channel selection (`lll/lll.c:445`), and its OpenISA reference HAL expresses the BLE seed as `iv | 0x40` (`hal/RV32M1/radio/radio.c:653-681`). Therefore `radio_whiten_iv_set` now accepts channel `0..39` and writes `WHITEPOLY=0x44`, `WHITEINIT=0x40|channel`. This records configuration only; QEMU does not yet whiten packets and the HAL has not inferred FRC sequencing.

For BLE 1M, the public table writes `RFCRC.CTRL = 0x708`, `RFCRC.INIT = 0xAAAAAA`, and `RFCRC.POLY = 0xDA6000` (source: `ble_efr32xg22_configurator_out.c`, lines 268–270). Zephyr passes the Bluetooth CRC polynomial `0x00065B` and advertising IV `0x555555`; reversing their 24 significant bits produces exactly those public-table values. `radio_crc_configure` now enables RFCRC and writes that public control value plus bit-reversed polynomial/IV. QEMU patch 0009 supplies the public RFCRC register masks. It intentionally does not issue the standalone `RFCRC.CMD.INITIALIZE` command or claim packet CRC validity: the FRC/sequence data path is still uncharacterized.

AGC's live `RSSI +0x018` and latched `FRAMERSSI +0x01C` have the same signed-looking 8-bit integer field in bits 8..15 and two fractional bits in bits 6..7 (reset raw value `0x8000`). The generated files do not document signedness, units, when the frame latch updates, or a measurement-ready contract, so T9 is only structurally resolved.

## 4.3 PROTIMER access pattern [F]
The public xG22 RAIL archive's `rfhal_protimer.o` accesses the secure alias of `0xA801C000`, confirming the base. It writes command values including `1`, `4`, `0x20`, `0x40`, `0x80`, and `0x10000` to `+0x00C`; uses `+0x030` and `+0x038` as timer state/period inputs; and accesses four 16-byte compare slots starting at `+0x100` (with associated control words at `+0x200`). The object also drives `RAC_RSM_IRQHandler`, so timer behavior is part of the event path. Locators in `librail_efr32xg22_gcc_release.a(rfhal_protimer.o)` are `.text.sli_rail_2867cf…` file offset `0x11c` (enable/command), `.text.sli_rail_daa5e20…` offset `0x138` (command `4`), `.text.sli_rail_18d8f930…` offset `0x180` (commands `0x20`/`0x80`), `.text.sli_rail_4aff3cd…` offset `0x558` (command `0x10000`), and `.text.sli_rail_30b95b…` offset `0x20c` (compare-bank accesses). These are access facts only—the register field names, counter unit, capture semantics, and safe TIFS sequence remain unresolved, so no HAL or QEMU timer implementation is inferred from them.

## 5. Shortest path to advertising in the QEMU radio model
1. **Partially done in QEMU patches 0006–0007:** BUFC has the trace-corrected programmed-data path and threshold IRQs; RAC uses the public xG22 register/command/state map and distinct RAC_RSM/RAC_SEQ interrupt banks. Seven focused tests cover both models. BUFC DMA via `BUF.ADDR`, packet socket/pcap output, and real sequencer timing remain unimplemented.
2. Let stock firmware run to PHY configuration (BLE advertising start) under the model and log the writes; compare to the public tables to decode `ty` (T3) and to label MODEM/FRC registers (T5).
3. Implement T1/T2/T6/T8 in `hal/radio.c` for ADV_IND TX only (no RX, no TIFS), drop the `STUB_REGISTERS` asserts for `radio_tx_enable`/`radio_tmr_start`.
4. Then RX window for SCAN_REQ/CONNECT_IND (T9, T5), then software TIFS (T8).

Hardware validation is a separate, later step and must start with a sniffer and a dummy load, never the suit's output stage.
