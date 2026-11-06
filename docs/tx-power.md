# Transmit power of the EFR32BG22 radio

`CONFIG_EFR32_RADIO_TX_POWER` (modules/efr32-radio, default n) maps the Zephyr controller's
transmit power in dBm to the BG22's 6 dBm power amplifier (PA). Off, the PA stays at its reset
level, its highest; that is the OWO product's configuration and its images are byte-identical
with and without this code (section 6).

Evidence tags as in docs/radio-tables.md: [F] found in a public source or measured, [I] inferred,
[U] unknown. No output power in this document was measured: no RF instrument was used. The
register readbacks and ramp times of section 5 were measured on silicon (docs/radio-tables.md
7.89); section 5 lists what remains.

## 1. What the controller gets

| Zephyr interface | With `EFR32_RADIO_TX_POWER` |
|---|---|
| `BT_CTLR_TX_PWR` choice | The default of every role (`RADIO_TXP_DEFAULT`, derived `hal/efr32/radio/radio.h`). Zephyr offers 0, -4, -8, -12, -16 and -20 dBm to every SoC; the module adds +1 to +9 dBm. Default 0 dBm. |
| `BT_CTLR_TX_PWR_DYNAMIC_CONTROL` (implies the option) | Each advertiser's, scanner's and connection's own level: the derived OpenISA LLL programs `lll->tx_pwr_lvl` at each event's prepare instead of the fixed default (module CMakeLists.txt). |
| Vendor HCI Write / Read Tx Power Level (`BT_HCI_OP_VS_WRITE/READ_TX_POWER_LEVEL`) | Through the controller's `ll_tx_pwr_lvl_set/get()`: the selected level is `lll_radio_tx_pwr_floor()` of the request; `0x7F` (no preference) is the default. |
| HCI LE Read Transmit Power | `lll_radio_tx_pwr_min_get/max_get()`: -27 and 9 dBm (section 3). |
| HCI Read Transmit Power Level (connections) | The connection's level (Zephyr `ll_tx_pwr_lvl_get()`), with dynamic control. |
| HCI LE Read Advertising Physical Channel Tx Power | Unchanged: Zephyr's `hci.c` answers 0 for every controller. |
| LE Power Control (`BT_CTLR_LE_POWER_CONTROL`) | Not available: the OpenISA LLL does not support it. |

A request gets the highest PA level whose published output (section 3) does not exceed it, or
the lowest level if none is that low. The controller reports a level's output rounded up to a
whole dBm, so the report is never below the published output and a request of the reported
value selects the same level. `radio_tx_power_max_set()` (the DTM transmitter, the amateur
identifications) uses the PA's highest level, 127, which has no published output.

## 2. Register evidence

The EFR32xG22 reference manual does not document the radio's registers (docs/radio-tables.md
section 4.1); the register and field definitions below are Silicon Labs' public revB2 register
model (`build/reference-models/{MODEM,RAC}_S_{register,field}.py`), the level ranges and dBm
figures Silicon Labs' Zlib-licensed RAIL headers in `vendor/hal_silabs`.

* **[F] MODEM.RAMPLEV**, offset 0x0CC, reset 0x009F9F9F, writable mask 0x00FFFFFF: RAMPLEV0,
  RAMPLEV1 and RAMPLEV2, 8 bits each at bits 0, 8 and 16. **MODEM.RAMPCTRL**, offset 0x0C4,
  reset 0x555, mask 0xFF800FFF: RAMPRATE0..2 (4 bits each at bits 0, 4 and 8), RAMPDIS (bit 23),
  RAMPVAL (bits 31:24).
* **[F] The PA's levels**: 0 to 127 for the high-power ("HP", the xG22's 6 dBm) PA and 0 to 15
  for the low-power (0 dBm) PA on Series 2 configuration 2, the xG22
  (`simplicity_sdk/platform/radio/rail_lib/chip/efr32/efr32xg2x/rail_chip_specific.h`,
  `RAIL_TX_POWER_LEVEL_2P4_HP_MIN/MAX`, `_LP_MIN/MAX`).
