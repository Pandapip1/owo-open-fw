# Zephyr's own link layer on the EFR32BG22: what a port must provide

**Unvalidated on hardware. Nothing here has transmitted or received a packet; the radio hooks are stubs.**
Decision (user, task #27): use Zephyr's open split controller (`BT_LL_SW_SPLIT`, ULL/LLL) instead of
`crates/ble-ll` or the Silicon Labs `liblinklayer.a`. All paths are relative to the Zephyr tree used
in this workspace (4.5.0-rc2, commit `67276f5b2`), `subsys/bluetooth/controller/` unless stated.

## 1. How the controller is built, and the seam

* **[F]** `BT_LL_SW_SPLIT` is on only if a devicetree node `zephyr,bt-hci-ll-sw-split` is enabled (`Kconfig:143-149`).
  The node binding is `dts/bindings/bluetooth/zephyr,bt-hci-ll-sw-split.yaml`; the board's own HCI node
  (`silabs,bt-hci-efr32`, `dts/arm/silabs/xg22/efr32bg22.dtsi:8-14`) is `disabled` by default, and the explorer-kit
  dtsi chooses it as `zephyr,bt-hci` (`boards/silabs/explorer_kits/xg22/xg22_explorer_kit.dtsi:15`). The overlay in
  `tests/radio-port/boards/bg22_ek4108a.overlay` disables that node, adds the sw-split node under `&radio` and re-points `zephyr,bt-hci`.
* **[F]** The lower link layer ("LLL" vendor) is selected only by `BT_LLL_VENDOR_NORDIC` (`Kconfig.ll_sw_split:8`, needs
  `SOC_COMPATIBLE_NRF`) or `BT_LLL_VENDOR_OPENISA` (`Kconfig.ll_sw_split:88`, needs `SOC_OPENISA_RV32M1`), and `CMakeLists.txt:193-221`
  adds the matching include directories and `add_subdirectory(ll_sw/nordic|openisa)`. No other Kconfig/CMake code
  mentions these symbols (`grep -rn BT_LLL_VENDOR` over subsys/include/drivers/soc/dts finds only those two files),
  so a third vendor does not need a Zephyr patch: define a Kconfig symbol with the same `select`s and add the include
  directories from a module. That is what `modules/efr32-radio` does.
* **[F]** The generic code reaches the vendor layer only through `#include "hal/<x>.h"` -> `hal/<x>_vendor_hal.h`
  (`hal/radio.h`, `hal/ticker.h`, `hal/cntr.h`, `hal/cpu.h`, `hal/swi.h`, `hal/debug.h`: 7-line forwarding headers),
  `lll/lll_vendor.h`, `lll/pdu_vendor.h` (included from `hci/hci.c:38,42`) and `hci_vendor.h` (`hci/hci.c:80`).
* **[F]** Two complete vendor implementations exist. Nordic's is PPI/DPPI hard-wired (`ll_sw/nordic/hal/nrf5/radio/radio.c`,
  2843 lines, ~95 `radio_*` functions incl. extended advertising, ISO, DF, hardware TIFS and CCM). OpenISA's
  (`ll_sw/openisa/hal/RV32M1/radio/radio.c`, 1622 lines, 78 `radio_*`) uses **software TIFS**, software AES-CCM, legacy
  advertising/scanning/connections, one PHY at a time, and has its LLL sources in `ll_sw/openisa/lll/*` (6453 lines).
  The EFR32 radio has no PPI; the OpenISA split is the closer fit and is what the module reuses.
* **[F]** Consequence found while building (`tests/radio-port`): OpenISA's LLL is **bit-rotted** against current Zephyr. Two
  places do not build outside RV32M1 and are patched in the derived copy by `modules/efr32-radio/CMakeLists.txt`:
  `lll/lll_conn.c:547` writes `pdu_data.resv` (no longer a member of `struct pdu_data`, `ll_sw/pdu.h:978`), and
  `lll/lll.c:83-84` reads the LPTMR1 compare flag in the tick ISR. Everything else compiled unchanged.

