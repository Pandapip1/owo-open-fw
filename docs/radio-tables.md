# EFR32BG22 radio: register/table knowledge needed by the Zephyr LL port

**Unvalidated on hardware.** Labels: **[F]** = fact from a cited public source or directly observed in a stock-firmware trace in the QEMU
model; **[I]** = inference, not confirmed. No blob bytes or disassembly are reproduced here. The sibling task (QEMU radio model,
`docs/radio-block.md`) had not committed when this was written; this file does not duplicate it and should be reconciled with it.

## 1. Block map
| Block | Base | Basis |
|---|---|---|
| PRORTC | 0xA8000000 | [I] literal pools in RAIL objects (`rfhal_*`) |
| FRC | 0xA8004000 | [I] same |
| AGC | 0xA800C000 | [I] same (low confidence) |
| unknown (RFCRC?) | 0xA8010000 | [I] |
| MODEM | 0xA8014000 | [I] |
| SYNTH | 0xA8018000 | [I] |
| PROTIMER | 0xA801C000 | [I] |
| RAC | 0xA8020000 | [I] |
| RF mailbox | 0xA8028000, 0xA802C000 | [I] |
| BUFC | 0xAA000000 | [F] layout matches the xG21 public header; matched by stock init trace |

Every block has `+0x004 = EN` [I]. IRQ lines (AGC 31 ... SYNTH 42) are [F] from the device header
(`gecko_sdk/platform/Device/SiliconLabs/EFR32BG22/Include/efr32bg22c224f512im40.h`).

## 2. RAIL PHY configuration table format (`sl_rail_ble_config.c`, Zlib licence)
* [F] The tables are public C arrays of `uint32_t` ending in `0xFFFFFFFF`.
* [I] Entry header `[ty:8][count:8][offset:16]` followed by `count` data words written to consecutive registers; the meaning of `ty` is **undecoded**.
  I could not locate the interpreter in the librail objects (searches for shift-by-24 patterns hit only PTI/PROTIMER code). A cross-check against the stock
  radio writes found no overlap because the stock run had not reached PHY configuration in the model. **Gap T3.**

## 3. Per-function blockers (IDs used in `modules/owo-radio-efr32/hal/radio.c`)
| ID | Needs | Status | Functions |
|---|---|---|---|
| T1 | block enables/reset, which IRQ ends an event | [I] EN at +4; IRQ choice unknown | `radio_setup/reset/sleep/wake/is_off`, `LL_RADIO_IRQn` |
| T2 | RAC command + state/status registers | unknown | `radio_rx_enable`, `radio_tx_enable`, `radio_disable`, `radio_is_*`, `radio_status_reset`, TIFS switch |
| T3 | PHY table apply (MODEM/FRC/AGC/SYNTH data) | format [I], values public | `radio_phy_set` |
| T4 | SYNTH channel/frequency registers | unknown | `radio_freq_chan_set` |
| T5 | FRC descriptors: sync word, CRC poly/init, whitening, length/header | unknown | `radio_aa_set`, `radio_crc_configure`, `radio_whiten_iv_set`, `radio_pkt_configure`, `radio_crc_is_valid` |
| T6 | BUFC RX/TX buffer setup | **[F]** known | `radio_pkt_rx_set`, `radio_pkt_tx_set` |
| T7 | PA power registers | unknown (RAIL PA tables are public per-board) | `radio_tx_power_*` |
| T8 | PROTIMER timebase/compare/capture, 32 kHz RTC sync | unknown | `radio_tmr_*`, `radio_tmr_tifs_*` |
| T9 | AGC RSSI read | unknown | `radio_rssi_*` |

Software-only (nothing blocked, not yet written): filter, bc, gpio, ccm, ar.
Timing constants in `lll/lll_vendor.h` and the `*_delay_get` functions are OpenISA placeholders [I], to be measured once T2/T8 exist.

## 4. BUFC [F]
Layout identical to the public xG21 header with offsets shifted; stock firmware initialises buffers at 0xAA000000 in a pattern consistent with it.
Per-buffer CTRL/ADDR/WRITEOFFSET/READOFFSET/CMD registers give T6. Source: the xG21 `efr32xg21_bufc.h` header (Silicon Labs Gecko SDK, public) and the model trace.

## 5. Shortest path to advertising in the QEMU radio model
1. Sibling model: make RAC state/command registers behave (idle -> TX -> idle, raising an event IRQ) and expose the transmitted BUFC contents to a socket/pcap. Because the model is ours, the register *semantics* can be chosen, but they must match hardware to be useful, so they must come from RE of the stock run (T2).
2. Let stock firmware run to PHY configuration (BLE advertising start) under the model and log the writes; compare to the public tables to decode `ty` (T3) and to label MODEM/FRC registers (T5).
3. Implement T1/T2/T6/T8 in `hal/radio.c` for ADV_IND TX only (no RX, no TIFS), drop the `STUB_REGISTERS` asserts for `radio_tx_enable`/`radio_tmr_start`.
4. Then RX window for SCAN_REQ/CONNECT_IND (T9, T5), then software TIFS (T8).

Hardware validation is a separate, later step and must start with a sniffer and a dummy load, never the suit's output stage.