* **[F] The PA's power-related RAC fields**: RAC.PACTRL (0x120) TX0DBMPOWER bits 3:0,
  TX0DBMSELSLICE 5:4, TX0DBMSLICERESET 8, TX0DBMLATCHBYPASS 9, TX6DBMPOWER 20:16,
  TX6DBMSELSLICE 23:21, TX6DBMSLICERESET 24, TX6DBMLATCHBYPASS 25, TX6DBMPREDRVREGBYPASS 26,
  TX6DBMPULLDOWNREG 27, TX6DBMREGBYPASS 28; RAC.PAENCTRL (0x0B4) PARAMP 8, INVRAMPCLK 16,
  PARAMPMODE 28; RAC.APC (0x0B8) ENAPCSW 2, AMPCONTROLLIMITSW 31:24.
* **[F] The fixed preset** the HAL writes at every `radio_tx_power_set()` (unchanged,
  `EFR32_RAC_STAGE2_*`, docs/hardware-bringup.md): PAENCTRL 0x10000000 is PARAMPMODE alone;
  PACTRL 0x10 is TX0DBMSELSLICE = 1 with **TX6DBMPOWER and TX6DBMSELSLICE 0**, while the 6 dBm
  PA transmits (the sequencer's `efr32_seq_pa_start` enables only its path, radio_seq.S). The
  6 dBm PA's power is therefore not held in PACTRL in this mode.
* **[F] What Silicon Labs' stack writes** (the stock image's RAIL PA configuration, read for
  understanding; nothing of it is copied): for the frequency-shift modulations it sets
  PAENCTRL.PARAMPMODE, programs the three RAMPRATE fields alike from the configured ramp time,
  and writes the power level into **all three RAMPLEV fields, offset by 32 for the 6 dBm PA** (16
  for the 0 dBm PA). For the amplitude modulations (MODEM.CTRL0.MODFORMAT BPSK, DBPSK or
  OOK/ASK) it writes RAMPLEV 0 and sets APC.ENAPCSW with the level in AMPCONTROLLIMITSW instead.
* **[I] Consistency**: the RAMPLEV reset value, 0x9F = 159 per field, is the 6 dBm PA's highest
  level, 127, plus 32. With nothing written (the product), the PA ramps to its highest level.

The HAL does what that establishes for FSK (`hal/radio.c`, `efr32_radio_pa_level_set()`):
`RAMPLEV = (level + 32) x 0x010101`, before the preset, while the radio is off (each event's
prepare). It leaves RAMPCTRL at its reset rates (stock computes them from a ramp time: not
reproduced, section 5) and the preset as it was.

**[U]** How the ramp value reaches the PA's slices and stripes (TX6DBMSELSLICE/TX6DBMPOWER have
3 + 5 bits, the ramp level 8) is not documented and not needed: the level is what Silicon Labs'
own tables are indexed by.

## 3. dBm per level

Published outputs: the "act_deci-dBm" column of Silicon Labs' xG22 6 dBm dBm-to-power-setting
table (`simplicity_sdk/platform/radio/rail_lib/plugin/sl_rail_util_pa_tables/efr32xg22/sl_rail_util_pa_dbm_powersetting_mapping_table_6dbm.h`),
every distinct level it uses. They are Silicon Labs' measurements on their own reference design,
not on the BGM220P explorer kit or the suit's BGM220PC22WGA: **all unverified here**. The
EFR32BG22 datasheet publishes no per-level figure, only a curve (Figure 4.9: about -27 dBm at
level 0, about 8.5 dBm at 127, flat above about 100); the module datasheet gives its maximum
(docs/radio-tables.md 7.89).