## 2. What the port must provide

### 2.1 Interrupt plumbing (`hal/swi.h`, `lll/lll.c`)
* **[F]** `lll_init` (`openisa/lll/lll.c:140-165`) calls `hal_swi_init()`, `IRQ_CONNECT`s four handlers
  (`LL_RADIO_IRQn` -> `isr_radio`, `LL_RTC0_IRQn` -> tick/worker ISR, `HAL_SWI_RADIO_IRQ` -> LLL mayfly,
  `HAL_SWI_JOB_IRQ` -> ULL-low mayfly), enables them and calls `radio_setup()` (`lll.c:164`).
* **[F]** Needed from the SoC: one radio interrupt, one tick-counter compare interrupt, two software-pendable interrupts
  (`hal_swi_lll_pend/worker_pend/job_pend`). EFR32BG22 has `SW0..SW3_IRQn` = 52..55 and the radio lines
  `AGC 31, BUFC 32, FRC_PRI 33, FRC 34, MODEM 35, PROTIMER 36, RAC_RSM 37, RAC_SEQ 38, RDMAILBOX 39, RFSENSE 40, PRORTC 41, SYNTH 42`
  (`gecko_sdk/platform/Device/SiliconLabs/EFR32BG22/Include/efr32bg22c224f512im40.h:94-105,115-118`).
* **Done** (`include/hal/efr32/swi.h`, `ll_irqs.h`): radio-LLL = SW0, ULL-low = SW1, worker = SW2 (pended from the counter alarm callback),
  `LL_RADIO_IRQn = RAC_SEQ_IRQn` **[I] placeholder** (which radio line ends an event is T1).

### 2.2 Tick counter and ticker (`hal/cntr.h`, `hal/ticker.h`, `ticker/ticker.c`)
* **[F]** Contract: `cntr_init/start/stop/cnt_get/cmp_set` (`hal/cntr.h`), 32-bit counter at 32768 Hz
  (`HAL_TICKER_CNTR_CLK_UNIT_FSEC 30517578125`, `openisa/hal/RV32M1/ticker.h`), refcounted start, compare interrupt that
  runs `ticker_trigger(0)` (`openisa/lll/lll.c:78-95`). `mayfly.c`/`ticker.c` glue (`hal/RV32M1/mayfly.c`, `ticker.c`) is
  generic and is reused by derivation.
* **Done** (`hal/cntr.c`): implemented on the Zephyr Counter API, devicetree alias `ll-counter` = `&burtc0` (BURTC, `dts/arm/silabs/xg22/xg22.dtsi:518-525`),
  because RTCC is the system sleeptimer (`xg22.dtsi:18 silabs,sleeptimer = &rtcc0`). Not run on hardware; the frequency/top asserts are in `cntr_init`.
  **Open:** BURTC clock source and its keeping time in EM2/EM3; local sleep-clock accuracy (`lll_clock.c` returns a fixed 500 ppm,
  `openisa/lll/lll_clock.c:16-33`, and `lll_clock_wait()` does nothing: HFXO start/stop is therefore not handled at all).

### 2.3 Crypto and entropy
* **[F]** `lll.c:124-127` gets the default entropy device (`BT_CTLR_ENTROPY`); the xG22 build selects `CONFIG_ENTROPY_GECKO_TRNG`. Nothing to write.
* **Done** `hal/ecb.c`: AES-128 ECB (`hal/ecb.h`: `ecb_encrypt`, `ecb_encrypt_be`, `ecb_encrypt_nonblocking`) via PSA (`psa_cipher_encrypt`,
  `PSA_ALG_ECB_NO_PADDING`), the API Zephyr's host uses (`subsys/bluetooth/crypto/bt_crypto_psa.c:10`). TinyCrypt is no longer in this tree.
