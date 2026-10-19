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

## 3. Per-function blockers (IDs used in `modules/owo-radio-efr32/hal/radio.c`)
| ID | Needs | Status | Functions |
|---|---|---|---|
| T1 | block enables/reset, which IRQ ends an event | [I] EN at +4; IRQ choice unknown | `radio_setup/reset/sleep/wake/is_off`, `LL_RADIO_IRQn` |
| T2 | RAC command + state/status registers | unknown | `radio_rx_enable`, `radio_tx_enable`, `radio_disable`, `radio_is_*`, `radio_status_reset`, TIFS switch |
| T3 | PHY table apply (MODEM/FRC/AGC/SYNTH data) | **[F] format decoded and blocks labeled; HAL/table import not implemented** | `radio_phy_set` |
| T4 | SYNTH channel/frequency registers | unknown | `radio_freq_chan_set` |
| T5 | FRC descriptors: sync word, CRC poly/init, whitening, length/header | unknown | `radio_aa_set`, `radio_crc_configure`, `radio_whiten_iv_set`, `radio_pkt_configure`, `radio_crc_is_valid` |
| T6 | BUFC RX/TX buffer setup | **[F]** known | `radio_pkt_rx_set`, `radio_pkt_tx_set` |
| T7 | PA power registers | unknown (RAIL PA tables are public per-board) | `radio_tx_power_*` |
| T8 | PROTIMER timebase/compare/capture, 32 kHz RTC sync | unknown | `radio_tmr_*`, `radio_tmr_tifs_*` |
| T9 | AGC RSSI read | unknown | `radio_rssi_*` |

Software-only (nothing blocked, not yet written): filter, bc, gpio, ccm, ar.
Timing constants in `lll/lll_vendor.h` and the `*_delay_get` functions are OpenISA placeholders [I], to be measured once T2/T8 exist.

## 4. BUFC [F]
The per-buffer field order matches the public xG21 header, but the xG22 trace establishes a different layout: four buffers begin at `+0x00C`, use a `0x40` stride, and have `THRESHOLDCTRL`/`CMD` at buffer-relative `+0x24`/`+0x28`. Global `IF`/`IEN` are at `+0x114`/`+0x118`. For example, stock configures buffer 3 with `CTRL +0x0CC`, `ADDR +0x0D0`, `THRESHOLDCTRL +0x0F0`, then `CMD +0x0F4`; the other three sequences occur at bases `+0x00C`, `+0x04C`, and `+0x08C`. These offsets are now covered by QEMU patch 0006 tests. `BUF.ADDR` DMA behavior remains unmodelled. Source: the xG21 `efr32xg21_bufc.h` header (Silicon Labs Gecko SDK, public) and the stock 7.9.07 QEMU trace.

## 4.1 RAC trace facts [F]
The recovered stock image reaches radio initialization at about 98.5 ms virtual time. At the inferred RAC base `0xA8020000`, it enables `+0x004`, writes `+0x010` with `0x80`, `0x100`, and `0x20` in distinct initialization paths, writes several values to `+0x014`, and performs a write/read handshake using `+0x040`. These observations narrow T2 but do not yet identify command names or state encodings. Patch 0006's `1/2/3` command contract remains explicitly simulator-only and must not be treated as this hardware map.

## 5. Shortest path to advertising in the QEMU radio model
1. **Partially done in QEMU patch 0006:** the model now has the trace-corrected xG22 BUFC programmed-data path, offsets/status/threshold IRQs, and a test hook that exposes each buffer's bytes. It also has an explicitly simulator-only RAC contract (idle/RX/TX, timed TX completion, RAC_SEQ IRQ) so model work can proceed without representing its offsets as hardware facts. BUFC DMA via `BUF.ADDR` still needs guest-memory reads in the bridge protocol, and socket/pcap output is not implemented. The recovered stock trace now constrains several RAC offsets, but assigning hardware-compatible T2 semantics still needs disassembly correlation or SWD capture.
2. Let stock firmware run to PHY configuration (BLE advertising start) under the model and log the writes; compare to the public tables to decode `ty` (T3) and to label MODEM/FRC registers (T5).
3. Implement T1/T2/T6/T8 in `hal/radio.c` for ADV_IND TX only (no RX, no TIFS), drop the `STUB_REGISTERS` asserts for `radio_tx_enable`/`radio_tmr_start`.
4. Then RX window for SCAN_REQ/CONNECT_IND (T9, T5), then software TIFS (T8).

Hardware validation is a separate, later step and must start with a sniffer and a dummy load, never the suit's output stage.