| PA level | RAMPLEV field | Published output (dBm) | Reported (dBm) | Status |
|---:|---:|---:|---:|---|
| 0 | 0x20 | -27.5 | -27 | unverified |
| 1 | 0x21 | -21.2 | -21 | unverified |
| 2 | 0x22 | -16.0 | -16 | unverified |
| 3 | 0x23 | -12.7 | -12 | unverified |
| 4 | 0x24 | -10.4 | -10 | unverified |
| 5 | 0x25 | -8.6 | -8 | unverified |
| 6 | 0x26 | -7.1 | -7 | unverified |
| 7 | 0x27 | -5.9 | -5 | unverified |
| 8 | 0x28 | -4.8 | -4 | unverified |
| 9 | 0x29 | -3.9 | -3 | unverified (unreachable by a whole-dBm request: -3 selects level 10) |
| 10 | 0x2A | -3.1 | -3 | unverified |
| 12 | 0x2C | -1.7 | -1 | unverified |
| 14 | 0x2E | -0.6 | 0 | unverified |
| 16 | 0x30 | 0.3 | 1 | unverified |
| 19 | 0x33 | 1.5 | 2 | unverified |
| 22 | 0x36 | 2.4 | 3 | unverified |
| 26 | 0x3A | 3.4 | 4 | unverified |
| 33 | 0x41 | 4.5 | 5 | unverified |
| 40 | 0x48 | 5.4 | 6 | unverified |
| 52 | 0x54 | 6.5 | 7 | unverified |
| 73 | 0x69 | 7.5 | 8 | unverified |
| 121 | 0x99 | 8.5 | 9 | unverified |
| 127 (reset, `radio_tx_power_max_set()`, product) | 0x9F | not published | - | unverified; BGM220P datasheet Table 4.5 POUTMAX 8.2 dBm typical for the 8 dBm module, and that it is this level's is [I]; SoC datasheet Figure 4.9 about 8.5 dBm, saturated; laptop RSSI equal to level 121's (7.89) |

## 4. Amateur operation (47 CFR 97.313)

`EFR32_RADIO_AMATEUR` selects `EFR32_RADIO_TX_POWER`. `efr32_amateur_tx_power_set(dbm)`
(include/hal/efr32/amateur.h) is the station's transmitter power: while amateur rules are in
force, every transmission from the radio's next event is capped at the level it selects, each
Bluetooth role's own level and the identifications alike (`efr32_tx_power_limit_set()`,
hal/tx_power.c); a role below it keeps its own level. It returns the selected output (section 3),
is kept across `efr32_amateur_disable()`/`enable()`, and is `EFR32_AMATEUR_TX_POWER_MAX` (no
ceiling: identifications at level 127) until set. 97.313(a)'s "minimum transmitter power
necessary" is thereby the operator's to set; the dBm are the unverified published outputs.
97.313(b)'s cap check is unchanged (`efr32_amateur_enable()` compares the module's datasheet
maximum with the administration's 1.5 kW PEP).

The Morse identification is on-off keyed (MODFORMAT OOK/ASK), for which Silicon Labs' stack sets
power through APC rather than RAMPLEV (section 2); this firmware writes RAMPLEV for it too, and
whether a lower RAMPLEV lowers the key-down power is **unverified** (section 5, item 4). The data
identification (BLE-ASI) is GFSK and follows RAMPLEV like any advertising PDU.

With amateur operation the roles' default becomes `BT_CTLR_TX_PWR` (0 dBm unless chosen); before,
every transmission used level 127.

## 5. Measurements needed (silicon, SDR or power meter)

1. **RAMPLEV on silicon** [F, done]: 0x009F9F9F after a system reset, and the section 3 field for
   each role's level and ceiling while `tests/tx-power-port` (`prj-hardware.conf`) runs on the
   explorer kit, 43/43 (docs/radio-tables.md 7.89).