* **Implemented and compile-checked** `radio_ccm_rx_pkt_set/tx_pkt_set/is_done/mic_is_valid/is_available` (`hal/ccm.h`: key, 39-bit counter, direction, IV):
  synchronous software AES-CCM with a 4-byte MIC through `psa_aead_*` and `PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM,4)`, preserving OpenISA's 13-byte BLE nonce and masked one-byte AAD contract. The nonce/AAD construction was cross-checked against OpenISA's Bluetooth Core sample vector (`f388...6088`). `radio_ar_*` (resolvable-address match) remains TODO.

### 2.4 The radio HAL (`hal/radio.h` -> `hal/efr32/radio/radio.h`, 78 functions)
The API is declared in `openisa/hal/RV32M1/radio/radio.h:13-108`. How OpenISA uses the hardware (this is the model to follow):
* **[F]** One radio interrupt for *TX done*, *RX header watermark* and *timer 2 (timeout)*; the handler (`radio.c:313-400`) timestamps AA and end,
  copies the PDU on the watermark interrupt (`pkt_rx`, `radio.c:233`), and starts the next RX/TX in software at `end + T_IFS - wake-up` by loading a compare timer
  (`radio.c:376-385`), then calls the LLL callback. `radio_switch_complete_and_rx/tx/disable` (`radio.c:928-950`) only record `next_radio_cmd`.
* **[F]** Timer API: `radio_tmr_start*` (`radio.c:1101-1140`) arms a compare at an absolute 32.768 kHz tick + remainder converted to the radio timer;
  `radio_tmr_hcto_configure` (`radio.c:1164`) arms the header-complete timeout; `radio_tmr_aa_capture/end_capture/sample` (`radio.c:1176-1223`) capture timestamps.
* **EFR32 blocks that correspond**: PROTIMER (timebase/compare/capture), RAC (RX/TX commands, state), FRC (frame: CRC, whitening, length, sync word), BUFC (RX/TX buffers),
  MODEM/AGC/SYNTH (PHY, RSSI, channel), RFMAILBOX/sequencer. Silicon Labs' public generated xG22 configurator models expose the register and field layouts; the pinned QEMU model uses the source- and trace-matched RAC core map. Behavioral semantics beyond those names still need trace or hardware validation; see `docs/radio-tables.md`.

Status of every function (all in `modules/efr32-radio/hal/radio.c`, each commented with its class):

| Group | Functions | Class | Blocked on |
|---|---|---|---|
| Setup | `radio_setup`, `radio_reset`, `isr_radio`, `radio_isr_set`, `radio_sleep/wake/is_off` | regs | T1 (block enables, IRQs), T2 |
| PHY/channel | `radio_phy_set`, `radio_freq_chan_set` | regs, public tables | T3, T4 |
| Packet format | `radio_aa_set`, `radio_whiten_iv_set`, `radio_crc_configure`, `radio_pkt_configure`, `radio_pkt_rx/tx_set` | regs | T5 (FRC), T6 (BUFC known) |
| Commands/status | `radio_rx_enable`, `radio_tx_enable`, `radio_disable`, `radio_status_reset`, `radio_is_ready/done/idle`, `radio_has_disabled`, `radio_crc_is_valid` | regs | T2 |
| TIFS/switch | `radio_switch_complete_*`, `radio_tmr_tifs_set`, `radio_tmr_tifs_base_get` | regs | T2, T8 |
| Timer | `radio_tmr_*` (17) | regs | T8 |
| RSSI | `radio_rssi_*` | regs | T9 |
| Power | `radio_tx_power_set/max_set` | regs | T7 |
| Delays | `radio_*_ready_delay_get`, `radio_*_chain_delay_get` | measured constants | hardware timing capture |
| Software | `radio_pkt_empty/scratch_get`, `radio_gpio_*` (no PA/LNA: no-ops), `radio_ccm_*` (implemented and compile-checked); `radio_filter_*`, `radio_bc_*`, `radio_ar_*` remain | software | nothing |

Functions that would start air activity (`radio_rx_enable`, `radio_tx_enable`, `radio_tmr_start*`) `LL_ASSERT` while `CONFIG_EFR32_RADIO_STUB_REGISTERS=y`.

### Packet-target bounds retained for future RX delivery

**[F]** Every current OpenISA `radio_pkt_configure` call uses an eight-bit length field:
legacy advertising/scanning pass `PDU_AC_LEG_PAYLOAD_SIZE_MAX` (37) in
`lll/lll_adv.c:353,356` and `lll/lll_scan.c:160,163`; connection setup passes its
effective data-length bound (or that bound plus the four-byte MIC) in
`lll/lll_conn.c:444,450`; and DTM passes 255 in `lll/lll_test.c:195`.
The current EFR32 packet-stage metadata records those values, and its dormant
normalized-PDU completion helper accepts only an explicit two-octet LL header
plus exactly the declared number of payload octets. It is intentionally not an
FRC/BUFC reader.

**[F]** Current receive targets have sufficient capacity for those configured
lengths, but this is a call-topology property—not a general `void *` capacity
contract. Advertising and unencrypted connection paths use `node_rx->pdu`
after `ull_pdu_rx_alloc_peek()` (`lll_scan.c:166-168`, `lll_conn.c:424-452`);
the RX pool provisions each element as the maximum of advertising, data, control,
and user PDU sizes (`ull.c:437-450`). Encrypted connections first target the
HAL scratch PDU (`lll_conn.c:444-447`), and DTM/advertiser scratch paths use
`radio_pkt_scratch_get()` (`lll_test.c:308`, `lll_adv.c:534`); that EFR32 buffer
is sized for maximum data PDU plus MIC (`hal/radio.c:61-64`). A future caller
outside these paths must supply an explicit destination-capacity contract;
the generic radio API itself has no size argument.

## 3. Build integration (done, compile-checked)
* `modules/efr32-radio`: `Kconfig` (symbol `BT_LLL_VENDOR_EFR32`, same selects as OpenISA, PSA instead of tinycrypt), `CMakeLists.txt`.
  This is intentionally generic driver naming; OWO names are reserved for code specific to the suit/device.
  (derives the OpenISA LLL/hal files into the build tree with `hal/RV32M1/` -> `hal/efr32/` and the two patches above, then adds the module's own `hal/{cntr,ecb,radio}.c`).
  No Zephyr code is copied into this repository.
* `tests/radio-port`: minimal app (`bt_enable`) + overlay. `west build -b bg22_ek4108a tests/radio-port -- -DCONFIG_BUILD_ONLY_NO_BLOBS=y`
  builds and links the real ULL (`ll_adv_enable`, `lll_init`, ...) with the module: 137,280 B flash, 25 KiB RAM (no application on top).
  It cannot run: the radio hooks are stubs and nothing configures BURTC.

## 4. Known gaps/risks (not resolved)
1. **Register knowledge** (T1-T9): the blocker. Shortest path in `docs/radio-tables.md` §5.
2. **Software TIFS on a Cortex-M33** with unknown RAC latency: if T_IFS (150 us +-2) cannot be met from the interrupt, a hardware path (PRS/PROTIMER -> RAC) is required.
3. **HFXO / sleep**: `lll_clock_wait` is a no-op; `EVENT_OVERHEAD_XTAL_US` etc. in `lll/lll_vendor.h` are OpenISA placeholders. Interaction with Zephyr PM (EM2) is unexamined.
4. **Feature set**: legacy 1M only (OpenISA LLL). The stock suit needs LE Secure/legacy pairing with MITM (host side, fine) and encryption (needs CCM above).
5. **Flash/RAM**: the controller adds ~100 kB flash on top of the application; the application slot in `boards/bg22_ek4108a.overlay` is 408 K, fine, but the existing app's RAM is already 85 % used without the controller.