2. **Output power per level**: each level of section 3 and level 127, advertising on channel 38
   with the kit at a fixed distance and a fixed SDR gain (relative) or into a power meter or
   spectrum analyzer through a coupler or the kit's RF test point if fitted (absolute). Pass:
   monotonic in the level, and within the datasheet's tolerance of the published figure at
   least at 0 dBm and the top. Repeat on the suit (BGM220PC22WGA, its supply). Relative sanity
   check done [F, 7.89]: laptop RSSI monotonic over levels 0-121 (-82.6 to -42.8 dBm, 39.8 dB
   against the table's 36.0), 127 equal to 121; the absolute figures still need an instrument.
3. **Ramp**: with RAMPCTRL at its reset rates, the PA's ramp-up and ramp-down at low and high
   levels (SDR envelope around TXEN and TXPD) and the MODEM.SEQIF.TXRAMPDONE time against the
   sequencer's bounded wait (radio_seq.S `efr32_seq_pa_stop`); the transmit spectrum mask
   (Core Vol 6 Part A 3.2.2) at the highest level. Measured without RF [F, 7.89]: TXRAMPDONE
   after PARAMP clear about 24 ns per RAMPLEV step (1.17 us at 0x3A, 3.46 us at 0x99), far
   inside the bounded wait; the frame does not wait for the ramp-up (TX to SYNCSENT 40.6-40.7 us
   at every level). The envelope and the mask still need an SDR.
4. **OOK**: the Morse identification's key-down power at a ceiling of 0 dBm and at no ceiling,
   to see whether RAMPLEV sets it; if not, APC.AMPCONTROLLIMITSW per section 2. Register
   readback [F, 7.89]: during the identification RAMPLEV holds the ceiling's level and APC reads
   0xFF000000 (ENAPCSW clear); the output itself is unmeasured.
5. **Reported against measured**: the HCI LE Read Transmit Power range and every selected level
   against item 2; replace "published" by "measured" in section 3 where they agree.

## 6. The product, QEMU and tests

* **Product**: `owo_suit` and `owo_suit -DOWO_HW=ON` build byte-identical images before and after
  (the option is off; `radio_tx_power_set()` and `radio_tx_power_max_set()` keep their code);
  `scripts/check-hardware-ram.sh` 30596 B <= 31000 B (master 918708d).
* **QEMU** (`vendor/qemu`): MODEM.RAMPCTRL and RAMPLEV now have the public reset values and
  masks (they were unmasked latches reading 0); qtest `modem-pa-ramp-registers`. The medium
  carries no transmit power, so a level changes nothing else in the model.
* **Host** (`tests/efr32-radio-contract/tx_power_test.c`): the table is ascending; the floors
  above; every whole dBm from -128 to 127 reports a value that selects the same level again and
  is not above the request; the ceiling caps the levels and the highest level, but not LE Read
  Transmit Power's range.
* **On target in QEMU and on the explorer kit** (`tests/tx-power-port`, `run-qemu.sh`;
  `prj-hardware.conf` on silicon; 43 checks, 43/43 on both): RAMPLEV 0x9F9F9F
  before any transmission; LE Read Transmit Power -27..9; the advertiser's +4 dBm default
  (RAMPLEV 0x3A3A3A), vendor Write Tx Power Level -10 (0x242424), -100 (selected -27, 0x202020)
  and no preference (back to +4) while advertising; the scanner's level kept; unknown connection
  and handle type refused; under amateur rules with a 0 dBm ceiling the +4 dBm advertiser at
  0x2E2E2E, a -10 dBm advertiser at its own 0x242424, the closing Morse identification at the
  ceiling (0x2E2E2E); again at a -27 dBm ceiling, the advertiser and a second identification at
  0x202020; Part 15 again, +9 dBm at 0x999999. Under amateur rules every channel is on channel
  37's 2402 MHz carrier, so on silicon the identifications stay there. The connection paths (per-connection
  level, Read Transmit Power Level) are the same code but not run: no connection in this test.
* `tests/amateur-port/run-qemu-amateur-data-e2e.sh` (`MODE=short`) passes with the option
  selected.
