# EFR32BG22 radio: register/table knowledge needed by the Zephyr LL port

**Unvalidated on hardware.** Labels: **[F]** = fact from a cited public source or directly observed in a stock-firmware trace in the QEMU
model; **[I]** = inference, not confirmed. No blob bytes or disassembly are reproduced here. The QEMU radio model is checked
against these findings in `docs/model-fidelity.md`.

## 1. Block map
| Block | Base | Basis |
|---|---|---|
| PRORTC | 0xA8000000 | [I] literal pools in RAIL objects (`rfhal_*`) |
| FRC | 0xA8004000 [S] / 0xB8004000 [NS] | [F] public xG22 configurator table address encoding and labels; generated `FRC_S.py` / `FRC_NS.py` |
| AGC | 0xA800C000 [S] / 0xB800C000 [NS] | [F] same |
| RFCRC | 0xA8010000 [S] / 0xB8010000 [NS] | [F] same |
| MODEM | 0xA8014000 [S] / 0xB8014000 [NS] | [F] same |
| SYNTH | 0xA8018000 [S] / 0xB8018000 [NS] | [F] same |
| PROTIMER | 0xA801C000 | [F] RAIL archive object access; see §4.3 |
| RAC | 0xA8020000 [S] / 0xB8020000 [NS] | [F] public table plus stock disassembly literals; generated `RAC_S.py` / `RAC_NS.py` |
| RF mailbox | 0xA8028000/0xA802C000 [S], 0xB8028000/0xB802C000 [NS] | [F] xG22 RAIL archive and stock IRQ access; see §1.1 |
| BUFC | 0xAA000000 | [F] layout matches the xG21 public header; matched by stock init trace |
| (unclaimed) 0xA8005000 | 0xA8005000 | [I] directly disassembled stock sequencer code touches `0xA8005064`/`+0x1000` SET alias `0xA8006064`; no public register model located yet; see §7.2 |

The public rev-B2 models establish a narrower, usable fact: FRC, AGC, RFCRC, MODEM, SYNTH, and RAC each have writable one-bit `EN.EN` at `+0x004` (their respective `*_S_register.py:24-37` and `*_S_field.py:17-24` files). `radio_setup()` enables exactly those six blocks, and QEMU masks the matching latches to bit zero with a native regression. Separately, the stock helper writes `1` at secure BUFC and PROTIMER `+0x004` at `flash-stock.bin` offsets `0x26de4` and `0x26dfc` (target literals `0xAA000004` and `0xA801C004` at `0x2723c` and `0x2724c`); the HAL mirrors those direct writes through the established non-secure aliases because it uses both blocks. This is not a claim that either bit is named `EN`, nor does it establish ordering, reset sequencing, or interrupt policy. No conclusion is drawn for PRORTC. IRQ lines (AGC 31 ... SYNTH 42) are [F] from the device header
(`gecko_sdk/platform/Device/SiliconLabs/EFR32BG22/Include/efr32bg22c224f512im40.h`).

### 1.1 RF mailbox endpoint contract [F]

The archive identifies two mailbox endpoint windows at non-secure
`0xB802C000` and `0xB8028000`. Both expose data at `+0x004`, flags/status at
`+0x040`, and enable at `+0x044`. In `rfhal_mailbox.o` archive
`0x4e4c0..0x4ecb0`, the `C`-endpoint producer waits for bit 1 of
`0xB802C040` to clear, writes packed `arg0 | arg1<<8` to `0xB802C004`, then
writes bit 1 through the `+0x1000` alias (`0xB802D040`), at
`0x4e4f4..0x4e51c`. Its poller reads `0xB802C004`, requires the upper byte
clear, and compares the low byte to its expected result (`0x4e51c..0x4e554`).
The service path waits on bit 1 at `0xB8028040`, acknowledges it through
`0xB802A040`, then consumes the low byte at `0xB8028004`
(`0x4e56c..0x4e59c`). `generic_phy.o:RDMAILBOX_IRQHandler` independently
intersects the latter status/enable pair, clears active bit 1 through the
alias, and consumes byte zero (archive `0x52fdc..0x53048`). This proves an
eight-bit command/response-style exchange and doorbell/ack convention. It
does not establish endpoint cross-routing, RDMAILBOX assertion timing, or
normal RX descriptor production. Stock does use the secure B endpoint in
`RDMAILBOX_IRQHandler`: it loads `0xA8028000` from the literal at
`flash-stock.bin:0x28dd0`, reads its `+0x040`/`+0x044` status-enable pair,
and acknowledges its masked result through the B-endpoint clear alias
`0xA802A040` (literal at `0x28dd4`; instructions `0x28c10..0x28c1a`). The
later literal `0xA802005c` at `0x28dd8` is read only after the bit-1 service
path and tested for bit 5 (`0x28c40..0x28c5e`); its block/meaning is not
inferred here. This establishes stock mailbox servicing, not a packet path:
the bounded handler analysis has not shown a buffer-2 `WRITEDATA` write or a
descriptor record producer.

The stock bit-1 branch further bounds this endpoint's observed role. It reads
the low byte of B `DATA + 0x004`, acts only when that byte is `1`
(`0x28c2a..0x28c38`), samples helper `0x29c60`, then polls bit 5 of
`0xA802005c` while taking a second sample and passing the pair to the
wrapped-difference helper `0x29a74` (`0x28c38..0x28c5e`). The visible handler
path contains no BUFC address, descriptor-ring access, or PDU copy. This is
consistent with a mailbox-assisted timing/control exchange, but does not
identify the helpers' semantic clock or prove a full endpoint protocol.

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

The BLE 1M delta begins with `0x03014FFC, &phyInfo_0`, labelled `SEQ.PHYINFO` by the public table (`ble_efr32xg22_configurator_out.c:474-476`). The generated rev-B2 model locates that field at `0xB0004FE0 + 0x1C = 0xB0004FFC` and defines it as a full-width address (`SEQ_register.py:130-143`, `SEQ_field.py:127-134`). `phyInfo_0` itself is a 20-word configuration object containing several source-language pointers (`ble_efr32xg22_configurator_out.c:87-110`), so its source address is not portable into the open image. The native QEMU machine aliases `0xB0000000..0xB0007FFF` to sequencer/RFC RAM, which naturally retains the pointer word; no duplicate MMIO model is appropriate. The open HAL must not copy the vendor pointer record until an address-independent PHYINFO representation and sequencer consumption contract are established.

The OpenISA call order makes the public BLE baseline's dynamic defaults safe to replace, rather than a reason to omit the whole table: both advertising and scanning call `radio_phy_set()` before `radio_pkt_configure()`, then write their access address and CRC immediately afterwards (`lll_adv.c:352-361`; `lll_scan.c:159-172`). In particular, the base table's default `MODEM.SYNC0`, `RFCRC.INIT`, and `RFCRC.POLY` cannot survive those later API calls. Whitening is also deliberately not entrusted to the table: the LLL applies `radio_whiten_iv_set(chan)` after its channel selection (`lll.c:445`). `hal/radio.c` now imports the exact public base (137 words) plus 1M, 2M, and coded deltas (85 words each) through a readable decoder, omitting only each `SEQ.PHYINFO` record; a mechanical comparison against the source arrays verifies every retained word. OpenISA defines `PHY_FLAGS_S2 = 0` and `PHY_FLAGS_S8 = BIT(0)` (`pdu.h:282-288,314-318`), so the importer maps coded S2 to the public `ble_LR_DSA_500kb_modemConfig` table (`configurator_out.c:663-724`) and S8 to `ble_LR_DSA_125kb_modemConfig` (`:600-661`); the associated public profile names identify their 500/125 kb/s roles (`configurator_out.h:68-76`). This is a source-backed configuration step, not a claim that the table alone configures packet transmission or sequencer behavior.

## 3. Per-function blockers (IDs used in `modules/efr32-radio/hal/radio.c`)
| ID | Needs | Status | Functions |
|---|---|---|---|
| T1 | block enables/reset, which IRQ ends an event | **[F] six core EN latches implemented; stock's raw BUFC/PROTIMER `+0x004 = 1` setup writes are mirrored without assigning bit semantics; RAC has separate RSM and SEQ flag/enable banks; reset and event policy remain unknown. OpenISA-compatible sleep/wake availability accounting is software-only.** | `radio_setup/reset/sleep/wake/is_off`, `LL_RADIO_IRQn` |
| T2 | RAC command + state/status registers | **[F] core command/state map known; sequencer behavior still abstract** | `radio_rx_enable`, `radio_tx_enable`, `radio_disable`, `radio_is_*`, `radio_status_reset`, TIFS switch |
| T3 | PHY table apply (MODEM/FRC/AGC/SYNTH data) | **[F] format decoded; address-independent BLE base plus 1M/2M/coded S2/S8 deltas imported; PHYINFO remains unresolved** | `radio_phy_set` |
| T4 | SYNTH channel/frequency registers | **[F] advertising frequencies implemented; a complete working `RAIL_StartTx` trace proves the physical 2 MHz index is written to `FRC.WCNTCMP2` and `SYNTH.CHSP` while `SYNTH.CHCTRL` remains zero; the HAL and QEMU medium now use that contract** | `radio_freq_chan_set` |
| T5 | FRC descriptors: sync word, CRC poly/init, whitening, length/header | **[F] RFCRC and BLE whitening seed implemented; `radio_pkt_configure()` clears the named FCD0..3 `SKIPWHITE` field and hardware readback matches stock for that field; remaining length/header and sequencer handoff behavior is incomplete. §7 adds a static decomp of the stock IRQ8/IRQ9 handlers and a hardware-observed `FRC.STATUS.ACTIVETXFCD` divergence that sharpens, but does not yet close, the open fault-boundary question from `docs/hardware-bringup.md`.** | `radio_aa_set`, `radio_crc_configure`, `radio_whiten_iv_set`, `radio_pkt_configure`, `radio_crc_is_valid` |
| T6 | BUFC RX/TX buffer setup | **[F] TX channel-0 and RX channel-1 staging setup implemented; a dormant normalized-PDU RX copy helper enforces controller bounds, but FRC/BUFC handoff remains incomplete** | `radio_pkt_rx_set`, `radio_pkt_tx_set` |
| T7 | PA power registers | **[F] the 6 dBm PA's level is MODEM.RAMPLEV (level + 32 in each field; reset = level 127); `EFR32_RADIO_TX_POWER` maps dBm to it by Silicon Labs' published outputs, unmeasured on these boards (docs/tx-power.md). The fixed RAC preset stays; without the option RAMPLEV keeps its reset value** | `radio_tx_power_*` |
| T8 | PROTIMER timebase/compare/capture, 32 kHz RTC sync | **[F] base, command offset, counter/period and compare-bank access observed; field semantics incomplete** | `radio_tmr_*`, `radio_tmr_tifs_*` |
| T9 | AGC RSSI read | **[F] live/frame value registers and fixed-point fields known; readiness/measurement policy unknown** | `radio_rssi_*` |

Software-only: filtering and bit-counter hooks now deliberately match the
OpenISA reference's no-op/no-match implementation (`hal/RV32M1/radio/radio.c:971-1012`);
gpio, ccm, and address resolution retain their separate implementation status.
Timing constants in `lll/lll_vendor.h` and the `*_delay_get` functions are OpenISA placeholders [I], to be measured once T2/T8 exist.

### 3.1 OpenISA clock-availability contract [F]

The target module is physically marked `BGM220PC22HNA2` (user-provided board
photograph, inspected 2026-10-20). Silicon Labs' [BGM220P data
sheet](https://www.silabs.com/documents/public/data-sheets/bgm220p-datasheet.pdf),
Figure 1.1 / page 1, independently identifies that ordering code as the
512 kB-flash / 32 kB-RAM EFR32BG22 module and assigns a *lowest operational
energy mode* to each block. The BLE radio chain—RX/TX front end, synthesizer,
DEMOD, IFADC, AGC, MOD, CRC, FRC, BUFC RAM, and the M0+ radio controller—is
EM1 (Sleep) minimum. RFSENSE with OOK detect is the distinct radio-adjacent
EM4 (Shutoff) exception. This constrains future QEMU power modeling: EM2/EM3
must not retain a usable BLE radio chain, while EM4 RFSENSE cannot be
misrepresented as normal packet reception. It does not identify the EMU/CMU
register transition sequence, so neither the HAL nor QEMU writes a guessed
power-control register on its basis.

The portable OpenISA reference does not use `radio_is_off()` as a RAC state
query. Its `radio_sleep()` and `radio_wake()` maintain a `dsm_ref` reference
count, and `radio_is_off()` reads the RV32M1 DSM power-state bits
(`hal/RV32M1/radio/radio.c:80-85,1529-1619`). `lll_clk_on_wait()` calls
`radio_wake()` and waits while `radio_is_off()`, while `lll_clk_off()` calls
`radio_sleep()` (`lll/lll.c:345-382`). Thus this port needs a matched
availability contract even before EFR32 power sequencing is known.

The xG22 RAIL archive exports `RAIL_Wake` (24 bytes), `RAIL_Idle` (10 bytes),
and `RAIL_Sleep` (56 bytes) from `rail_singleprotocol.o`: their sections begin
at archive file offsets `0x149266`, `0x1492c4`, and `0x149280`, respectively
(member data starts at `0x148fdc`; section-relative object offsets are
`0x28a`, `0x2e8`, and `0x2a4`). Direct disassembly shows each is only a
wrapper: Wake calls a validation helper then tail-calls another helper, Sleep
calls validation then a helper with a stack byte, and Idle tail-calls a helper.
None contains a peripheral-address literal or MMIO access. Those wrappers do
not identify the underlying RAC, oscillator, or power-manager sequence. Accordingly,
`hal/radio.c` uses an atomic software reference count: setup begins awake,
sleep decrements it and reports `-EALREADY` on an unmatched decrement, wake
increments it, and off means a zero count. This is deliberately immediate and
does not write a guessed EFR32 power register or equate RAC `STATUS.STATE=OFF`
with a powered-down radio. Hardware power sequencing remains T1/T2 work.

Following one level deeper still does not provide that sequencing contract.
Wake reaches a 84-byte helper at archive file offset `0x138b8` and Sleep a
144-byte helper at `0x13948`, both in `rail_rf_hal.o` (member data offset
`0x10dd4`; object section offsets `0x2ae4` and `0x2b74`). The former calls
further helpers and has literals for `0xB801C000` and `0xB802C000`; the latter
uses a core critical section, calls further helpers, and has literals for
`0xE000E100` and `0xB801C000`. `RAIL_Idle` reaches a 304-byte helper at
`0x11758` (section offset `0x984`) which calls several additional helpers and
accesses FRC/RAC/PROTIMER-area literals. These are concrete call-graph and
access facts, but without resolving the called routines or their state inputs,
they are not evidence for an EFR32 sleep/wake register sequence.

Subsequent archive resolution supplies a narrow, useful part of that sequence.
The Wake helper calls `rfhal_trustzone.o` at archive offset `0x496a8`, which
writes `0x20000000` to `0xE000E100` (NVIC ISER0 bit 29, public
`EMUDG_IRQn`), and PROTIMER helpers at `rfhal_protimer.o` offsets `0x3c5c8`,
`0x3c314`, and `0x3c050`; the last clears pending IRQ36 at `0xE000E284`,
enables it at `0xE000E104`, then writes `1` to non-secure PROTIMER CMD
`0xB801C00C`. The Sleep helper symmetrically reaches `rfhal_trustzone.o`
`0x496b8` (NVIC ICER0 bit 29 at `0xE000E180`) and `rfhal_protimer.o`
`0x3c06c` (clear pending IRQ36, disable it at `0xE000E184`, DSB/ISB, then
write `4` to the same PROTIMER CMD address). These are IRQ and PROTIMER
gating facts only: they do not establish RAC or RF power state.

The Idle path now has similarly bounded RAC evidence. `RAIL_Idle` in
`rail_singleprotocol.o` at archive offset `0x1492c4` reaches a 304-byte
`rail_rf_hal.o` helper at `0x11758`. Its mode tables at `0x145f0`, `0x145ec`,
and `0x145e8` map the public `RAIL_IDLE`, `RAIL_IDLE_ABORT`,
`RAIL_IDLE_FORCE_SHUTDOWN`, and `RAIL_IDLE_FORCE_SHUTDOWN_CLEAR_FLAGS` modes
to internal boolean tuples `(0,0,0)`, `(1,0,0)`, `(1,1,0)`, and `(1,1,1)`.
The reached `generic_phy.o` helper at `0x52524` writes `0x100` to
`0xB8020010` (`RAC.CMD.RXDIS`). For the nonzero first tuple component (Abort
and both Force Shutdown modes), its callee at `0x52470` also writes `0x20` to
the same CMD register (`RAC.CMD.TXDIS`). The preceding helper reaches
`RAC.CTRL.SET.FORCEDISABLE` for the two Force Shutdown modes; only when that
CTRL bit is set does `0x52524` poll `RAC.STATUS.STATE` at `0xB802000C` until
OFF. This directly supports QEMU's distinct RXDIS and TXDIS command
transitions; it does not prove that every Idle invocation forces OFF, or
establish the rest of the radio shutdown sequence.

### 3.2 TX-power boundary [F]

The available RAIL evidence rules out treating OpenISA's power argument as a
portable PA register value. `RAIL_SetTxPower` is a 34-byte wrapper in
`rail_tx.o`, at archive file offset `0x156488` (member data starts at
`0x1562e8`, section-relative object offset `0x1a0`). It dispatches to
`sli_rail_63a89db964f651a01db7d72312883c55`, whose 168-byte section is at
archive file offset `0x123e4` in `rail_rf_hal.o` (member data starts at
`0x10dd4`, section-relative offset `0x1610`). That helper saves the requested
raw setting in RAIL state, selects another helper based on its state/config
inputs, and can return failure; it is not a direct MMIO wrapper.

`RAIL_SetTxPowerDbm` is similarly a 50-byte wrapper at archive file offset
`0x1564ae`. Its 92-byte target,
`sli_rail_984b4b7d4bcd6e72d28d953fbc3f5c82`, begins at `0x1248c` in
`rail_rf_hal.o` (section-relative offset `0x16b8`). It either calls the
application conversion callback
`sl_railcb_convert_ddbm_to_power_setting_entry`, or obtains a TX-power
configuration and calls `RAIL_ConvertDbmToRaw` before entering the raw-setting
path. These direct call-graph facts demonstrate that dBm conversion depends on
the configured PA path and may be application-supplied. They do not identify a
safe xG22 PA register, range, board matching network, or calibration value.

Consequently `radio_tx_power_set()` and `radio_tx_power_max_set()` remain
explicitly unsupported: the HAL must not turn a controller power value into an
uncalibrated PA write. A future implementation needs a public board-specific
power table or independently validated calibration/configuration contract, and
QEMU should only model the resulting registers once that contract establishes
their externally visible behavior.

The public xG22 sources reinforce that boundary rather than supplying the
missing decoder. `sl_rail_pa_power_setting_t` is explicitly an abstract
32-bit setting decoded into one or more hardware register values
(`sl_rail_types.h:2433`), while `sl_rail_tx_power_config_t` exposes PA mode,
supply voltage, and ramp time but no register location
(`sl_rail_types.h:2658-2677`). The 0 dBm and 6 dBm xG22 mapping tables contain
opaque setting values (`sl_rail_util_pa_dbm_powersetting_mapping_table_0dbm.h:41-46`
and `sl_rail_util_pa_dbm_powersetting_mapping_table_6dbm.h:41-46`), and the
public conversion code returns one of those settings without performing a
visible MMIO write (`pa_conversions_efr32.c:260-310`). Thus even a board-matched
public PA table is not a documented PA-register decoder; it is not evidence
for adding a guessed HAL write or QEMU register model.

**Update (docs/tx-power.md).** The decoder this section looked for is not an MMIO write in the
conversion code: the power setting is the PA's level, and the level is what the radio's PA ramp
is programmed with, MODEM.RAMPLEV (public register model: three 8-bit fields, reset 0x9F9F9F).
The stock RAIL's PA configuration writes the 6 dBm PA's level plus 32 into all three fields for
FSK; 0x9F is its highest level, 127, plus 32. `EFR32_RADIO_TX_POWER` writes it; the dBm are
Silicon Labs' published outputs, unverified here, with the measurements listed there.

### 3.3 Initialization does not recover reset sequencing [F]

`RAIL_Init` is an 136-byte wrapper at archive file offset `0x149144` in
`rail_singleprotocol.o` (member data offset `0x148fdc`, object section offset
`0x168`). It allocates and publishes a RAIL handle, invokes the weak frame-type
callback and RX-FIFO setup callback, then calls
`sli_rail_7e61d0565567ab0020244c3cf3f002a7`. That 118-byte RF helper begins
at `0x14e1a8` in `rail_rf.o` (member data offset `0x14e008`, object section
offset `0x1a0`). It calls further helpers and conditionally records their
status in the RAIL handle; it contains neither a peripheral-address literal
nor an MMIO instruction. These wrappers establish the call graph but do not
identify an xG22 block-reset order, enable order, or interrupt clear policy.
Its first target is `sli_rail_f70cad743ef3526a2ef7ac090e6c385a`, a 188-byte
helper at `0x13880` in `rail_rf_hal.o` (member data offset `0x10dd4`, object
section offset `0x27ac`). Unlike the wrappers, it has concrete writes: it
writes `0x60` to non-secure `FRC.RXCTRL` (`0xB8004000 + 0x04c`) and clears
`0x40` through the non-secure `RAC.SR0` clear alias
(`0xB8022000 + 0x040`). The generated public models name and permit both
register accesses. `radio_setup()` performs exactly those two writes after
enabling the six public radio blocks, and QEMU's focused regression retains
their latch/alias behavior. Their field effects remain uncharacterized, so
they do not establish a complete reset protocol; `radio_reset()` remains
intentionally unimplemented rather than duplicating setup and misrepresenting
it as hardware reset behavior.

## 4. BUFC [F]
The per-buffer field order matches the public xG21 header, but the xG22 trace establishes a different layout: four buffers begin at `+0x00C`, use a `0x40` stride, and have `THRESHOLDCTRL`/`CMD` at buffer-relative `+0x24`/`+0x28`. Global `IF`/`IEN` are at `+0x114`/`+0x118`. For example, stock configures buffer 3 with `CTRL +0x0CC`, `ADDR +0x0D0`, `THRESHOLDCTRL +0x0F0`, then `CMD +0x0F4`; the other three sequences occur at bases `+0x00C`, `+0x04C`, and `+0x08C`. The compatible public field definitions give `CTRL` mask `0x7`, read/write offset masks `0x1FFF`, `THRESHOLDCTRL` mask `0x2FFF`, and `CMD` mask `0xF` (`efr32bg21_bufc.h:124-245`); QEMU applies and regresses those masks at the trace-established xG22 offsets. `ADDR` deliberately remains a full-width latch because the retained xG22 trace uses full SRAM addresses, which the older header's 24-bit encoding does not explain. A future trace-backed native BUFC DMA model can read `BUF.ADDR`; no BUFC DMA trigger or transfer semantics are inferred or modelled yet. Source: the public header and the stock 7.9.07 QEMU trace.

The official xG22 reference manual independently establishes the high-level
data direction missing from the compatible header: after frame synchronization,
the FRC moves receive data into buffer storage, while transmission moves the
post-preamble/post-sync data from buffer storage to the modulator
([EFR32xG22 Reference Manual, §5.1.5, p. 46](https://www.silabs.com/documents/public/reference-manuals/efr32xg22-rm.pdf)).
This supports modeling the known channel-1 receive staging and channel-0
transmit staging as real hardware-backed buffers. It does **not** identify a
buffer number for either path, specify the FRC/BUFC trigger or byte boundary,
describe the channel-2 descriptor ring, or establish CRC/trailer/completion
ordering. QEMU therefore keeps its TXEN snapshot and synthetic RX injection as
explicit simulator boundaries rather than presenting them as documented silicon
timing.

The stock trace gives one tighter ordering fact for buffer 3: at `radio-trace.log:283-287` (virtual time 99,318,096 ns), it writes `BUFC+0x0f4 = 1`, reads `BUFC+0x0d0 = 0x20002f50` and `+0x0d4 = 0`, writes global `BUFC+0x100 = 0`, then writes `BUFC+0x0f4 = 4`. The same five-access sequence repeats at `radio-trace.log:293-297` (99,320,816 ns). This demonstrates that firmware consumes the programmed address around these command values, but it does **not** establish which command starts DMA, direction, byte count, or completion condition; the QEMU model therefore still does not perform a DMA transfer.

The public BG21 header (`platform/Device/SiliconLabs/EFR32BG21/Include/efr32bg21_bufc.h:224-243`) gives the compatible `BUF.CMD` mask `0x0f`, with bit 0 named `CLEAR` and bit 1 named `PREFETCH`. The QEMU model retains that masked register value and implements only `CLEAR`, which agrees with the trace's first command. Bit 2 (`0x4`) is within the mask but unnamed in that header; it is retained for software visibility with no invented side effect.

The RAIL archive narrows the `CMD=4` sequence without naming that bit. In `librail_efr32xg22_gcc_release.a(rfhal_bufc.o)`, section `.text.sli_rail_adf0dd5a9b9483ac88bd66365969a5a1` starts at object file offset `0x3a6`. At file offsets `0x3cc..0x3e2` it writes `CMD=1`, then buffer-3 `CTRL`, `ADDR`, and `WRITEOFFSET`; at `0x3de` it writes `CMD=4`; and, only when the write offset is nonzero, at `0x3ea` it writes public `PREFETCH` (`CMD=2`). This corroborates the trace ordering and shows `CMD=4` is part of buffer setup, but does not assign it a name or establish DMA/packet behavior, so QEMU still gives bit 2 no side effect.

The same public header defines identical global `IF` and `IEN` masks, `0x8F0F0F0F` (`efr32bg21_bufc.h:247-338`): four sparse four-bit buffer groups at bit positions `0..3`, `8..11`, `16..19`, and `24..27`, plus bus error at bit 31. Within each group, bit 0 is overflow, bit 1 underflow, bit 2 threshold, and bit 3 corrupt (`efr32bg21_bufc.h:250-329`). Its threshold flag/enable bits are therefore at `2`, `10`, `18`, and `26`, rather than a compact four-bit bitmap. This is corroborated by the stock write `BUFC.IEN = 0x00050000` at `radio-trace.log:52-53`, which enables buffer-2 overflow and threshold positions. QEMU masks both latches to that source-backed value, uses the public threshold positions, and latches the channel-overflow bit when pointer-derived occupancy exceeds its configured capacity; its regression covers a 64-byte buffer's 65th port write. Underflow, corruption, and any RAIL packet/error recovery policy remain unmodelled.

The downloaded stock sequencer-RAM capture adds a distinct programmed-I/O fact. The untracked container `scratch-radio.tgz` has SHA-256 `5054d2b46b9315238f5864c5f0f931fb0759bb374d7d41257038842da827cd86`; its sole `radioram.dump` member begins at tar offset `0x200` and has SHA-256 `acd72f8ebb4008e918af059b1ba2383dc64b33c66690871f4343a5bfed100847`. Thumb code at RAM offset `0x0ce4` (`0xA0000CE4`) loads the literal at member offset `0x0e30`, `0xA8004090` (`FRC.RXRAWDATA`), then loads the `0xAA000064` literal at member offset `0x0e34` (secure buffer 1 `WRITEDATA`). The instruction range `0x0d50..0x0d6c` makes four 32-bit stores to that port: `0x0d5a` writes `RXRAWDATA[15:0]` after `UXTH`, and `0x0d60`, `0x0d66`, and `0x0d6a` write separately extracted bits `15:8`, `23:16`, and `31:24`. These are word-wide bus transactions, but compatible public BUFC fields `READDATA`, `WRITEDATA`, and `XWRITE` are all low-octet `0x000000ff` values (`efr32bg21_bufc.h:170-192`); each store therefore contributes one low-octet FIFO lane, yielding raw bytes 0 through 3 in order. The public revB2 model identifies the source as read-only `FRC_S.RXRAWDATA` (`FRC_S_register.py:871-885`); the trace-established xG22 layout identifies the destination as `0xAA000000 + 0x04c + 0x018`. This proves that the captured sequencer feeds that port with programmed I/O from FRC raw RX data; it does not establish packet framing, FIFO ownership, byte count, or DMA behavior. Hardware additionally proves that these aliases are initiator-dependent: the private M0+ can use A/AA and HardFaults on its first B-window RAC store, while Zephyr's M33 uses B/BA after assigning `AHBRADIO` non-secure. Both views nevertheless reach shared radio/FIFO state. QEMU currently aliases secure and non-secure BUFC windows unconditionally; that is a known fidelity gap rather than evidence that either hardware core may use both. Its data-port regression verifies a high-bit 32-bit write, the secure-buffer-1/non-secure-alias read, and the resulting guest-RAM byte. It also makes the source register read-only to guest writes, matching the public model; a future internal receive producer may supply it.

The capture's SHA-256 is `acd72f8e…ed100847`, and the same FRC literal occurs
in the shipped sequencer image: `generic_efr32xg22_release_seq.o`, member
data `0x13f048`, contains it at archive file offset `0x140ab4` (the large
`.rodata.sli_rail_6c7715…` section begins at relative `0x980`; image-relative
offset `0x10ec`). Thus the PIO sequence is evidence from the shipped image,
not an ad hoc RAM program. Conversely, M33 `generic_phy.o:FRC_IRQHandler`
starts at archive offset `0x52758` (length `0x7e4`): it snapshots and
acknowledges enabled FRC `IF/IEN` (`0xB8004068/0xB800406c`) and BUFC
`IF/IEN` (`0xBA000114/0xBA000118`) before dispatch, but has neither literal.
The sequencer is therefore the observed raw-byte producer, and the M33 handler
is downstream bookkeeping. Neither establishes packet records, metadata,
`TRAILRXDATA.CRCOK`, or their ordering with RXDONE.

The 32 KiB capture also provides a bounded literal-search result for the
descriptor question. Its only little-endian `0xAA0000xx` BUFC literals are at
capture offsets `0x0dec` (`0xAA000054`), `0x0e34` (`0xAA000064`), `0x15b0`
(`0xAA0000e4`), `0x1624` (`0xAA00000c`), `0x1858` (`0xAA0000d8`), and
`0x328c` (`0xAA000018`). They target channel 0, channel 1, or channel 3;
there is no channel-2 literal in the capture (whose SHA-256 is
`acd72f8ebb4008e918af059b1ba2383dc64b33c66690871f4343a5bfed100847`).
This is not only a literal-search result. The sole computed BUFC-base sequence
in the captured image is at dump offsets `0x1310..0x131e`: it forms
`0xAA00000C + (r5 << 6)`, reads `[computed + 0x20]`, and writes that value to
another peripheral. It does not form or write buffer 2's `WRITEDATA` address
(`0xAA0000A4`). A focused instruction scan also finds no computed
`BUFC + 0xA4` port address or channel-2 descriptor store. This is static-code
evidence only, not evidence that any listed access executes; it rules out a
plainly encoded normal sequencer descriptor producer from this captured image,
but not hardware/opaque-runtime production.
It also bounds the capture's TX evidence: its sole channel-0 literal,
`0xAA000018` at member offset `0x328c`, is `READOFFSET`; there is no static
channel-0 `ADDR`, `WRITEOFFSET`, or `WRITEDATA` literal. The nearby
`0xA8004088` literal at `0x1164` is FRC `AUXDATA`, not a TX FIFO port. Thus
the capture does not establish an FRC/BUFC TX consumer or framing route; the
QEMU TXEN snapshot remains explicitly synthetic.

One branch of that downstream bookkeeping is now exactly located.  With BUFC
threshold bit 18 pending, `generic_phy.o:FRC_IRQHandler` calls
`sli_rail_0906929bba6a17fd8a9605e65064a9f4` at handler-relative `0x240`
(archive offset `0x52998`).  That 108-byte routine is in
`rfhal_bufc.o`, archive range `[0x3398c, 0x339f8)` (member starts
`0x332f4`, ELF section file offset `0x698`).  It derives an eight-byte-aligned
entry from active-handle fields `+0x18`, `+0x38`, `+0x3a`, and `+0x3c`, advances
the 16-bit `+0x3c` value by eight, writes it to BUFC `+0x0b0`, acknowledges
BUFC bit 18 through `0xBA002114`, and writes one to `0xE000E284` (NVIC
`ICPR1`, clearing pending IRQ 32--not enabling it).  It then retains the entry
pointer in its private BSS slot.  At handler-relative `0x68a` (archive
`0x52de2`), the 36-byte companion
`sli_rail_31258cb71f8a6b9069347d576bc679f0` at `[0x339f8, 0x33a1c)` consumes
that one retained pointer through a local helper and clears the slot.  This
directly establishes deferred queue-entry hand-off and release/advance around
the BUFC threshold route; it does not identify that entry as a BLE PDU or
define its framing, CRC, metadata, or RXDONE ordering.

The same public header defines every buffer's local `STATUS.THRESHOLDFLAG` at bit 20 (`efr32bg21_bufc.h:194-205`), while the corresponding global threshold flags remain sparse at `8*n+2`. QEMU derives that local status bit from the matching sparse global flag; its regression test uses buffer 1 specifically, so this cannot silently regress to a compact global-flag assumption.

## 4.1 RAC core [F]
The public Simplicity SDK generated revB2 register model
(`platform/radio/efr32_multiphy_configurator/host_py_rm_studio_internal/full/efr32xg22/revB2/RAC_S_{register,field}.py`)
matches the stock disassembly and trace exactly. At `0xA8020000`: `EN +0x004`, `RXENSRCEN +0x008`, read-only `STATUS +0x00C`, write-only `CMD +0x010`, `CTRL +0x014`, `FORCESTATE +0x018`, `IF/IEN +0x01C/+0x020`, and `SEQIF/SEQIEN +0x028/+0x02C`. SET/CLR/TGL aliases are at `+0x1000/+0x2000/+0x3000`.

QEMU masks the native writable RAC latches to the public revB2 definitions: `RXENSRCEN=0x00003FFF`, `CTRL=0x1F0107EF`, `FORCESTATE=0x0000000F`, `IF/IEN=0x00FF000F`, and `SEQIF/SEQIEN=0x3FFF000F`; it also masks write-only `CMD` to `0xC000FDFF` before interpreting modeled commands. Source: `RAC_S_register.py:40-120,152-190,198-355`. This preserves documented software-visible fields without assigning meaning to unmodeled commands.

The read-only `IPVERSION` identities at each radio-block base are also
modelled: FRC, AGC, MODEM, SYNTH, and RAC reset to `1`, while RFCRC resets to
`0`. QEMU returns those values through both secure and non-secure radio views
and ignores writes. These are register-identification values only, with no
radio behavior attached; source: the corresponding public revB2
`{FRC,AGC,RFCRC,MODEM,SYNTH,RAC}_S_register.py:8-18` models.

`CMD` bit 0 is `TXEN`, bit 1 `FORCETX`, bit 5 `TXDIS` (`0x20`), bit 7 `RXCAL` (`0x80`), and bit 8 `RXDIS` (`0x100`). `RXENSRCEN.SWRXEN` occupies bits 0..7. `STATUS.STATE` is bits 24..27; the matching sequencer flags establish OFF=0, RXWARM=1, RXSEARCH=2, RXFRAME=3, RXPD=4, RX2RX=5, RXOVERFLOW=6, RX2TX=7, TXWARM=8, TX=9, TXPD=10, TX2RX=11, TX2TX=12, SHUTDOWN=13. `STATUS` also exposes `SEQACTIVE`, `DEMODENS`, `TXENS`, and `RXENS` in bits 28..31.

The stock Series-2 installer has a `RAC_SEQ` route: xG22 devicetree assigns `rac_seq` external IRQ 38 (`dts/arm/silabs/xg22/efr32xg22.dtsi:15-18`), and the generic installer connects it to `RAC_SEQ_IRQHandler` (`soc/silabs/silabs_s2/soc_radio.c:30`). That is not, by itself, an appropriate completion route for this port: the device header defines the separate direct `FRC_IRQn = 34` and `RAC_SEQ_IRQn = 38` (`efr32bg22c224f512im40.h:98-102`), while the OpenISA LLL binds the port-selected `LL_RADIO_IRQn` to `isr_radio()` (`lll/lll.c:143`).

The vendor archive refines that route. In `librail_efr32xg22_gcc_release.a(generic_phy.o)`, the four-byte `RAC_SEQ_IRQHandler` section starts at archive file offset `0x52f48` and branches directly to `FRC_IRQHandler`, whose section begins at `0x52758`. The four-byte `BUFC_IRQHandler` section likewise branches to `FRC_IRQHandler`; both branches are directly disassembled from that archive member. That shared handler begins by computing and acknowledging enabled FRC (`IF +0x068` AND `IEN +0x06C`) and BUFC (`IF +0x114` AND `IEN +0x118`) pending sets before its larger packet/state dispatch. The device tree routes their direct IRQs separately—BUFC 32 and FRC 34—so QEMU asserts the corresponding direct line from each pending-and-enabled pair, while retaining RAC_SEQ line 38 for RAC sequencer flags. The same device header assigns AGC its separate direct IRQ 31; QEMU likewise drives that line from `AGC.IF +0x044` AND `AGC.IEN +0x048`, including the standard aliases, but does not synthesize an AGC event. The distinct 48-byte `RAC_RSM_IRQHandler` section starts at `0x52f68`; it masks RAC `IF` with `IEN`, clears the handled flags through the `+0x2000` alias, and calls the RAIL state-change callback only when the state-change flag is present. Thus `RAC_SEQ` is a shared FRC/BUFC/sequencer handler route, not a direct state-change ISR.

The stock application independently has that interrupt shape. During boot,
`flash-stock.bin:0x1bad8..0x1bae8` writes `0x00016000` (literal at
`0x1bb64`) to `SCB.VTOR` (`0xe000ed08`, literal at `0x1bb68`), selecting the
application vector table rather than the bootloader table at flash zero. Its
entries prove these installed radio-adjacent handlers: AGC IRQ31 →
`0x28bc6`, BUFC IRQ32 → `0x28b9c`, FRC_PRI IRQ33 → `0x28b9e`, FRC IRQ34 →
`0x2865c`, MODEM IRQ35 → `0x28bc4`, PROTIMER IRQ36 → `0x28ba0`, RAC_RSM
IRQ37 → `0x28ba4`, RAC_SEQ IRQ38 → `0x28ba2`, RDMAILBOX IRQ39 → `0x28c0e`,
RFSENSE IRQ40 → `0x496e0`, PRORTC IRQ41 → `0x2a75c`, and SYNTH IRQ42 →
`0x28c00` (vector words at `flash-stock.bin:0x160bc..0x160e8`; table values
carry the Thumb bit). The IRQ32/33/34/35/36/38 entries converge on the common
dispatcher at `0x2865c`: its first instructions read, mask, and acknowledge
three pending/enable register pairs (`0x28660..0x28692`), beginning with FRC
`IF/IEN` at `0xa8004068/+0x06c` (base/ack literals at `0x28d68/0x28d6c`) and
BUFC `IF/IEN` at `0xaa000114/+0x118` (literals at `0x28d70/0x28d74`); the
third pair is AGC. BUFC pending bit 18 selects a call to `0x29664` at
`0x2883e..0x28846`, the already-located descriptor route. This is strong
evidence that an RX completion is intended to enter an interrupt-driven shared
radio path, with FRC/BUFC and timer/sequencer sources participating. It still
does not identify which pending bit means a valid RX packet or its byte layout.
IRQ37 is a distinct state-change path (`0x28ba4..0x28bc2`); IRQ31 and IRQ35
share a smaller mask/ack path at `0x28bc6..0x28bf6`; IRQ42 is a minimal
mask/ack handler at `0x28c00..0x28c0c`. The later stock PROTIMER start helper
does explicitly clear and enable IRQ36 at `0x29a3a..0x29a50`; the provisioning
blocked native boot has not reached that helper, so this does not establish
that FRC, BUFC, or RAC NVIC enables have occurred in that stalled run.

The shared-dispatch statement is direct instruction evidence, not just a
matching handler shape: the first instruction at the installed BUFC,
FRC_PRI, MODEM, PROTIMER, and RAC_SEQ entries is respectively `b.n 0x2865c`
at `flash-stock.bin:0x28b9c`, `:0x28b9e`, `:0x28bc4`, `:0x28ba0`, and
`:0x28ba2`.  FRC IRQ34's vector already names `0x2865c` itself.  Thus those
six vector routes enter one dispatcher before it reads any FRC/BUFC/AGC
pending register.  This proves installed control flow only: it neither proves
that an NVIC line was enabled in the observed stalled boot nor assigns an RX
meaning to any pending bit.  In contrast, IRQ31 begins an AGC-only
mask/acknowledge sequence at `0x28bc6`, IRQ37 begins its separate RAC state
path at `0x28ba4`, and IRQ39 begins a separate RDMAILBOX mask/acknowledge
path at `0x28c0e` (all offsets are bytes in `flash-stock.bin`).

The stock handler separates FRC status acknowledgement from descriptor
delivery. It computes and W1C-acknowledges the entire enabled FRC pending mask
at `0x28660..0x28672`, but its direct FRC branches test bits 16, 2, and 14
only (`0x28708..0x28780`); it does not branch on FRC `RXDONE` bit 4 before the
whole-mask acknowledgement. Conversely, BUFC pending bit 18 branches at
`0x2883e..0x28846` to `0x29664`. That helper selects an eight-byte record from
the software ring rooted at RAM `0x20001178` (literal at `0x29860`), advances
the ring by eight bytes, updates BUFC channel-2 `THRESHOLDCTRL` at
`0xaa0000b0`, acknowledges BUFC flag `0x40000` through `0xaa002114`, and
publishes the selected record pointer at state `+0x08` (`0x29664..0x296a4`).
The compatible public layout identifies `+0x0b0` as the threshold/watermark,
not a producer/consumer pointer (`efr32bg21_bufc.h:208-222`). Its return is
the selected record's byte `+2` low three bits; the ISR dispatches values 1–7
through the table at `0x28846..0x28880`. It subsequently invokes deferred
record handling at `0x296a6` before clearing the BUFC pending mask
(`0x28b28..0x28b36`). Thus BUFC channel-2 threshold is the concrete stock
descriptor-delivery route; FRC `RXDONE` is not, on the current evidence, a
direct PDU-delivery condition. The meaning of record byte `+2`, its payload
framing remains unrecovered. Its low three-bit class does have an independently
source-backed event meaning: the branch table bytes at `0x28850..0x28856` map
classes 1/3 to `r8 |= 0x10000`, 2 to `0x800`, 4 to `0x400`, 5 to `0x100`, and
6/7 to `0x8`. The public RAIL event ABI names those bits respectively
`RX_PACKET_ABORTED`, `RX_ADDRESS_FILTERED`, `RX_FIFO_OVERFLOW`,
`RX_FIFO_FULL`, and `RX_PACKET_RECEIVED`
(`vendor/hal_silabs/gecko/platform/radio/rail_lib/common/rail_types.h:745-778,
907-915,968-1012,1068-1076`). The binary is stripped, so this is an ABI
correlation rather than a recovered symbol name; its numeric mapping is direct.
In particular, classes 6 and 7 provide the missing concrete stock path from
BUFC channel-2 threshold to the generic RAIL receive event.

The deferred call is descriptor release, not an RX-client callback. `0x296a6`
calls `0x29574` only for a retained, untagged record pointer, then clears the
saved pointer (`0x296a8..0x296b8`). The release marks descriptor word `+0`
with bit 15, writes its low 13 bits to BUFC `0xaa000058`, acknowledges BUFC
`0x40000`, and updates queue state (`0x29574..0x2963c`). The generic event
processing call is instead `0x27b3e` at `0x28b1e..0x28b24`, before release;
it reaches `0x1e39c` after masking its event sets. That call is shared with
non-RX paths, so it is a next trace target—not evidence that a Bluetooth
receive callback has already run.

The next generic layer remains shared rather than an RX callback. At
`0x27b3e..0x27b54` it masks three event sets against fields of the state object
at literal `0x28474` before calling `0x1e39c`. The latter reaches its
client-style indirect call through context `+0x08` only when the post-policy
event pair intersects callback masks at context `+0xe8/+0xec`
(`0x1e448..0x1e460`); its arguments are the context and those two masks.
Neither condition is exclusive to the BUFC descriptor class. The apparent
third-event-set bridge is independent: the descriptor return class maps only
to the first event mask (`r8`) through `0x28842..0x28880`, while `r7` is a
separate pending/enable word loaded from `0x200010b0+0x50/+0x5c`
(`0x286b6..0x286d8`). Its bit-10 path calls `0x2847c(2)`, a separate byte
counter/quota helper at `0x200032f0+2`, and only conditionally contributes
third-event-set bit zero (`0x28480..0x28532`). No descriptor path writes `r7`
or that third event set. Descriptor classes can therefore reach the generic
masked-event callback only through their `r8` mask contribution; mapping one
of those masks to a Bluetooth RX-client callback remains unrecovered.

The open port consequently binds `LL_RADIO_IRQn` to direct `FRC_IRQn` and enables only `FRC.IEN.TXDONE|RXDONE` during `radio_setup()`. The generated revB2 model places `IEN` at `+0x06C`, exposes the standard SET alias, and names those two bits (`FRC_S_register.py:684-704`, `FRC_S_field.py:1197-1254`). This is deliberately narrower than the vendor handler: it gives the existing `radio_is_done()` completion policy an identical IRQ source, without treating an arbitrary sequencer, buffer, error, or RAC state flag as successful completion. It does not establish receive production or the rest of RAIL's packet-state dispatch.

The generic OpenISA LLL, not the radio HAL, installs and enables this route: `lll_init()` uses `IRQ_CONNECT(LL_RADIO_IRQn, ..., isr_radio, NULL, 0)` then enables `LL_RADIO_IRQn` (`lll/lll.c:142-155`). Since this port maps that macro to `FRC_IRQn` (`include/hal/efr32/ll_irqs.h:14-18`), `radio_isr_set()` only swaps the callback parameter and `isr_radio()` invokes it. The FRC enable policy above confines that callback path to TXDONE/RXDONE in this port; it does not make any other FRC source a completion.

The open HAL implements the source-backed TX/abort/idle subset: `radio_tx_enable()` waits for `STATUS.STATE=OFF` then writes `TXEN`; `radio_disable()` performs separate write-only `RXDIS` then `TXDIS` commands; and `radio_is_idle()` returns whether `STATUS.STATE` is OFF. The idle-before-TX sequencing is the portable OpenISA contract (`hal/RV32M1/radio/radio.c:811-818`), and the advertising path invokes it at `lll_adv.c:698`. The field definitions are public generated revB2 source: `RAC_S_field.py:207-214` (`TXEN`), `:157-164` (`STATE`), `:257-264` (`TXDIS`), `:287-294` (`RXDIS`), `:797-804` (`SEQIF.STATETX`, bit 25), and `:707-714` (`SEQIF.STATEOFF`, bit 16) in the sibling `silabs-simplicity-sdk` checkout. Separate abort writes intentionally avoid claiming that a combined command write is hardware-equivalent. The native QEMU qtest covers `TXEN` then `TXDIS` state transitions and their matching `SEQIF` bits.

Each modeled RAC state transition also latches `SEQIF.STATECHANGESEQ` (bit 0)
alongside its state-specific sequencer flag. This is a public register-visible
event only: it follows `RAC_S_field.py:667-673` and does not assert that QEMU
implements the hardware sequencer hand-off or its timing. The focused qtest
covers both TX and OFF transitions.

`radio_disable()` has an additional controller lifecycle obligation which RAC's
commands cannot supply: OpenISA abort and ticker-stop call sites first replace
the radio callback with `isr_abort`, `isr_cleanup`, or `isr_window`, then call
`radio_disable()` (`lll_scan.c:300-341`, with equivalent paths in advertising
and connection code). The reference RV32M1 HAL aborts its sequence and
deliberately arms a T2 IRQ to enter that callback (`hal/RV32M1/radio/radio.c:821-848`);
it is not a received or transmitted PDU completion. On EFR32, RXDIS/TXDIS
have no FRC TXDONE/RXDONE latch, so the port acknowledges any old completion,
records an explicit software-abort reason, and pends the already-connected
`FRC_IRQn`. `isr_radio()` consumes that reason before examining FRC flags and
calls the installed cleanup handler without making `radio_is_done()` true.
`radio_isr_set()` clears both the reason and a pending FRC IRQ while swapping
handlers, preventing an old abort from being delivered to a later event. This
is an OpenISA control-plane completion only; it assigns no packet or RAC event
semantics.

The locally installed xG22 GCC RAIL archive is an additional behavioral gold standard: SHA-256 `c94fa52d538db6faa9150a327edff04f00980947897e7c7d09d12686190477fb`, with `rfhal_rac.o` payload at archive byte offset `0x23cbc` and `generic_phy.o` at `0x518c8`. The former reads non-secure `RAC.STATUS` at `0xB802000C`, extracts state bits `24..27`, and handles state values including `2` and `9`; its status view also uses `SEQACTIVE` bit 28, `TXENS` bit 30, and `RXENS` bit 31. `generic_phy.o:RAC_RSM_IRQHandler` masks `RAC.IF` with `RAC.IEN`, clears handled flags, and reads that state only for `STATECHANGE`; `RAC_SEQ_IRQHandler` branches to `FRC_IRQHandler`. This corroborates QEMU's state values, status bits, and direct RAC-RSM `IF & IEN` route, and its focused regression checks the TXENS/RXENS status bits. It does not justify a synthetic RXCAL completion or a general sequencer completion event.

Two adjacent OpenISA helpers have a deliberately narrower contract than their names suggest. The reference RV32M1 HAL documents `radio_is_ready()` as always false because the LLL expects no idle/TX/RX state at that staging point (`hal/RV32M1/radio/radio.c:855-859`); its advertising path asserts that result after packet setup (`lll_adv.c:536`). It likewise marks `radio_has_disabled()` unused and returns false (`hal/RV32M1/radio/radio.c:866-870`). The EFR32 HAL now matches those software contracts directly rather than inventing a RAC-state interpretation.

The public FRC completion latches make a narrow status implementation safe: `IF.TXDONE` is bit 0 and `IF.RXDONE` bit 4 (`revB2/FRC_S_field.py:1197-1254`). `radio_is_done()` consequently observes either latch, while `radio_status_reset()` acknowledges exactly those two bits through the standard CLR alias; this does not decide which IRQ should invoke the LLL callback or how receive completion is produced. QEMU's 150-us TX timer remains a simulator abstraction, but on expiry it now raises the public `FRC.IF.TXDONE` latch and returns RAC to OFF; `TXDIS`/`RXDIS` cancel that pending synthetic completion. Its regression verifies the visible latch and final RAC state.

The stock TXDONE hand-off can be traced one step further in the same archive.
In `generic_phy.o:FRC_IRQHandler`, the enabled FRC mask is retained in `r7`
after the read/AND at section offset `0x0006` (archive offset `0x5275e`) and
is cleared through the FRC `+0x2000` alias at section offset `0x0010`
(`0x52768`). Near the common exit, section offsets `0x067c..0x0682`
(`0x52dd4..0x52dda`) pass the accumulated event word, the secondary event
word, and the retained FRC-derived word to
`sli_rail_978af8e4393fb6ecb4e92f5256373899` (section/archive offset
`0x001e4`/`0x51aac`). That helper masks each word with its registered event
masks before calling the RAIL event combiner.

The combiner is `sli_rail_2c0c7ba0065447faa0ab7205349f5d73` in
`rail_rf_hal.o`, whose section starts at archive offset `0x13384` (the
`rail_rf_hal.o` member begins at `0x10dd4`). Its section offsets
`0x0008..0x001a` (`0x1338c..0x1339e`) explicitly test bit 0 of the first,
already-filtered event word and enter the TX-event path when that bit is set;
bit 9 of the second word is tested alongside it. This establishes the vendor
ordering: read `FRC.IF`, mask with `FRC.IEN`, clear the enabled FRC bits, then
translate the resulting event state through the registered event masks and
combiner. It does not establish that every FRC bit is translated into that
first event word: the handler has no direct branch on public
`FRC.IF.TXDONE` bit 0, and the combiner receives accumulated words rather than
the raw latch. The public TXDONE bit assignment therefore remains the safe
basis for the open port's narrow completion path, while this archive trace
supplies the stock acknowledgement/order evidence only.

The direct dataflow resolves the remaining TXDONE question: raw `r7` is not
copied into the combiner's first word. After the nonzero test at section
offset `0x00a4` (archive `0x527fc`), the handler tests only `r7 & 0x8000` at
`0x00aa` (`0x52802`). For a nonzero mask without that bit—including a mask
containing only public `FRC.IF.TXDONE` bit 0—the branch reaches `0x025c`
(`0x529b4`), moves the zero test result into `fp`, and then unconditionally
sets `fp` to `0x01000000` at `0x0104` (`0x5285c`). The final call moves this
derived `fp` to the first argument at `0x067e` (`0x52dd6`); the raw-derived
value is instead passed as the third argument from `r7` at `0x067c`
(`0x52dd4`). The event-mask helper preserves that separation (`r0` is masked
at archive `0x51ac2`, while `r2` is masked at `0x51ab6`). The combiner tests
bit 0 only in its first argument (`rail_rf_hal.o` archive `0x13390`), while
its uses of the third argument test bits 31 and 1..3, not bit 0. Thus the
archive proves FRC TXDONE is acknowledged and can gate entry into the shared
path, but does not prove—and in this direct path excludes—a raw TXDONE-bit-0
mapping into the combiner's TX-event bit. The open port's separate TXDONE
completion policy remains necessary.

Both latches share `FRC_IRQn`, but OpenISA TX callbacks advance their state
machine without independently testing `radio_is_done()` (`lll_adv.c:511-542`).
The EFR32 HAL therefore records TX or RX direction before arming its respective
RAC source and dispatches the installed LLL callback only for the matching
named completion bit; it clears mismatched or stale completion flags so they
cannot leave the IRQ asserted or turn an RX boundary into a TX callback. This
is software IRQ attribution over the two public latches, not a claim about how
either latch is produced or a substitute for RX delivery.

`FRC.TRAILRXDATA` has a public field named `CRCOK`, but its complete receive
semantics remain unproven: the non-secure register is at `+0x054`, reset zero,
and read-write with mask `0x0000003f`; `CRCOK` is bit 1
(`revB2/FRC_NS_register.py:498-521`, `FRC_NS_field.py:987-1044`). In the same
vendor archive, `RAIL_ConfigRxOptions` writes the *whole* register as zero
when `REMOVE_APPENDED_INFO` is selected and as `0x1b` when it is clear; the
latter includes the public bit named `CRCOK`. The generated names do not say
why that option maps to these values, so they cannot establish that bit 1 is
always an unmodified hardware-produced CRC result. The OpenISA LLL reads
`radio_crc_is_valid()` only after `radio_is_done()` in its advertising, scan,
connection, and test ISRs (`lll_adv.c:605-609`, `lll_scan.c:367-371`,
`lll_conn.c:137-143`, `lll_test.c:136-143`). The open HAL samples the named
bit directly only because it has no receive producer or RX-options setup; it
does not claim a working CRC policy. A future producer must first prove the
active appended-info configuration and then distinguish the hardware result
from a software delivery failure. The reference RV32M1 `pkt_rx()` marks an
over-limit length with private `force_bad_crc`, still reports completion, and
makes `radio_crc_is_valid()` false (`hal/RV32M1/radio/radio.c:233-250,351-375,
907-916`). If the EFR32 normalized-PDU staging helper rejects a completed
frame but the implementation still raises RXDONE, it likewise needs its
private delivery-invalid latch, sampled with the register and cleared at
status reset; writing or relabelling bit 1 would be an unproven policy. QEMU
retains the documented six-bit mask and does not synthesize the bit because
packet reception and CRC computation are unmodelled.

`RAIL_StartRx` supplies the corresponding receive-start primitive. It branches from `rail_singleprotocol.o` into `sli_rail_dadf1c52f26190429dcacf7151424c68` in `rail_rf_hal.o`; the 68-byte routine begins at archive file offset `0x11b68` in `librail_efr32xg22_gcc_release.a` (member data offset `0x10dd4`, section offset `0x0d94`). After its setup helpers, it stores `2` at non-secure `RAC.RXENSRCEN.SET` (`0xB8021008`). Public generated fields identify bit 1 as `SWRXEN`, and `SEQIF.STATERXSEARCH` is bit 18 (`revB2/RAC_NS_field.py:727-734`). The open HAL consequently waits for `STATUS.STATE=OFF` and performs exactly that SET-alias write. QEMU latches the public source-enable register, then represents that source-backed request as stable `RXSEARCH` (state 2), including the matching sequencer flag; it deliberately does not claim packet reception, frame transition, or receive completion. Its qtest verifies `SWRXEN` produces `RXSEARCH`, then `RXDIS` produces `OFF`.

The parallel TX API is materially broader than the HAL's current start
primitive. `RAIL_StartTx` is a four-byte wrapper at archive offset `0x149464`
in `rail_singleprotocol.o` (member offset `0x148fdc`, section-relative offset
`0x488`) and tail-calls `sli_rail_0aff8abbb6b0be838a41142fd1c42495` at
`0x13fb4` in `rail_rf_hal.o`. That wrapper calls the 548-byte common TX helper
`sli_rail_3d0930b0b28334e352a161a5b4a4cb94`, which begins at `0x13d90`
(same member; section offset `0x2fbc`). Direct disassembly of that helper
contains accesses through RAC alias bases, a `0xBA000000` BUFC literal, and a
sequencer-RAM literal (`0xB0003E00`), as well as calls to further helpers.
For example, at helper-relative `0x7c` it writes `0x20` through the RAC clear
alias at `0xB8022000 + 0x40`; the public rev-B2 model names the underlying
register `RAC_NS.SR0` (`RAC_NS_register.py:454-470`). This is a concrete
vendor start-path fact, but not enough to assign packet/FIFO semantics to
those writes or reconstruct the called helpers. `radio_tx_enable()` therefore
continues to issue only the public `RAC.CMD.TXEN` primitive. This final
primitive is independently archive-backed: the common helper's successful
path calls `sli_rail_8a1daf8683aeba05cbb8b1aa7d69b03e` in `generic_phy.o`;
its 44-byte section starts at archive offset `0x51fb4` (member data offset
`0x518c8`, section offset `0x6ec`). At section-relative `+0x10..+0x12` it
writes `1` to the literal `0xB8020000 + 0x10`, which the public rev-B2 model
names `RAC_NS.CMD`. QEMU models this command and has a focused TX-command
regression. Its fixed completion remains an explicit simulator abstraction
rather than a claim that it executes the RAIL transmit pipeline.

This identifies every previously observed stock write: the recovered image reaches radio initialization at about 98.5 ms virtual time, polls `STATUS.STATE`, writes `RXCAL`, `RXDIS`, and `TXDIS`, and waits for TX state 9 before disabling. In the retained stock trace, `radio-trace.log:201` writes `RXDIS = 0x100`, `:203` reads `STATUS`, `:208` writes `TXDIS = 0x20`, and `:207` is the preceding `STATUS` read. That is an observed ordering fact, not a claim that the two commands are interchangeable. The QEMU model uses this source- and trace-matched RAC map. Its packet modulation and fixed 150 us completion are still simulator abstractions, not hardware timing claims.

The same trace records the reset prefix independently of a transmit request.
At byte offsets `0x00000a26`, `0x00000a56`, and `0x00000a86` in
`evidence/stock/radio-trace.log` (lines 55--57), stock firmware clears
`RAC.SR0`, `SR1`, and `SR2` (`+0x040`, `+0x044`, and `+0x048`).  At offset
`0x00000ab6` (line 58) it writes one to `RAC.CTRL` (`+0x014`), which the
public rev-B2 field model identifies as `CTRL.FORCEDISABLE`; it then reads
`STATUS` at `0x00000b16` (line 60).  The following write to `SEQIF` and the
later `CTRL` values `0x04000000` and zero (offsets `0x00000ae6`,
`0x00000b75`, and `0x00000ba5`) belong to an uncharacterized sequencer
hand-off. Hardware bring-up without the proprietary sequencer firmware showed
that writing `FORCEDISABLE` while RAC was already OFF instead entered
`STATUS.STATE=SHUTDOWN` (13) indefinitely. The open HAL therefore skips that
vendor reset prefix when RAC is already OFF. QEMU now exposes SHUTDOWN for an
asserted `FORCEDISABLE`, while still cancelling its synthetic TX timer, rather
than inventing the omitted sequencer transition back to OFF.

## 4.2 SYNTH, FRC, and AGC fields [F]
The same public revB2 model identifies the remaining core data-path fields. SYNTH has 28-bit `FREQ` at `+0x034`, 21-bit `IFFREQ` at `+0x038`, `DIVCTRL` at `+0x03C`, 6-bit `CHCTRL.CHNO` at `+0x040`, and 18-bit `CHSP` at `+0x044`. The public BLE 1M channel config specifies base 2402 MHz, 2 MHz spacing, physical channels 0..39. Zephyr passes MHz relative to 2400 (`2`, `26`, `80` for advertising). Hardware watchpoints now establish the corresponding working `FREQ` words as `0x01f46aab`, `0x01f96aab`, and `0x0204aaab`; the HAL writes the named advertising-channel word and `CHNO = (chan - 2) / 2`. General data-channel frequency encoding remains deliberately rejected until it has a source-backed derivation. QEMU preserves these fields with their public masks but does not infer RF tuning from `CHNO` alone.

A hardware-breakpoint trace of the working stage-2 M33's channel-37 TX
preparation path writes `SYNTH.FREQ=0x01f46aab`, followed by `CHCTRL=0` and
`CHSP=0`.  It also updates `MODEM.SRCCHF`, `MODEM.DIGMIXCTRL`,
`RAC.DIGCLKRETIMECTRL`, and `RAC.XORETIMECTRL`.  Injecting the observed
frequency alone, and then the frequency plus the stock mixer and retiming
values, did not make stage 3 observable in separate 20-second LE scans.  This
proves that QEMU's present CHNO-only tuning abstraction is incomplete, but it
does not justify copying those channel-37 calibration words into the HAL.
Their named derivation and the still-stalled FRC/BUFC launch contract remain
open.

After the FIFO-consumption boundary was recovered, repeating the frequency
test plus a snapshot-scoped match of the differing named FRC, MODEM, RAC,
PROTIMER, mixer, and retiming registers still produced no advertisement at
the exact staged AdvA.  Those raw values remain characterization evidence and
were not promoted into the HAL.  The negative result moves the remaining gate
past channel selection and the tested static register snapshot.

Matched hardware snapshots at the initial RAC-enable boundary establish an
ordering constraint that endpoint-only QEMU previously omitted. Stock enables
BUFC and PROTIMER before RAC, while the original open HAL enabled both after
RAC. On silicon the latter path remains in SHUTDOWN, and enabling the two
dependencies later does not repair the state. The HAL now retains the complete
observed block order; QEMU's focused enable-order regression preserves the
failure boundary without pretending to implement the still-unknown successful
sequencer hand-off.

The final immediate-TX primitive adds one short sequencer handshake before
`RAC.CMD.TXEN`: helper `0xe35c` sets `RAC.SR0` bit 7, clears `SR0` bit 3, and
clears bit 5 at shared sequencer word `0xb0003ec4`.  Replaying that exact state
under the otherwise augmented stage-3 run did not produce an advertisement or
advance the BUFC read offset; that establishes only that it is insufficient.
Because static control flow proves it is part of every working launch, the HAL
retains it behind phase/provenance names.  The M0+ helper at image offset
`0x9c8` pulses its SR0 gate, tests shared bit 5, and writes the public
`RAC.RXENSRCEN.SWRXEN` command only when the bit is clear; this identifies the
shared bit as an RX-after-TX inhibit.  Recovering the consumers of M33's SR0
bits 7 and 3 remains required before those two names can claim field meaning.

The in-process QEMU RF mailbox now snapshots that `CHCTRL.CHNO` value at its synthetic `RAC.CMD.TXEN` boundary and delivers only to an `RXSEARCH` peer with the same currently latched value. This is the minimal channel-selection behavior supported by the public field and the HAL's channel programming: its focused qtest rejects a mismatched peer, then accepts the identical still-active snapshot after that peer retunes and re-enters `RXSEARCH`. It is not a PHY, propagation, whitening, access-address, advertising-filter, or connection-channel-map model; those semantics remain unmodelled.

FRC exposes dynamic-frame-length configuration at `DFLCTRL +0x00C`, `MAXLENGTH +0x010`, frame/length/address word-count comparators at `+0x020..+0x028`, whitening control/polynomial/init at `+0x030/+0x034/+0x038`, RX control at `RXCTRL +0x04C`, and four frame configuration descriptors `FCD0..3` later in the block. `MAXLENGTH` is specifically a 12-bit `MAXLENGTH` field in bits 0..11 plus a four-bit `INILENGTH` field in bits 12..15 (`FRC_S_register.py:118-133`, `FRC_S_field.py:277-294`); the BLE tables' `0x4101` is therefore not a one-field byte maximum. The public model also gives eight-bit `DATABUFFER +0x018` and four-bit `BUFFERMODE +0x078` (`FRC_NS_register.py:161-170,779-793`, `FRC_NS_field.py:340-344,1820-1844`), plus `PKTBUFCTRL` at `+0x0CC` (mask `0x0103FFFF`) and read-only `PKTBUFSTATUS` at `+0x0D0` (six-bit count). The current public xG22 rev-B1 model identifies `SNIFFCTRL +0x084` as a read/write register with mask `0x0003FFFF` (`host_py_rm_studio_internal/full/efr32xg22/revB1/FRC_S_register.py:798-804`); stock writes it through the standard CLR then SET windows at `flash-stock.bin` offsets `0x2a6cc` and `0x2a6d0`, so QEMU preserves that exact masked latch without assigning sniff behavior. The same model identifies `RAWCTRL +0x08C` with mask `0x000021BF` (`FRC_S_register.py:848-867`); the historical stock trace writes this register at `radio-trace.log:91`, so QEMU likewise constrains the latch but does not infer raw-data behavior. `RXCTRL` has mask `0x000007FF`; it names RX packet/error-policy fields but does not itself establish packet-buffer behavior. The same public `FRC_NS_register.py:610-699,998-1087` defines `IF/IEN` at `+0x068/+0x06C` and `SEQIF/SEQIEN` at `+0x0B4/+0x0B8`, each with writable mask `0x7F7FFFFF`; QEMU retains those latches and verifies the masks. The current public rev-B1 model additionally identifies historical configuration offsets `MODEM.IF/IEN +0x208/+0x20C` (mask `0x0007FFFF`) and `MODEM.IRCAL +0x228` (mask `0x0000FFBF`) (`MODEM_S_register.py:1641-1707,1841-1854`); QEMU constrains those latches, but does not synthesize modem IRQs or calibration. That preserves software-visible pending/enable bits without assigning completion or IRQ policy. The generated `FRC_S` and `FRC_NS` models give the same offsets/masks at secure `0xA8004000` and non-secure `0xB8004000`; Zephyr's xG22 devicetree exposes `radio@b0000000` (`efr32xg22.dtsi:11-24`), so the open HAL uses the latter. RFCRC similarly has secure/non-secure bases `0xA8010000`/`0xB8010000`.

The same public SVD defines write-only `FRC.CMD +0x02C`; bit 0 is
`RXABORT` and bit 9 is `CONVINIT`.  Static disassembly of the working stage-2
ELF finds directly addressed command stores at `0xe294`, `0xef14`, and
`0x10f7c`, all writing `0x201` (`RXABORT | CONVINIT`).  It finds no direct M33
store to `FRC.DATABUFFER`.  The duplicated first two stores come from the same
named RAIL helper in `generic_phy.c` and `generic_phy_shared.c`; the third is
in a larger RAIL state-transition helper.  These sites establish that stock
initializes the convolution path while aborting RX, but do not yet establish
whether the command is sufficient to connect BUFC channel 0 to FRC for TX.

The public xG22 BLE 1M base table records `DFLCTRL = 0x00018101`, `WCNTCMP0/1 = 0/1`, whitening control/poly/init all zero, `CONVRAMADDR = 0x4000`, and `FCD1/2/3 = 0x4CFF/0x4DFF/0x4DFF` ([F] source locator: `ble_efr32xg22_configurator_out.c:225-242`). Its per-PHY table adds `MAXLENGTH = 0x4101`, `FECCTRL = 0`, `CTRL = 0x407A0`, and `FCD0 = 0x4CFF` (`...:474-481`); it also writes `SEQ.PHYINFO`, whose effect is not inferred here. QEMU retains the public masks for the decoded `MAXLENGTH`, `CTRL`, and `FCD0..3` registers and the base-table `WCNTCMP`, RAM-address, FEC, trailing-data, convolution, puncture, clock-gate, and supplemental-length latches using the generated public masks (`FRC_S_register.py:190-229,332-401,474-577,966-979,1146-1229`). It assigns none of them packet-pipeline effects. The generated field definitions (`FRC_S_field.py:197-274`, `:3357-3434`) decode `DFLCTRL` as `DFLMODE=1`, `DFLOFFSET=1`, `DFLBITS=8`, `MINLENGTH=1` (the other named fields zero). They decode each `0x4CFF` descriptor as `WORDS=255`, `BUFFER=0`, `INCLUDECRC=1`, `CALCCRC=1`, `SKIPWHITE=1`, with the remaining named fields zero; `0x4DFF` differs only by `BUFFER=1`. This establishes public configuration values, not their packet-pipeline or buffer semantics.

The portable OpenISA LLL consistently calls `radio_pkt_configure(8, max_len, flags)`: advertising (`lll_adv.c:353,356`), scanning (`lll_scan.c:160,163`), and direct test (`lll_test.c:195`) use their respective legacy or test payload maxima; connection setup uses its negotiated RX/TX octets, adding the four-byte MIC when encryption is enabled (`lll_conn.c:444-450,480-487`). These call sites establish that the API's second argument is a byte limit and that `flags` varies with PHY and encryption. They do not establish the relation of that byte limit to the separate `MAXLENGTH`/`INILENGTH` fields or the FRC descriptors, so the open EFR32 HAL does not yet program them.

The RAIL frame-type callback does not close that gap. `RAILCb_ConfigFrameTypeLength` in `rail_rx.o` is a four-byte tail-call wrapper at archive file offset `0x1552a4`; its target `sli_rail_3bff6dfc0590d5c54bb0576abaeb38dd` in `rail_rf_hal.o` begins at `0x1434c`. The target stores a relocation-resolved helper address in a `.bss` slot and returns zero. It has no peripheral-address literal or MMIO store, so this callback path is not evidence for a `MAXLENGTH`, `INILENGTH`, or descriptor policy.

The xG22 RAIL archive gives a narrower, directly located length observation. `RAIL_SetFixedLength` is a four-byte wrapper in `rail_rf.o` around `sli_rail_308157e3c5d7e1d3aaf039be96b40226` in `rail_rf_hal.o`; the latter section begins at archive file offset `0x1123c` (member starts at `0x10dd4`, section-relative offset `0x468`). Its nonzero, non-`0xffff` path clamps the API argument at `0x1000`, then writes `argument - 1` to non-secure `FRC.WCNTCMP0` at `0xB8004020`; its zero and sentinel paths also update `FRC.DFLCTRL` at `0xB800400c`. The generated `FRC_NS` model identifies those offsets. This proves that RAIL's *fixed-length* API uses the non-secure FRC window and a word-count comparator, not that OpenISA's dynamic BLE `radio_pkt_configure(8, max_len, flags)` should use that policy.

The private-core working image supplies the separate dynamic-mode bootstrap
case. Its routine at `0x1632` writes a configuration-record capacity minus one
to `FRC.WCNTCMP0`; the matched BLE launch has `WCNTCMP0=0xff`, equal to the
public FCD0..3 `WORDS=255` sentinel. Leaving the public base-table value at
zero exposed the zero comparator before the length byte could establish a
boundary. `radio_pkt_configure()` now seeds a named
`DYNAMIC_LENGTH_PENDING_WORD_COUNT=255`. After an attempted 11-byte hardware
advertisement FRC had replaced it with 10, demonstrating header parsing and a
total-frame-count-minus-one boundary. The same attempt still latched
`SEQIF.TXABORTED` before IRQ8 and consumed only two bytes, so this initialization
is retained as correct but does not claim to solve the separate abort cause.

Working-image `0x2c1e..0x2c24` provides a similarly bounded reason for the
pre-launch `FECCTRL` difference: under its frame-mode branch it writes `7` to
the CLR alias and `1` to the SET alias. The public generated model identifies
bits 0..2 as `FECCTRL.BLOCKWHITEMODE`, and the live working BLE capture reads
mode 1. The open BLE path consequently selects named block-whitening mode 1
while preserving all other FEC fields; this complements its cleared
`FCD.SKIPWHITE` and channel whitening seed. A one-variable hardware test
confirmed the field latched but left `SEQIF.TXABORTED` and BUFC read offset two
unchanged. The vendor sets it because the active BLE frame uses block
whitening; it is retained for that purpose, not because it fixes this abort.

The vendor TX-FIFO setup provides separate BUFC evidence. `RAIL_SetTxFifo` reaches `sli_rail_86983bfebc3fcf81f3ab3444e4625a88` in `rfhal_bufc.o`, whose 60-byte section begins at archive file offset `0x33414` (member starts at `0x332f4`, section-relative offset `0x120`). Its caller validates a four-byte-aligned FIFO address, reduces the requested allocation to the largest supported power of two in the 64–4096-byte range, and limits the initial count to that result. The called BUFC routine first writes command `1` at `0xBA000034`, then writes the derived configuration, FIFO address, and initial count to `0xBA00000C`, `+0x004`, and `+0x008`; it follows with command `2` when the initial count is nonzero. The transform is directly recoverable from the caller: for a selected allocation `2^n`, the first word is `n - 6` (so 512 bytes encodes as `3`), not a raw byte capacity. QEMU retains these channel-0 setup latches and derives FIFO occupancy (including threshold status) from `WRITEOFFSET - READOFFSET`, so that initial byte count is visible before any port access. It applies the same 64–4096-byte encoded capacity to backing-RAM port accesses, wrapping them at the configured circular-buffer boundary; its regression writes across a 512-byte boundary. It also reads a configured BUFC data port from that guest-RAM backing address, matching RAIL's documented zero-copy `RAIL_SetTxFifo()` use (`rail.h:1425-1429`); its regression preloads RAM and verifies the port returns that byte. These exact writes establish a FIFO setup contract, but not whether FRC descriptors consume the configured buffer or how a RAC transition launches it.

A live DWT watchpoint adds the separate per-packet producer contract that the
SetTxFifo helper alone could not establish. Stock writes each packet octet as
a 32-bit value to channel-0 `WRITEDATA` (`base + 0x018`); the observed loop at
`0x11c60..0x11c76` performs four byte-valued stores per iteration. After the
first store, both `WRITEOFFSET` and `READOFFSET` were one, showing FRC was
already consuming concurrently. The open HAL now retains an owned PDU copy
during preparation and publishes it through the data port at its
scheduler-owned launch boundary. It no longer assigns WRITEOFFSET and issues
PREFETCH as though SetTxFifo's initial-count path were RAIL_WriteTxFifo.
QEMU's data-port regression covers byte insertion and pointer advancement;
the controller smoke test exercises the resulting HAL path. Silicon still
leaves the open path at write 11/read zero, so this corrects the producer
contract without claiming the missing FRC consumer contract.

`radio_pkt_tx_set` now applies exactly that limited contract (2026-10-31: the staging FIFO is no longer RAIL's fixed 512 bytes but the smallest power of two from 64 that holds the largest PDU the build stages, with the matching `n - 6` encoding; 64 bytes, encoding `0`, without data length extension or DTM. Not yet re-checked on silicon). It copies the OpenISA PDU's two-byte header plus its length-byte-selected payload into an aligned 512-byte staging FIFO, then writes `CMD=1`, configuration `3`, its address, the copied byte count, and `CMD=2` in the vendor order. QEMU's deliberately synthetic TXEN snapshot now reads that configured circular guest-RAM range (`ADDR`, `READOFFSET`, `WRITEOFFSET`) when present, while retaining the direct data-port staging path for register-level tests; its focused regression uses the same clear/configure/address/count/prefetch shape as the HAL. This makes the simulator observe the HAL's real staging representation, not an otherwise-empty internal port-write buffer. It remains a simulator snapshot boundary only: neither this change nor the QEMU latch model claims that FRC consumes channel 0, that RAC starts a packet, or that any bytes reach the air.

The corresponding RAIL RX-FIFO helper is directly located at archive file offset `0x3345c` in `librail_efr32xg22_gcc_release.a(rfhal_bufc.o)` (member starts at `0x332f4`, section-relative offset `0x168`, 20 bytes). It writes channel 1 `CMD=1` at `0xBA000074`, then its capacity encoding, backing address, and `THRESHOLDCTRL=0x0FFF` at `0xBA00004C`, `+0x004`, and `+0x024`; it does not issue a prefetch command. The caller derives the same 64–4096-byte power-of-two encoding as the TX path. `radio_pkt_rx_set` therefore initializes an owned aligned 512-byte channel-1 staging buffer using that exact sequence, deliberately leaving the caller PDU untouched until the FRC delivery/descriptor contract is known. QEMU regression coverage retains those four latches. This is FIFO allocation only, not RX enable, packet reception, or a claim that any received bytes become visible to OpenISA.

The archive now also proves the *byte-consumption* half of that FIFO, but not
packet delivery. `RAIL_ReadRxFifo` is a wrapper in `rail_data.o` at archive
offset `0x15be2c` (member data `0x15bccc`, section `+0x160`) which tail-calls
the common reader in `rail_rf_hal.o` at `0x12adc` (member data `0x10dd4`,
section `+0x1d08`). That reader obtains packet information first, clamps the
requested byte count to its returned 16-bit availability, then invokes the
BUFC copy helper with channel 1. The helper in `rfhal_bufc.o` at `0x335fc`
(member `0x332f4`, section `+0x308`) reads channel-1 `STATUS` low 13-bit
count at `0xBA00006C` and `READOFFSET` at `0xBA000058`; its callee at
`0x33328` (section `+0x34`) copies circular bytes from `BUF.ADDR |
0x20000000` and advances `READOFFSET` when requested. This proves that the
configured backing RAM is a consumable circular byte buffer. It does not
recover the prerequisite packet-info layout, an on-air frame boundary, CRC
policy, or when those bytes become eligible to copy into an OpenISA PDU.
The sequencer's `RXRAWDATA`-to-channel-1 PIO path and `FRC.RXDONE` remain
separate observations; a simulator may choose to make all its modeled writes
visible before its synthetic completion, but must not label that ordering as
vendor hardware behavior.

The packet-info helper used immediately before that copy establishes only a
queue/buffer presentation. Its result has status byte `+0`, availability
`u16 +2`, first contiguous span `u16 +4`, first data pointer `u32 +8`, optional
wrap pointer `u32 +12`, and metadata byte `+16`; `RAIL_ReadRxFifo` clamps its
request to the `+2` value. The helper derives these from an eight-byte internal
queue record and BUFC state; its completed-packet path reads BUFC around
`0xBA000058`/`0xBA0000AC`, while the incoming variant observes
`0xBA000054`/`0xBA00005C`. This proves that RAIL consumes a scatter/gather view
of circular buffer availability, not that any of those fields is a BLE PDU
length, a CRC result, a frame boundary, or a sequencer-produced record format.

The archive resolves the relationship between that descriptor and release more
tightly. `RAIL_GetRxPacketInfo` at `rail_data.o` archive
`[0x15be30, 0x15be34)` tail-branches to 356-byte
`rfhal_bufc.o:sli_rail_052f8b2565813079f19c931bac48db51`, archive
`[0x33a5c, 0x33bc0)` (member `0x332f4`, section file offset `0x768`).  Its
completed-record path rejects a descriptor whose first `u16` is at least
`0x4000`, uses that field's low 13 bits with BUFC `READOFFSET`/`STATUS`
(`0xBA000058`/`0xBA0000AC`) to locate bytes in the separate data buffer, and
uses descriptor byte `+2` low three bits and byte `+3` for output bytes `+0`
and `+16`.  It constructs the one- or two-span pointers and lengths at output
offsets `+4..+12`; it does not parse a BLE header or CRC.  Separately,
`RAIL_ReleaseRxPacket` at archive `[0x15bef0, 0x15bef4)` tail-branches to
`sli_rail_f921a6fdde390c986016ab0c8bf49fae`, the same local helper reached by
the deferred FRC/BUFC path above.  Thus the eight-byte object is an RX
descriptor for data-buffer presentation and release, not a normalized PDU;
the descriptor's proven fields still do not establish on-air framing or CRC
metadata.

The completed-record path also resolves descriptor word 0's data relationship.
At helper-relative `+0x94..+0xf2` it reads channel-1 `READOFFSET`
(`0xBA000058`) and low-13-bit `STATUS` (`0xBA0000ac`), rejects descriptor word
0 values at least `0x4000`, then treats word 0 low 13 bits as a channel-1
circular-FIFO endpoint. It computes the capacity-wrapped distance from the
current read cursor to that endpoint and stores it as packet-info
`packetBytes` at `+2`; it constructs one or two backing-RAM spans from the
same cursor. Therefore the word is neither a direct BLE length nor a channel-2
offset. This fully explains RAIL packet-info/copy mechanics, but still cannot
normalize the bytes for Zephyr: RAIL does not parse a BLE header here, and
packetBytes may include appended radio detail under the unresolved active
RX-options configuration.

`rail_data.o` makes the copy boundary equally explicit: its packet-copy helper
is archive `[0x15bf34,0x15bf5c)` (member starts `0x15bccc`, section `+0x268`)
and performs only one `memcpy` for span 0 plus an optional second `memcpy` for
the wrapped span. It performs no header, length, or CRC parsing. This confirms
that neither RAIL's packet copy nor the descriptor endpoint supplies the
normalization Zephyr requires.

The installed public RAIL headers corroborate the API-side layout without
filling that hardware gap: `rail_types.h:6172-6231` names the fields as
`packetStatus`, `packetBytes`, two portion lengths/pointers, and `filterMask`,
and `rail.h:4400-4432` says `RAIL_CopyRxPacket` copies exactly `packetBytes`
through those portions.  They also make direct use as a BLE PDU unsafe:
`rail.h:4345-4355,4382-4391` warns that incoming information can be pre-CRC
and include radio-appended detail, while `rail_types.h:5753-5767` documents
that appended-info configuration changes whether those bytes are interpreted
as packet data.  The hold/release lifecycle is likewise software state rather
than descriptor framing: `RAIL_HoldRxPacket` tail-branches to
`sli_rail_d9e…` at `[0x33850, 0x33868)`, which sets bit zero in the shared
candidate pointer; the deferred helper tests that tag before releasing the
entry.  This is a hold marker on the pointer, not bit zero of a hardware
descriptor.  Consequently an eventual Zephyr RX extractor must first prove
the active RAIL appended-info and completed-status configuration, rather than
passing `packetBytes` or the descriptor bytes to the LLL as a normalized PDU.

One concrete appended-info control is located, but the stock selection is not.
The public `RAIL_RX_OPTION_REMOVE_APPENDED_INFO` is bit 4
(`rail_types.h:5629-5767`). `RAIL_ConfigRxOptions` tail-branches to
`rail_rf_hal.o:sli_rail_c1ee…`, archive `[0x11bd8, 0x11d04)` (member
`0x10dd4`, section file offset `0xe04`); when its option mask includes bit 4,
it updates active-handle `+0x90` and writes FRC `+0x054` with zero when that
option is selected or 27 when it is clear. This proves the current RAIL
library's hardware-observable option handling, not the stock firmware's
choice. The complete stock-image little-endian word scan finds the secure
`FRC+0x054` literal only at file offsets `0x1f7dc`, `0x28d68`, and `0x298a8`;
it finds no normal, SET, CLR, or TGL alias literal for either secure or
non-secure FRC. Their recovered uses are reads: instruction `0x1f31e` loads
`0x1f7dc`, then `0x1f322` reads `[r0]`; `0x2889e` loads `0x28d68`, then
`0x288a2` reads `[r1]`; and `0x297c8` loads `0x298a8`, then `0x297ca` reads
`[r1]`. The fourth use of `0x28d68`, from `0x28660`, accesses only FRC
`+0x14/+0x18` (`IF/IEN`). The sole nearby helper candidate passes
`0xA800404c` at `0x1ea1c` to `0x1e22a`, which adds `0x2000` and stores to the
`RXCTRL` SET alias `0xA800604c`, not to the trailer. Nearby FRC base literals
`0xA8004040/4048/404c/4050` likewise have no recovered `+0x54` store. Thus no
literal-backed stock store
or `RAIL_ConfigRxOptions` call has been established. Computed-address writes
or opaque helper use remain possible, so this absence is not evidence for
either option state. A fresh bounded 25-second native-QEMU stock run
likewise observed 19 radio writes but none to secure/non-secure `FRC+0x054` or
its SET/CLR/TGL aliases; the temporary trace was discarded after inspection.
That bounds the currently stalled pre-provisioning path only—it does not prove
the configuration selected by a fully provisioned stock boot.

The local archive and public default are mechanism/reference evidence only,
not a substitute for that stock selection: its archive has SHA-256
`c94fa52d538db6faa9150a327edff04f00980947897e7c7d09d12686190477fb`, while
the stock image is a distinct stripped artifact. The public default leaves
`REMOVE_APPENDED_INFO` clear (and does not request CRC storage), but stock may
override it. In the archive's clear-option path, `0x1b` selects public
`TRAILRXDATA` fields RSSI, CRCOK, PROTIMER CC0 wrap-low/wrap-high, and
`RTCSTAMP` (the generated rev-B1 xG22 model names these six one-bit fields at
`host_py_rm_studio_internal/full/efr32xg22/revB1/FRC_NS_field.py:978-1038`; the
register is `FRC_NS.TRAILRXDATA` at `+0x054`, reset `0`, writable mask
`0x3f`, in `FRC_NS_register.py:498-507`). This is a stronger register-layout
fact than the earlier abbreviated field list, but the generated names do not
define what event populates any field or how the timestamp is scaled. In
particular, it does not establish that `RTCSTAMP` is an EVENT_TMR value or
that the PROTIMER CC0 fields identify packet end. None of those defaults or field names establishes the
fully provisioned stock firmware's actual trailer policy.

Archive call tracing adds no BLE-specific selection. Aside from the public
`RAIL_ConfigRxOptions` entry, the only direct archive caller of that helper is
generic `rail_rf_hal.o:sli_rail_f70cad…` at archive `0x13580`: it passes mask
`0xffffffff` and the existing handle `+0x90` option value. That path is reached
through generic `rail_rf.o:sli_rail_7e61…` at `0x14e196` from
`sl_rail_init`/`RAIL_Init`; no relocation from `rail_ble.o` or
`rail_ble_rf_hal.o` reaches the helper, and the local xG22 BLE configurator
source has no RX-option/appended-info entry. Public headers define
`RAIL_RX_OPTIONS_DEFAULT` as all options disabled (`rail_types.h:5691-5697`),
but the generic call's existing-handle argument does not prove its initialized
value or the stock image's setting.

The descriptor-ring initializer distinguishes the CPU's lifecycle work from
record production. `rfhal_bufc.o:sli_rail_6281eecc9c4ea6e1ecc523be2e1d48d2`
is archive `[0x33740, 0x337c8)` (member `0x332f4`, section file offset
`0x44c`). It locks the active handle, derives a `log2(size)-6` encoding from
handle `+0x38`, gives BUFC the ring base from `+0x18`, and writes channel-2
registers `0xBA0000B4 = 1`, `+0x08c = log2(size)-6`, `+0x090 = ring_base`,
and `+0x0b0 = 7`; it clears BUFC flags `0x50000` and resets the software
indices at `+0x3a` and `+0x3c`. It has one narrow, software-produced
exception: `rfhal_bufc.o` section
`.text.sli_rail_893fd72dfcf6ebbe2598a01d375fa2c0`, archive
`[0x33390,0x333e0)` (object `0x9c..0xec`), handles `BUFC.IF.BUF2OF` bit 16.
It W1C-clears that bit, clears its IEN through the alias, then writes this
eight-byte record to buffer-2 `WRITEDATA`: `[BUF1.WRITEOFFSET low,
BUF1.WRITEOFFSET high, 0x04, 0, 0, 0, 0, 0]`. The compatible public BUFC
layout identifies the source as buffer 1 `WRITEOFFSET` and the destination as
buffer 2 `WRITEDATA` (`efr32bg21_bufc.h:42-56,290-304,379-393`). This directly
corroborates class 4 as `RX_FIFO_OVERFLOW`; it establishes only that overflow
record, not normal packet, aborted, or filtered producer timing/fields. No
other direct buffer-2 `WRITEDATA` write occurs in that object. An archive-wide
`arm-none-eabi-objdump -d -r` scan of the identified SHA-256 archive likewise
finds no other `STR*` at `[BUFC base,#0xa4]`, direct/alias port literal, split
`#0xa4` construction, or BUFC-port relocation; the eight stores above are at
section offsets `+0x26,+0x2a,+0x30,+0x36,+0x3a,+0x3e,+0x42,+0x46` (archive
`0x333b6,0x333ba,0x333c0,0x333c6,0x333ca,0x333ce,0x333d2,0x333d6`). This
bounds the archive's statically evident CPU writers, but not arbitrary runtime
address construction, hardware/sequencer production, or another binary. Thus
BUFC hardware (possibly with the sequencer) remains the candidate normal
producer, while the observed CPU paths configure, select, hold, release, and
synthesize this one overflow record.

The channel distinction is source-backed rather than a modelling convention.
The compatible public BUFC layout has its first buffer at `+0x00c` and a
`0x40` stride (`efr32mg21_bufc.h:45-58`), placing channel 2 at `+0x08c` and
its command/threshold registers at `+0x0b4/+0x0b0`. The initializer above
therefore configures channel 2 as the descriptor ring. In contrast, the
captured sequencer PIO target `0xAA000064` is channel 1 `WRITEDATA`
(`+0x04c + 0x018`), the separate payload-byte FIFO. The vendor FRC handler
routes BUFC bit 18 (`0x40000`) to the descriptor helper; the compatible public
header names it `BUF2THR` (`efr32mg21_bufc.h:290-309`). It later clears only
the sparse threshold positions `2,10,18,26` together. QEMU consequently
keeps channel-1 payload storage and channel-2 descriptor-ring register latches
separate, with a focused regression for the documented channel-2 setup. Its
in-process RF-medium test seam may additionally enqueue a record only after a
guest configures channel 2 with a nonzero encoded capacity and backing address;
that conditional enqueue is explicitly a QEMU convention, not a model of the
unidentified hardware descriptor producer.

The archive also bounds exactly what a normal producer would need to supply,
without identifying that producer. `RAIL_GetRxPacketInfo` at archive
`[0x15be30,0x15be34)` reaches the eight-byte-ring consumer at
`[0x33a5c,0x33bc0)`. It rejects descriptor word 0 values `>= 0x4000`, uses its
low 13 bits as a channel-1 circular-FIFO endpoint, and combines that endpoint
with channel-1 `READOFFSET` and `STATUS` to return one or two payload spans.
Descriptor byte 2 low three bits become the reported packet-status byte and
byte 3 becomes packet metadata; it does not parse a BLE header, CRC, or PDU.
`RAIL_ReleaseRxPacket` later advances the same software ring and writes the
endpoint back to channel-1 `READOFFSET`. These consumer facts establish the
minimum record shape, but not when a record is valid, its normal status values,
or a safe QEMU condition for synthesizing one.

The public RAIL 3 API independently gives this ring a narrower role. Its
`sl_rail_packet_queue_entry_t` is an opaque `uint64_t` receive Packet Queue
entry, explicitly separate from RX-FIFO payload data and retained in packet
arrival order (`sl_rail_types.h:2048-2055`). The configuration contract says
enabled packet-completion events add an entry and that callback return or
`sl_rail_release_rx_packet()` removes it (`sl_rail_types.h:2305-2322`);
`sl_rail.h:2222-2268` describes the queue as circular per-packet metadata.
The eight-byte stock ring and separate channel-1 FIFO therefore match that
public abstraction. The entry remains opaque, however, so this compatible
API-level correlation does not prove its xG22 field encoding or producer
trigger.

The QEMU-only successful-RX convention is deliberately smaller than a silicon
claim. Once its in-process medium has copied all offered bytes to BUFC channel
1, and only when that explicit channel-2 configuration is present, it writes
one eight-byte channel-2 record before it raises QEMU's already-synthetic FRC
`RXDONE`: byte 0 is the low byte of the resulting channel-1 write endpoint,
byte 1 carries endpoint bits 8..12, byte 2 is `0x06`, and bytes 3..7 are zero.
The endpoint packing and byte-2 class position are grounded in the stock
consumer at archive `[0x33a5c,0x33bc0)` above; the stock class-dispatch table
at `flash-stock.bin` file offsets `0x28850..0x28856` maps classes 6 and 7 to
the received-packet event. Selecting class 6, zeroing byte 3 and the reserved
tail, making configured channel 2 the enable condition, and ordering the
record before synthetic `RXDONE` are QEMU test conventions only. In
particular, they establish neither silicon producer timing, normal metadata,
BLE framing, CRC/trailer state, nor an IRQ/completion contract. The native
dual-SoC qtest exercises both paths: an unconfigured channel 2 remains
raw-byte-only, while the configured path observes the record and threshold
before it observes `RXDONE`; it is a regression for this simulator boundary,
not hardware validation. If the configured 512-byte descriptor backing ring
has no room for another eight-byte record, QEMU sets the channel-2 overflow
flag and drops the synthetic receive before touching channel 1 or raising
`RXDONE`; this is a conservative simulator policy, not an inferred silicon
recovery rule. The overflow regression accepts 64 records, rejects the 65th,
then verifies that a record after explicit ring consumption uses only its own
channel-1 endpoint.

The public RAIL archive adds a narrow runtime observation for sync-word programming. In `librail_efr32xg22_gcc_release.a(rail_ble_rf_hal.o)`, section `.text.sli_rail_51695f5143b2a4311e2362c17f8fac96` starts at object file offset `0x334`. At section-relative `+0x4a..+0x5c`, it constructs a three-word configuration record with target `0xA8014068` and a source word loaded from its third argument at `+0x0c`. The generated model names that 32-bit target `MODEM.SYNC0.SYNC0` (`MODEM_S_field.py:1167-1174`); the public BLE table independently labels the same offset `MODEM.SYNC0` (`ble_efr32xg22_configurator_out.c:275-276`). QEMU retains this register and its 32-bit public mask.

The OpenISA LLL supplies the API meaning and byte order that the archive alone does not: its advertising preparation creates `uint32_t aa = sys_cpu_to_le32(PDU_AC_ACCESS_ADDR)` at `lll_adv.c:321`, then calls `radio_aa_set((uint8_t *)&aa)` at `:359`; scan uses the same conversion at `lll_scan.c:127,170`. The reference OpenISA HAL treats its argument as the access address (`hal/RV32M1/radio/radio.c:687-704`). Together these public sources justify `radio_aa_set` writing the four supplied bytes, little-endian, to `MODEM.SYNC0`. The EFR32 HAL assembles the word byte-wise so that it does not impose an unadvertised alignment requirement on this byte-pointer API. This establishes only the configuration write; it does not establish when the sequencer consumes it or any sync/demodulation behavior.

The public BLE 1M base table also configures named MODEM control, bitrate, sync, shaping, threshold, antenna-switch, and BLE-IQ registers (`ble_efr32xg22_configurator_out.c:271-309`). QEMU retains the public rev-B2 masks for those table targets (including `MIXCTRL +0x01C`, `CTRL0..6 +0x020..+0x038`, `TXBR/RXBR +0x058/+0x05C`, `SYNC0/1 +0x068/+0x06C`, and the later RX/antenna controls). Its mask source is `MODEM_S_register.py` at the register-class locations listed in its public model, not inferred from the table values. This preserves visible configuration while still assigning no demodulation, sync matching, or antenna behavior.

The BLE 1M per-PHY delta supplies the remaining MODEM shaping, Viterbi, long-range, and BLE-IQ control values (`ble_efr32xg22_configurator_out.c:474-529`). QEMU retains their public masks too—e.g. `SRCCHF +0x0EC` mask `0xEFFFF8FF`, `DSACTRL +0x0FC` mask `0xFFEFFFFF`, `DIGMIXCTRL +0x10C` mask `0x007FFFFF`, and `VTBLETIMING +0x170` mask `0x0000FFF3`. These remain register latches only; this evidence does not establish their data-path semantics.

That same archive section gives a direct whitening setup: at file offsets `0x3ea..0x3f2` (section-relative `+0xb6..+0xbe`) it forms `FRC.WHITEPOLY = 0x44` at `0xA8004034`; at file offsets `0x404..0x40c` (`+0xd0..+0xd8`) it ORs bit 6 into an input byte and stores the result as the data word for `FRC.WHITEINIT` at `0xA8004038`. The generated xG22 model confirms 16-bit writable `WHITEPOLY` and `WHITEINIT` fields at those offsets (`FRC_S_register.py:293-328`). Zephyr's LLL calls `radio_whiten_iv_set(chan)` after its BLE channel selection (`lll/lll.c:445`), and its OpenISA reference HAL expresses the BLE seed as `iv | 0x40` (`hal/RV32M1/radio/radio.c:653-681`). Therefore `radio_whiten_iv_set` now accepts channel `0..39` and writes `WHITEPOLY=0x44`, `WHITEINIT=0x40|channel`. This records configuration only; QEMU does not yet whiten packets and the HAL has not inferred FRC sequencing.

For BLE 1M, the public table writes `RFCRC.CTRL = 0x708`, `RFCRC.INIT = 0xAAAAAA`, and `RFCRC.POLY = 0xDA6000` (source: `ble_efr32xg22_configurator_out.c`, lines 268–270). Zephyr passes the Bluetooth CRC polynomial `0x00065B` and advertising IV `0x555555`; reversing their 24 significant bits produces exactly those public-table values. `radio_crc_configure` now enables RFCRC and writes that public control value plus bit-reversed polynomial/IV. QEMU supplies the public RFCRC register masks. It intentionally does not issue the standalone `RFCRC.CMD.INITIALIZE` command or claim packet CRC validity: the FRC/sequence data path is still uncharacterized.

AGC's live `RSSI +0x018` and latched `FRAMERSSI +0x01C` have reset raw value `0x8000` and a public read-only mask of `0x0000FFC0` (`revB2/AGC_NS_register.py:116-149`). The generated fields supply the bit positions but not units. The vendor resolves both the conversion and the appropriate packet-time source: `RAIL_GetRxIncomingPacketRssi` branches to `sli_rail_40a4a663e9540c5b085d11f994ab5fe0` in `rfhal_radio.o`, whose 32-byte section begins at archive file offset `0x3fd4c`; it reads `FRAMERSSI` at `0xB800C01C`, masks `0xFF00`, and calls `sli_rail_d0c1b6aa0db6742c099aa9a06dbabf70` at archive file offset `0x3fc7e`. That helper sign-extends the 10-bit field at bits 6..15, treats `-512` as invalid, subtracts 200 quarter-dB, and clamps at `-511`; the caller rounds negative values by adding three before extracting the signed dB result. The HAL follows that conversion and returns its positive magnitude, matching OpenISA's reference `radio_rssi_get()` contract (`hal/RV32M1/radio/radio.c:957-960`) and its `0x7f` unavailable RSSI convention (`lll_scan.c:1122-1126`). The public AGC model names write-only `CMD.RSSISTART` at `+0x04C`, bit 0 (`AGC_NS_register.py:458-472`, `AGC_NS_field.py:997-1005`), so `radio_rssi_measure()` now emits exactly that command. It also names bit 0 of read-write `IF +0x044` as `RSSIVALID`, with the same bit in `IEN +0x048` (`AGC_NS_register.py:398-439`, `AGC_NS_field.py:837-924`); the HAL polls and clears that latch for OpenISA's readiness/status API. QEMU accepts and discards the write-only command and retains both public latches and their aliases, but does not synthesize an RSSI sample or assertion of `RSSIVALID` without an RF input.

## 4.3 PROTIMER access pattern [F]
The stock-image evidence also identifies the immediately preceding CMU path without
turning it into a radio-state rule. `flash-stock.bin:0x26e0c..0x26e22` reaches the
clock-source decoder at `0x1baf0..0x1bb10`; its `0x1bb74` literal is
`0x40008070` (`CMU.SYSCLKCTRL`). That helper separately reads
`0x27258 = 0xb0004fe0` and tests bit 16. The public BG22 header names
`SYSCLKCTRL.CLKSEL` bits `0..2` and `RHCLKPRESC` bit 16
(`efr32bg22_cmu.h:663-708`; the `SYSCLKCTRL` register layout is at `:63`).
The stock PROTIMER setup itself is at `flash-stock.bin:0x29974..0x29a36` and
uses literal `0x29d54 = 0xa801c008`; `radio-trace.log:64-67` independently
records its first four writes (`+0x008 = 0x00011102`, `+0x030 = 0x00001233`,
`+0x038 = 0x80015558`, `+0x00c = 1`). These are access/control-flow and
public-layout facts. They do not prove CMU latency or gating, PROTIMER command
semantics, or any RAC transition timing/gating relationship.

Direct stock control flow further bounds the early command helpers. At
`flash-stock.bin:0x29a3a..0x29a50`, the enable helper clears and enables NVIC
bank 1 bit 4 through `0xe000e284`/`0xe000e104`, then writes `1` to
`PROTIMER +0x00c`; external IRQ 36 is that bank/bit according to
`efr32bg22c224f512im40.h:99-101`. The matching stop helper at
`0x29a52..0x29a72` clears/disables the same NVIC bit before writing `4` to
`+0x00c`. This proves the vendor pairs those command values with IRQ36
masking, not which command enables, clears, or causes an interrupt.

The public BG22 integration sources independently establish only the fixed
wiring around that interrupt. The xG22 devicetree assigns the radio node's
`protimer` interrupt name to external IRQ 36
(`vendor/zephyr/dts/arm/silabs/xg22/efr32xg22.dtsi:11-29`), and the Series-2
installer connects that vector to `PROTIMER_IRQHandler`
(`vendor/zephyr/soc/silabs/silabs_s2/soc_radio.c:21-31`). The device header
names CMU `CLKEN1` bit 4 as the PROTIMER bus-clock gate
(`efr32bg22_cmu.h:586-590`), while its PRS table names output signals
`PROTIMERL_CC0..CC4` and `PROTIMER_T0MATCH`/`T1MATCH`
(`efr32bg22_prs_signals.h:637-651`). These sources establish vector, clock,
and signal names only. They do not map the observed `+0x100` slots to those
signals, define an IF/IEN bit, or establish when a compare result asserts
IRQ36 or reaches RAC/FRC; QEMU must not turn this wiring evidence into an
expiry model.

The stock scheduler at `flash-stock.bin:0x29ab4..0x29b9c` also proves the
layout of four indexed 16-byte slots: it writes a computed time to
`0xa801c100 + 0x10*index + 0x0c` and `0x11` at `+0x0`; its conditional helper
at `0x29a92..0x29ab0` writes `1` at the associated
`0xa801c300 + 0x10*index` word. The initialization caller at
`0x27f84..0x27f98` configures slots 0 and 1 only. Neither path reads a
completion/status register or demonstrates that a slot match raises IRQ36,
starts RAC/FRC, or has a particular field meaning. The model therefore keeps
these as latches/slots rather than inventing a timer expiry path.

The public xG22 RAIL archive's `rfhal_protimer.o` accesses the non-secure
window at `0xB801C000`, confirming both the base and its alias relationship
with the trace's secure `0xA801C000` window. The member data starts at archive
file offset `0x3bf34`. Its 28-byte
`.text.sli_rail_2867cf1f7327fd002fccf5d8ded311be` section starts at
`0x3c050` (object offset `0x11c`) and writes command `1` to `+0x00c`; the
36-byte `.text.sli_rail_daa5e20a02fa3083d34766f4d0c794f3` at `0x3c06c`
(object offset `0x138`) writes command `4`. Both first program the NVIC before
the command write. The 20-byte `.text.sli_rail_18d8f9306bac0be367395437e84b7c36`
at `0x3c0b4` writes either `0x20` or `0x80` to that same command register,
and the 24-byte `.text.sli_rail_4aff3cd2f4ff81ffa9da09f59bb74c75` at
`0x3c48c` writes `0x10000` after its PTI auxiliary-data call. This identifies
the command values and vendor-visible window, not their names or effects.

The object uses `+0x030` and `+0x038` as timer state/period inputs; and
accesses four 16-byte compare slots starting at `+0x100` (with associated
control words at `+0x200`). It also drives `RAC_RSM_IRQHandler`, so timer
behavior is part of the event path. QEMU maps the secure and non-secure
PROTIMER windows to a shared named latch state; its focused regression writes
the RAIL non-secure `CTRL`/`CMD` path and observes `CTRL` through both windows.
This preserves the observed alias behavior without claiming timer progression,
compare, capture, IRQ, or command semantics.

The 432-byte archive helper at `rfhal_protimer.o:0x3c140`
(`.text.sli_rail_30b95b9e98ec0997b2a8f05421916f4a`, object section offset
`0x20c`) makes the slot access pattern more specific. For `index` 0 through
3 it stores a computed value at `0xB801C10C + 0x10*index` and `0x11` at
`0xB801C100 + 0x10*index`; on one branch it writes `1` through the standard
CLR alias at `0xB801E100 + 0x10*index`. The latter is an alias access to the
same `+0x100` slot word, not evidence for an independently named activation
register. That same clear path writes `0x100 << index` through the CLR alias
at `0xB801E064`; this records an additional slot-indexed mask access, not an
IF/IEN interpretation. These are exact accesses and values only: this helper
has no relocation or call to FRC, RAC, or an IRQ handler (only local
arithmetic, critical/atomic, and PTI support), so it does not prove the timer
comparison condition, a command meaning, an expiry flag, or an IRQ side
effect.

The archive also directly constrains the interrupt dispatch path. In
`generic_phy.o`, whose ELF begins at archive offset `0x518c8`,
`.text.PROTIMER_IRQHandler` is the four-byte payload at `0x52f44..0x52f47`
and has an `R_ARM_THM_JUMP24` relocation to `FRC_IRQHandler`; the target
handler begins at `0x52760`. That is a direct branch, not evidence for a
particular expiry source or flag. The target handler reads and acknowledges
`FRC.IF/IEN` at `0xB8018068/+0x06c`, then reads BUFC pending/enable at
`0xBA000114/+0x118`, before processing radio state. Thus any eventual native
PROTIMER event model must join the shared FRC-facing event path rather than
invent a timer-only guest callback. It remains unknown which PROTIMER status
condition, if any, produces which FRC-visible event, so QEMU deliberately does
not yet synthesize an IRQ36 assertion or compare expiry from this fact.

Further disassembly makes the PROTIMER half of that shared path concrete. At
archive offsets `0x527c0..0x527cc`, the handler reads PROTIMER `+0x014`,
`IF +0x064`, and `IEN +0x070`, then computes `IF & IEN`; at
`0x527d2..0x527e8` it acknowledges handled timer bits through the standard
CLR alias at `0xB801E064`. The handler later passes timer-derived aggregate
masks together with enabled FRC pending bits to its dispatcher at `0x52dda`;
the callee is at archive `0x51aac` and masks these against registered event
masks before calling the common RAIL event path. QEMU therefore models the
observed PROTIMER `IF`/`IEN` latch bank and drives IRQ36 only from their
intersection, with normal/SET/CLR/TGL aliases shared between secure and
non-secure windows. It deliberately leaves all flags inert: no counter edge,
compare slot, FRC flag, RAC command, packet event, or guest callback is
synthesized. Static code observes timer bits `0x10`, `0x10000`, `0x01000000`,
`0x02000000`, `0x08000000`, and `0x20000000`, but does not name them or map
them to slots, so they remain evidence of a pending-bank dispatch—not a timer
semantics definition.

The public xG2x RAIL header documents `RAIL_TimerTick_t` as a counter whose
tick is roughly 0.5 microseconds and whose wrap range is about 17 minutes
(`rail_chip_specific.h:272-328`). The locally retained xG22 RAIL archive's
`RAIL_GetTime` path reads non-secure `PROTIMER +0x020`: its shared helper is
at archive byte offset `0x3c5c8` and calls the atomic counter reader at
`0x3c598`. QEMU consequently exposes a resettable 32-bit counter at that
offset, shared with the secure window.  It is driven by its own modeled 2 MHz
`PROTIMERCLK` input and QEMU's `ptimer` (rather than the CPU clock or host
time); its regression advances virtual time just beyond 1000 ns and observes
two ticks, then verifies reset returns it to zero. A companion regression
runs the same interval with the separate BURTC clock enabled, confirming that
PROTIMER remains driven by `PROTIMERCLK` rather than BURTC. These are emulator
timebase approximations only. They are not interchangeable with the
OpenISA HAL's `radio_tmr_start(trx, ticks_start, remainder)`: the reference
port first adjusts a coarse RTC tick and a microsecond remainder, then
programs its own RTC comparator (`hal/RV32M1/radio/radio.c:1026-1126`). The
vendor's 232-byte PROTIMER setup helper at archive offset `0x3bf68`
(`.text.sli_rail_a2bb09bbf0f5a66273c06b6551b0acd9`; object offset `0x34`)
computes several scaled values before writing `CTRL=0x00011102`, `STATE`
(`+0x030`), and `PERIOD` (`+0x038`) through the non-secure window. Its
432-byte compare helper begins at `0x3c140` (object offset `0x20c`) and also
coordinates FRC, sequencer RAM, and RAC accesses. Thus neither helper proves
a conversion from the OpenISA RTC/remainder pair to PROTIMER ticks, nor a safe
TIFS comparator sequence.

The archive does prove that scheduled operations are deferred rather than
started inline by their API wrappers. The scheduled-TX path reaches the common
compare helper with slot index 3 at archive offset `0x521a8`
(`generic_phy.o` begins at `0x518c8`); the scheduled-RX path reaches the same
helper with index 2 at `0x51fe0`. The 432-byte helper at `0x3c140` records the
target at `PROTIMER +0x10c + 0x10*index`, writes `0x11` at
`+0x100 + 0x10*index`, and applies an index-derived mask through the `IF` CLR
alias. Each wrapper then returns. This is evidence for deferred timer-slot
programming, not for a proven autonomous PROTIMER-to-RAC/FRC launch route.

The Zephyr port consequently uses its existing free-running BURTC ticker
counter as a single alarm source: the ordinary LLL ticker compare and one
pending radio-launch compare are multiplexed to the earlier wrap-safe Counter
deadline. In the QEMU profiles that same Counter-owned BURTC is also Zephyr's
tickless system clock, so its combined driver is the sole `COMP`/IRQ18 owner
and arbitrates the Counter deadline against the kernel deadline. The Counter
ISR only records the radio expiry and wakes a pre-created TX or RX Zephyr
thread; those persistent threads run the normal radio enable path only when
selected by the kernel scheduler. Disable/reset disarms the deadline and
invalidates its generation, but does not destroy the workers. This is an
intentional scheduler integration choice, rather than a claim that it matches
the unresolved vendor PROTIMER semantics. The focused QEMU regression
`tests/burtc-arbitration-port` verifies that a Counter alarm and a kernel
sleep complete through this single comparator owner.

The retained stock trace records the concrete early configuration sequence: `radio-trace.log:64-73` writes `+0x008 = 0x00011102`, `+0x030 = 0x00001233`, `+0x038 = 0x80015558`, `+0x00c = 1`, `+0x100 = 0x01000003`, `+0x100 = 0x03000003`, `+0x110 = 0x00200003`, and `+0x060 = 0`; `:194-210` additionally reads `+0x130` and writes `+0x074/+0x078`, command `0x40000`, `+0x140`, and `+0x064`; `:301` reads `+0x120`. QEMU therefore gives this observed register set an explicit `PROTIMER` named latch model, with a regression for the exact trace values. These are access facts only: register field names, the correspondence between RAIL timer ticks and the raw PROTIMER counter, capture/compare semantics, command effects, IRQ behavior, and safe TIFS sequence remain unresolved, so neither the model nor the HAL synthesizes timer behavior.

## 5. RFSENSE / selective OOK (planned capability, not the BLE path)

RFSENSE is retained here as a documented, future capability. It is not part of
the current QEMU or Zephyr startup/RX/TX priority. The evidence-backed address
map is:

| item | value | status and evidence |
|---|---:|---|
| non-secure base | `0x5009C000` | **[F]** RFSENSE register-window base from the stock/model evidence; keep this separate from the secure alias `0x4009C000`. |
| secure base | `0x4009C000` | **[F]** secure alias of the same peripheral window. Secure/non-secure selection is an access-policy distinction, not a second detector. |
| `CTRL` | `+0x000` | **[F]** control register; detector mode and wake behavior belong here. |
| `CMD` | `+0x004` | **[F]** command register; command effects are not yet characterized. |
| `STATUS` | `+0x008` | **[F]** status register; no packet/RX interpretation is implied. |
| `IF` / `IEN` | `+0x00C` / `+0x010` | **[F]** interrupt flag/enable pair; the eventual wake/IRQ policy remains open. |

The BG22 device header assigns `RFSENSE_IRQn = 40` in
`/Users/gavin/Projects/zephyr-modules/hal/silabs/simplicity_sdk/platform/Device/SiliconLabs/EFR32BG22/Include/efr32bg22c224f512im40.h:104`;
the same IRQ ordering is recorded in `docs/zephyr-ll-port.md:40-44`. The clock
gate is `CMU_CLKEN1_RFSENSE`, bit 14 (`0x4000`), in
`/Users/gavin/Projects/zephyr-modules/hal/silabs/simplicity_sdk/platform/Device/SiliconLabs/EFR32BG22/Include/efr32bg22_cmu.h:636-640`.
The stock image has the IRQ40 vector-table word at
`evidence/stock/stock-7.9.07.bin+0x000000e0`; its little-endian value is
`0x000496e1`, a Thumb entry at `0x000496e0`. This establishes a nonzero
RFSENSE vector target, not an observed detector event or the handler's
register protocol. The ignored local `scratch-radio.tgz` archive contains only
`radioram.dump` (0x8000 bytes); an exact 32-bit little-endian scan finds neither
`0x4009c000` nor `0x5009c000` at any archive-member offset. The stock image
likewise contains neither literal. Absence of those literals is not evidence
that the firmware does not use RFSENSE (addresses can be synthesized), so no
register semantics are inferred from the scan.
The generated device header does not provide a BG22 RFSENSE register struct;
the base/offset table above therefore records the verified address evidence
without pretending that a complete public register model exists.

RAIL exposes this as a separate feature/API boundary: the public RAIL types
define `RAIL_RfSenseBand_t` and the selective-OOK configuration in
`/Users/gavin/Projects/zephyr-modules/hal/silabs/gecko/platform/radio/rail_lib/common/rail_types.h:3770-3825`,
and the API contract is declared by `RAIL_ConfigRfSenseSelectiveOokWakeupPhy`
and `RAIL_SetRfSenseSelectiveOokWakeupPayload` in
`/Users/gavin/Projects/zephyr-modules/hal/silabs/gecko/platform/radio/rail_lib/common/rail.h:4860-4898`.
The checked-in configurator profile is the 1-kbps OOK profile in
`/Users/gavin/Projects/silabs-simplicity-sdk/platform/radio/rail_lib/chip/efr32/protocol_config/efr32xg22/rfsense_ook_efr32xg22_configurator_out.h:35-50`.
These APIs configure low-energy RF detection/wake; they do **not** establish
BLE packet RX, the Zephyr LLL receive-delivery contract, or ordinary TX.

Energy-mode semantics are also distinct. The BG22 data-sheet description says
RFSENSE can wake from EM2, EM3, or EM4 and that selective mode performs OOK
preamble/sync detection (supplemental: [Silicon Labs BG22 data sheet](https://www.silabs.com/documents/public/data-sheets/efr32bg22-datasheet.pdf)).
An EM4 wake is a reset/restart boundary for the M33 application, not a normal
RX interrupt that leaves the BLE controller running. Consequently, do not
model RFSENSE as ordinary packet reception or claim that a wake preserves
Zephyr controller state.

Non-claims and next scope: no RFSENSE detector thresholds, OOK correlator,
payload semantics, wake-cause retention, interrupt-clear behavior, or
secure/non-secure arbitration has been validated on hardware. No QEMU model,
Zephyr hook, or RAIL call is being added by this note. When this capability is
scheduled, first add a focused register/IRQ test for the address aliases and
`CLKEN1` gate, then validate EM4 reset/wake behavior and only afterward decide
whether a RAIL-facing wake API is needed.

## 6. Shortest path to advertising in the QEMU radio model
1. **Partially done in QEMU:** BUFC has the trace-corrected programmed-data path and threshold IRQs; RAC uses the public xG22 register/command/state map and distinct RAC_RSM/RAC_SEQ interrupt banks. Focused regressions cover both models. BUFC DMA via `BUF.ADDR`, packet socket/pcap output, and real sequencer timing remain unimplemented.
2. Let stock firmware run to PHY configuration (BLE advertising start) under the model and log the writes; compare to the public tables to decode `ty` (T3) and to label MODEM/FRC registers (T5).
3. Implement T1/T2/T6/T8 in `hal/radio.c` for ADV_IND TX only (no RX, no TIFS), drop the `STUB_REGISTERS` asserts for `radio_tx_enable`/`radio_tmr_start`.
4. Then RX window for SCAN_REQ/CONNECT_IND (T9, T5), then software TIFS (T8).

Hardware validation is a separate, later step and must start with a sniffer and a dummy load, never the suit's output stage.

## 7. Stock sequencer static decomp and the `ACTIVETXFCD` divergence (2026-10-27) [F]

### 7.1 Provenance

`stage2.5-sdk-sequencer/generic_efr32xg22_release_seq.o` (SHA-256
`b57b5fa7...`) is a Cortex-M0+ object extracted in an earlier session; its
origin was not re-verified in this pass, but `sl_rail_util_sequencer.c`
(Zlib-licensed, same tree already cited elsewhere in this file) shows the
shipped RAIL library selects a sequencer image via
`sl_rail_load_sequencer_image_0/1()`, consistent with a loader-supplied
microcode blob rather than source compiled by the application. Decisive
independent corroboration: `cmp` against `tests/hardware-lockstep/`'s
live AP3 capture of the actually-running SEQRAM
(`stage2-seq-frcram-2a2c.bin`, SHA-256 `096de38f...`, 20480 bytes) shows the
two are byte-identical except for one byte within the reset-vector word
(object `0x2a2c`, live `0x2ade`; both odd/Thumb). Every address and
instruction cited below is read directly from these two files; no literal
disassembly text is reproduced, per the house rule at the top of this file.

### 7.2 The gate-pulse and SEQIF-ack helpers (fully resolved)

The vector table (word 0 = `0x3e30`/`0x3e3e`-ish stack top, matching this
project's own `EFR32_SEQ_STACK_TOP`) places `IRQ8` at `0x2578` and `IRQ9` at
`0x25c8`, matching the independently live-observed `0x2578` already recorded
in `tests/sequencer-probe/README.md`.

Two small subroutines, each reached by `bl` from IRQ8, resolve completely:

* **`0x7ac`** writes bit 18 (`0x00040000`) to `0xA8021040` (the SET alias of
  a base register at `0xA8020040`), then busy-waits for bit 2 of
  `0xA8020040` itself to clear. This is exactly the gate pulse
  `tests/sequencer-probe/probe.S` already names `OBS_IRQ8_GATE_SET` /
  `OBS_IRQ8_GATE_BASE` / `OBS_IRQ8_GATE_BIT`, now with its wait condition
  resolved: it is a busy-wait on bit 2 of the *base* (non-aliased) register,
  not a fixed delay.
* **`0x7be`** writes the same bit 18 through the CLR alias `0xA8022040`
  (`OBS_IRQ8_GATE_CLR`) and returns immediately — no wait.
* **`0x7c8`** is a general-purpose helper, called with a mask in `r0`: it
  clears that mask in `RAC.SEQIF` via the CLR alias `0xA8022028` (this
  project's `EFR32_RAC_SEQIF_CLEAR`), then busy-waits until `RAC.SEQIF`
  (`0xA8020028`, `EFR32_RAC_SEQIF`) reads clear on that same mask. IRQ8 calls
  it with `r0 = bit 24 (0x01000000)` as its last action before returning.
* **`0x1bda`** performs exactly three unconditional CLR-alias writes,
  already named by `probe.S` as `OBS_IRQ8_CLR_2044`/`_2040`/`_2008`: clear
  bits `{0,1}` of `0xA8020044`, bits `{3,5,7}` of `0xA8020040` (the *same*
  base register as the gate, different bits), and bit 4 of `0xA8020008`, all
  via their CLR aliases at base `0xA8022008`.

IRQ8's top-level body is: `bl 0x7ac` (gate+wait) → write `5` to the SET alias
of a fifth, still-unnamed RAC register at `0xA8020000+0x3C` (`0xA802113C`) →
`bl 0x7be` (gate clear) → `bl 0x894` (§7.3) → `bl 0x1bda` (three clears) →
clear-and-wait `RAC.SEQIF` bit 24 via `0x7c8`. IRQ9's body opens with the
*same* `bl 0x894` call, so `0x894` is a shared "RAC service" routine, not
IRQ8-specific.

**Official names, from the public register model, not more disassembly:**
fetched Silicon Labs' own `RAC_S_register.py`/`RAC_S_field.py`
(`build/reference-models/`) and found `0xA8020040`/`0xA8020044` are
`RAC.SR0`/`RAC.SR1` — and, per that same model, **both are documented as
generic 32-bit read-write scratch words, reset value `0`, with no
hardware meaning assigned at all**. So the "gate pulse"/"busy-wait" this
section already fully resolved and ported into production `radio_seq.S`
is not a documented hardware feature; it is a **firmware convention**
stock's own code invented, using otherwise-blank scratch registers as a
software semaphore between whichever two logical halves of the system
coordinate through it. `radio_seq.S` has been updated to use the
official `SR0`/`SR1` names throughout (previously `TXWARM_GATE_BASE`/
`0X44_BASE`), with this distinction called out directly in its own
comments — this project still faithfully reproduces the exact bit
pattern stock uses (bit 18/bit 2 on `SR0`, bit 16/22 on `SR1`), it just no
longer implies that pattern is itself a silicon-defined gate. One real
naming bug caught in the process: a bit constant applied to `SR0`'s SET
alias (the second pulse in §7.3.1) had been named with an `0X44` (`SR1`)
prefix since whenever it was first added — cosmetic, the correct address
was always used at its one call site, but now corrected to
`EFR32_RAC_SR0_SECOND_PULSE_BIT` to match where it's actually written.
The fifth register at `0xA802113C` (SET alias of `0xA8020000+0x3C`) is
also no longer unnamed: it's `RAC.PRESC`'s SET alias — a real, documented
prescaler-control register, though which of its fields the written value
`5` actually sets has not been traced.

### 7.3 `0x894`/`0xd38`: a stateful dispatcher, not a store sequence (partially resolved)

`0x894` reads ARM core `SCB->ICSR` (`0xE000ED04`, bits `[31:23]`) and
`NVIC->ISPR[1]` (`0xE000E200`, bit 15 → external IRQ 47's pending bit), and
dereferences **four SEQRAM-local addresses never written by this object
itself**: a function pointer at local `0x3854`, a flag word at `0x3eec`, a
pointer at `0x3fcc` (passed into a further callee), and a byte/word flag
pair at `0x3eb8`/`0x3f68`. It also performs a SET/CLR-aliased read-modify
sequence on a previously undocumented register pair at `0xA8005064` /
`0xA8006064` (§1 block map). Its unconditional tail calls `0xd38`, which is
itself a substantial (continues past 100+ instructions already read)
bit-flag combination routine over more local state plus `0xA8004000`-range
(FRC) register reads — not yet fully decoded. `0xd34`, reached from one
branch, is a bare `bx lr` (no-op) in this image.

This matters for *how*, not just *whether*, this project replicates stock
behavior: `radio_seq.S`'s own header already commits to "bounded peripheral
mechanics only... [not choosing] jobs, priorities, channels, deadlines."
`0x894`/`0xd38` is exactly the kind of job/channel-dependent policy that
commitment excludes.

**Checked, not just assumed: nothing writes the `0x3850`/`0x3854` function-
pointer slots, in any image this project has access to.** A raw byte scan
of the *entire* sequencer object (not just `fn_0x894`'s own body) for every
literal reference to these two addresses finds exactly one hit each, both
already inside `fn_0x894` itself — so no other sequencer function reads or
writes them either. Checking further, whether stock's own M33-side RAIL
library pokes these in during setup (the natural place for an M33→M0+
callback handoff, the same shape as the mailbox doorbell in §7.5.4): a raw
byte scan of `build-stage2/zephyr/zephyr.bin` (this project's known-working
stock M33 image) for the non-secure/secure SEQRAM-alias forms of both
addresses (`0xB0003850`/`0xB0003854`/`0xA0003850`/`0xA0003854`) finds
**zero** hits. This de-prioritizes, rather than strengthens, the earlier
hypothesis that this callback mechanism is the missing link to
`ACTIVETXFCD`: if no image this project can inspect ever initializes these
slots, the branch that reads them in `fn_0x894` is most likely dead on
stock too for this scenario, not a live path the open stack is failing to
service.

`0xd38` resolves further than first read suggested, with one correction:
a register tracked through many reassignments (GNU `objdump`'s literal-pool
annotations only show *where* a loaded constant lives, not its value, so
each one must be resolved and each register's *last* assignment before use
re-checked — an earlier pass here mistakenly treated a stale value as current
and guessed a `BUFC.BUF.WRITEDATA`-style byte-pump; re-tracing every
assignment shows that is wrong) turns out, at its last assignment before use,
to hold `0xAA000054` = `BUFC.BUF[1].XWRITE` (struct layout from the public
`efr32bg21_bufc.h`, `BUFC_BUF_TypeDef`/`BUFC_TypeDef`, already the project's
own header for this block). Three further, mutually exclusive branches from
that base write: `BUF[1].FIFOASYNC` (`+0x10`) twice, high/low halves of one
value, gated on bit 24 of a SEQRAM-local word; `BUF[2].READOFFSET` (`+0x20`),
value `8`, on an early-exit path; and `BUF[3].CMD` (`+0x6C`) twice with two
different values — the same "two sequential CMD writes" shape this project
already established for channel 0/1 FIFO setup — gated on comparing
`FRC.WCNT` (`+0x01C`, the live running word counter, not a `WCNTCMP*`
threshold) against a table-derived value. None of this has been confirmed to
execute for this project's single-descriptor BLE advertising case rather
than some other `generic_phy` scenario (RX, calibration, a different
protocol); that requires either exhausting the remaining static branches or
a temporary, non-production diagnostic probe exercising this exact call with
this project's real runtime state, neither of which has been done yet. It is also the concrete, hardware-proven reason a
blind register-store replay cannot work: `tests/sequencer-probe/README.md`
already records that replaying the *complete* captured IRQ8/IRQ9 MMIO
store sequence "still emitted no observable advertisement," and
`trace_stores.py --mmio-min 0xA0000000` structurally cannot record the four
SEQRAM-local reads above — a replay that only reproduces addresses
`>= 0xA0000000` cannot reproduce decisions keyed on addresses `< 0x5000`.
The gate-pulse/SEQIF-ack mechanics in §7.2 are safe to add to `radio_seq.S`
as named, bounded operations; `0x894`/`0xd38`'s dispatch logic should not be
ported wholesale.

### 7.3.1 `fn_0xd38` re-verified more rigorously: two real transcription bugs, and a second, unported gate pulse

Going back over `sequencer-decomp/src/fn_0xd38.c`'s previously
lower-confidence second half with the same rigor as `fn_0x894`'s already-
verified first half (re-resolving every literal-pool reference against
the raw capture rather than trusting the first pass) found two confirmed
errors and one genuinely new, significant lead:

* **Two swapped/misattributed register targets, confirmed wrong.** The
  `distance < 8` branch's two writes had their targets swapped
  (`RAC_0X18 = 6` and `OBS_BLOCK_AA001114 = BIT(16)`, not the reverse as
  first written); `RAC_0X18` is consequently a write target on this path,
  not read-only as its own comment originally claimed. Separately, the
  unconditional block just after the `0x81`-mask test had both of its
  addresses wrong too: resolving `0x10d4` and `0x10c0` directly against
  the capture gives `0xA8021040` (the TXWARM gate's own SET alias,
  `RAC_GATE_BASE_VAL_SRC+0x1000`) and `0xA8015020`, not
  `PROTIMER_0X0C+0x2000`/`MODEM_0X44-0x1c4` as first written. Both caught
  by re-deriving each literal from the raw bytes, not by compiling — the
  code compiled cleanly both times; wrong addresses don't trigger a
  compiler error.
* **A clobbered `flags` value on the `distance < 8` path.** `movs
  r5,#128` on that path unconditionally overwrites the same register the
  function's first half accumulates `flags` into, discarding whatever it
  held before. The `0x81`-mask test immediately after therefore always
  evaluates against a literal `0x80` on that path, not the accumulated
  value — reproduced by assigning `flags = 0x80U` in that branch rather
  than leaving it as an unrelated local.
* **The significant lead:** corrected, the unconditional block (reached
  whenever the `0x81`-mask test does not take the `far_skip_table` exit)
  pulses `0xA8021040` — **the exact same gate register `radio_seq.S`
  already pulses bit 18 of for the main TXWARM sequence** — with a
  *different* bit, `BIT(22)`, immediately followed by `OBS_BLOCK_A8015020
  = BIT(21)` and `OBS_BLOCK_A8016100 = 1`. This is a previously
  undocumented, second pulse of hardware this project already knows is
  central to TX completion, reached from `fn_0x894`'s unconditional tail
  call into `fn_0xd38` for ordinary TXWARM/TX servicing (`state_nibble`
  values matching `TXWARM`/`TX`, not an edge case), not from some
  RX-only or calibration-only branch.

**Deliberately not ported yet.** This bit-22 pulse is real and its
target register is exactly the one this whole investigation keeps
returning to, which makes it tempting to add immediately — but its
surrounding gate (the `distance` computation from live `PROTIMER`/`FRC`
timing registers, and the `flags & 0x81` test, both still only partially
verified) is not understood well enough to know whether it is safe to
fire unconditionally, or must be timed/conditioned the same way stock
conditions it. Pulsing an undertested bit on the project's most
safety-sensitive register under the wrong conditions could plausibly make
things worse, not better. Recorded here as the most promising concrete
lead for `ACTIVETXFCD` (§7.4/§7.6) found so far, pending either a fuller
static resolution of the gating conditions or live hardware tracing of
when stock actually takes this path.

**Update: tried the conservative, deterministic subset on hardware —
confirmed inert for this scenario, not just ineffective.** Implemented
exactly the `distance < 8` branch (the one case where stock's gating
condition is fully determined by the computation itself, independent of
the unported `flags` accumulation): read `PROTIMER+0x38`/`FRC+0x1C`,
compute `distance`, and only if it is below `8`, pulse `RAC+0x40` bit 22
plus the two accompanying writes — bounded, no loops, same risk class as
code already proven safe. QEMU 74/17/14 still pass. On real hardware, at
the same checkpoint used throughout §7.6: `FRC.STATUS` and `RAC.STATUS`
came back byte-for-byte identical to every attempt before it. Added a
diagnostic trace word (`EFR32_SEQ_TRACE_DISTANCE`, recording the computed
value unconditionally regardless of which branch is taken) to check
*why*, rather than accept a silent no-op: the live computed distance was
`0x1ffe` (8190) — nowhere near the `< 8` threshold. So this specific,
safely-portable branch genuinely never fires in this scenario; it isn't
that the pulse has no effect, it's that stock itself would also be
taking the *other*, unported branch here. This narrows things further
without resolving them: the `flags & 0x81` test on the `distance >= 8`
path — gated on job-dispatch state this project has deliberately not
reproduced — is now the specific remaining unknown blocking this lead,
not the pulse itself or the distance computation.

**Chasing that unknown further found something that reframes the whole
`RAC.SR3` finding (§7.5.1/7.5.2): `fn_0xd38` reads `RAC.SR3` *before*
reaching the distance computation at all, as part of an earlier gate
guarding a `goto far_abort` that, if taken, skips straight to the
function's exit — bypassing the distance computation, the `far_skip_table`
lookup, and the bit-22 pulse entirely.** Working through that earlier
gate with live-captured inputs from this project's own open sequencer
(`flags` built from `EFR32_SEQ_TRACE_LOCAL_3F64_P8`-sourced bits, same
diagnostic-capture technique as above) and `RAC.SR3` itself:

* With stock's own observed `SR3=0x08F08A31` (bits 4 and 5 both `1`),
  neither of `fn_0xd38`'s two conditional `flags &= ~BIT(n)` clears
  fires, `flags` stays `0x14`, `flags & 0xb1 = 0x10` (nonzero) — `far_abort`
  is **not** taken, and the function continues into the body this whole
  section has been analyzing.
* With `RAC.SR3=0` (confirmed live on this project's own open build,
  `read_memory 0xb802004c` → `0x0`, not the one static byte-scan from
  earlier — an actual register read on real hardware with the open
  stack flashed), *both* clears fire, `flags` becomes `0x04`,
  `flags & 0xb1 = 0` — `far_abort` **would** be taken.

**This does not describe the open stack's current behavior** — `fn_0x894`/
`fn_0xd38` are sequencer-object code this project has not ported at all
(§7.3), so none of this dispatch logic runs on real hardware today
regardless of `SR3`'s value. What it does establish: `RAC.SR3` is not
merely a fidelity nicety independent of everything else in this section —
it is a *precondition* for the dispatch logic this section keeps finding
promising leads inside of to ever run its main body at all, on stock or on
any future, more complete open port. Porting more of `fn_0x894`/`fn_0xd38`
without first resolving `SR3`'s origin (§7.5.2, still unresolved: read-only
everywhere on both cores, not found as a literal anywhere, value differs
between stock-flashed and open-flashed states despite no software writing
it on either) would hit the same `far_abort` exit stock avoids, for
reasons unrelated to whether the ported logic itself is correct.

### 7.4 `FRC.STATUS.ACTIVETXFCD` is unset where it should be set

`FRC.STATUS` (`0xA8004008`, read-only, mask `0x01FFFFFF`) was previously
unmapped in QEMU and undiscussed here. Its fields, by bit
(`host_py_rm_studio_internal/full/efr32xg22/revB2/FRC_S_field.py`,
`FRC_S_register.py`, same GitHub tree already cited throughout this file,
ref `sisdk-2025.6`): `SNIFFDCOUNT[4:0]`, `ACTIVETXFCD[5]`,
`ACTIVERXFCD[6]`, `SNIFFDFRAME[7]`, `RXRAWBLOCKED[8]`, `FRAMEOK[9]`,
`RXABORTINPROGRESS[10]`, `TXWORD[11]`, `RXWORD[12]`, `CONVPAUSED[13]`,
`TXSUBFRAMEPAUSED[14]`, `INTERLEAVEREADPAUSED[15]`,
`INTERLEAVEWRITEPAUSED[16]`, `FRAMEDETPAUSED[17]`, `FRAMELENGTHERROR[18]`,
`DEMODERROR[19]`, `FSMSTATE[24:20]`.

`tests/hardware-lockstep/stage2-working-irq8-frc.bin` (captured from the
confirmed-working stock controller at the AP3 IRQ8 breakpoint) reads
`STATUS = 0x00000060`: `ACTIVETXFCD=1, ACTIVERXFCD=1`, everything else
(including `FSMSTATE`) zero. `tests/hardware-lockstep/stage3-open-first-irq-frc.bin`
(captured at the open stack's own, equivalent first IRQ8) reads
`STATUS = 0x00001000`: `ACTIVETXFCD=0`, `RXWORD=1` instead, everything else
zero. The same two captures' `FRC.IF`/`FRC.SEQIF` already differ far more
starkly — stock reads `0x601a0e51` (bit 0 `TXDONE` set) where open reads
`0x40020c04` (bit 2 `TXABORTED` set, no `TXDONE`/`RXDONE`) — which is the
value `docs/hardware-bringup.md` already decodes and already concludes
aborts "before either open M0+ state handler can influence the
transaction."

**Caveat, stated plainly:** the `stage2-working-irq8-*` captures were taken
from a free-running stock controller already mid-advertising, so they may
represent a later repeat cycle rather than the controller's first-ever IRQ8
(several other registers in the same snapshot — e.g. `RAC`'s `0xA802003C`
region reads a large nonzero value on stock and exactly `0` on open — look
like accumulated steady-state activity, not reset state). `ACTIVETXFCD`
matching `stage2`'s confirmed-working `IRQ9` capture too (same `0x60`) makes
this less likely to be a cycle-count artifact for that specific bit, but it
is not yet a first-cycle-to-first-cycle comparison. `tests/hardware-lockstep/stage2-first-frc.bin`
/`stage3-first-frc.bin` (a true, pre-TXEN baseline pair) agree at `STATUS=0`
on both sides, which is at least consistent with `ACTIVETXFCD` being a
TXEN-driven transition on both systems, not a static config difference.

**Non-claim:** this does not identify *what* sets `ACTIVETXFCD`, only that
it is set on the working path at IRQ8 and not on the open path. `FRC_S_CMD`
has no TX-start command field (only `RXABORT` and resume/init bits), so the
trigger is most likely internal hardware wiring from `RAC`'s TX-state
transition, gated on some FRC precondition this project has not yet
identified — plausibly, but not yet shown to be, the `0x894`/gate-pulse
mechanics in §7.2–7.3.

**A candidate self-re-arm mechanism, reachability unconfirmed.** Continued
decomp of the same object locates, at local `0xf9c`, a function that: reads
`RAC+0x44` (`0xA8020044`, via a pointer chain rooted at `RAC.CMD`'s own
address) and proceeds only if bit 4 is set; computes a PROTIMER-relative
distance (`0xA801C038`/`0xA801C00C`/`0xA801C11C`) against a stored value;
and, on an exact match, writes `RAC.SEQIF` bit 21 via its SET alias,
`RAC+0x08`'s CLR alias byte `0x10`, and — critically — writes `1` directly to
`RAC.CMD` (`0xA8020010`), i.e. the M0+ itself re-issuing `TXEN`, not just
acknowledging one the M33 already issued. Its one call site, at local
`0x2b80`, is gated on a 4-bit field of a different register reading exactly
`2`, inside a large (~90+ instruction) routine that opens with a 19-entry
table-driven initialization loop (helper `0x2a60`, called with indices
`0..13, 15, 17, 19, 23, -1, 27`) touching more RAC/PROTIMER state than
anything else decoded in this section.

**Correction, superseded below:** this was left unconfirmed in the prior
pass. A full match-decompilation of `0xf9c` (owo-bg22-proprietary-baselines
`sequencer-decomp/src/fn_0xf9c.c`, compiled and verified to reproduce the
original's control flow) now shows the earlier summary above was also
incomplete in a way that matters: the `TXEN` write is real, but it is one
of *two* structurally parallel paths through this function, and **both**
paths -- not just the one already described -- are additionally gated on
`RAC+0xAC`'s bits `[15:12]` (the same state-like nibble `fn_0x894`/`fn_0xd38`
read) equalling `2`. Per this project's own established `RAC.STATUS.STATE`
enum (SS4.2), `2` is `RXSEARCH`; this project's plain `ADV_IND` transmission
goes through `TXWARM(8)`/`TX(9)`, never `RXSEARCH(2)`. The function also
reads `FRC.STATUS` bit 4 (the top bit of `SNIFFDCOUNT`) and conditionally
copies `FRC.RXRAWDATA` into a BUFC-region word -- both read as RX-sniff or
auto-response plumbing, not a general TX re-arm.

**Conclusion: `0xf9c` is very likely not relevant to this project's TX
scenario**, reachable or not. The gate at bit 4 of `RAC+0x44` does not
change this -- the fully-traced disassembly shows it selects between two
different ways of reaching the *same* `RXSEARCH`-gated behavior, not
between TX-relevant and TX-irrelevant behavior. This closes the `0xf9c`
lead for the `ACTIVETXFCD` question; it remains a correct, now-verified
decompilation of what this function does for a *different* scenario (RX
search / sniff), and is kept for that reason, not as a TX-path candidate.

### 7.5 A confirmed, missing SEQRAM-local write: `0xB0003EEC = 0x12`

`tests/hardware-lockstep/stage2-rail-starttx.json` is a real, dynamic
single-step store trace of the working stock M33's own `RAC_SEQ_IRQn`
(exception 54) handler immediately before it writes `RAC.CMD.TXEN`
(captured earlier, analyzed here for the first time). At step 1081 it
writes `0x00000012` to `0xB0003EEC` — the M33 non-secure SEQRAM alias of
local `0x3EEC`. This is the exact flag word §7.3 already identified, from
the *opposite* direction (static M0+ decomp), as something `0x894`
dereferences but never initializes itself. Two independent analyses — one
reading the M0+ object, one reading a live M33 trace — landed on the same
address. `rg` over this project's own `radio.c` and `radio_seq.S` confirms
neither writes `0x3EEC` anywhere; the open M33 driver never establishes this
state at all.

The same trace also has two more SEQRAM-local writes (`0x3ECC=0` at step
1082, `0x3EC4=0` at step 1149) and what first looked like a second,
unexplained gate pulse on `RAC+0x40` (bit 7 via the SET alias at step 1140,
bit 3 via CLR at step 1145) — checked and already correctly implemented:
that's exactly `EFR32_RAC_SR0_TX_PREPARE_SET`/`_CLEAR` (`BIT(7)`/`BIT(3)`),
already in `radio_tx_enable()`. Not every match in this trace is a gap;
`0x3EEC` is the one that is.

**Correction, found by tracing further:** the step-1081 instruction is not
actually the field's meaningful write. Its own disassembly context
(`stage2-m33.dis:10616-10644`) shows the surrounding code *reads* `0x3EEC`
into `r9` *before* two further calls, then writes the *same* `r9` back
unchanged afterward — a save/restore around those calls, not a fresh value
being established. Searching every `str` to this address in the whole
stock M33 image (five sites total) shows the register is a general,
accumulated multi-purpose flags word, not a single "TX armed" trigger:
one site (`stage2-m33.dis:10206-10210`) does a generic caller-parameterized
`bic r3,r3,r7; orr r3,r3,r6` (bits supplied by the caller, reusable
helper); another (`:10fc2-10fd0`) conditionally sets or clears only bit 27
based on an unrelated flag; two more (`:e50c-e512`, `:f5f6-f5fe`) are plain
AND-mask bit clears. So the observed `0x12` is whatever bits had
accumulated from earlier, unrelated initialization paths by the time this
project's capture happened to run — not a portable constant. The
`EFR32_SEQ_UNNAMED_FLAG_3EEC_OBSERVED_VALUE` write added below is
accordingly a **weak, likely-incomplete guess**, kept only because it is
cheap, inert if wrong, and better than leaving the word at whatever
garbage this project's own, much shorter boot path happens to leave there;
it should not be read as "the fix." The address itself, independently
corroborated by the static M0+ decomp in §7.3, remains worth having
correctly initialized — just not necessarily to this one captured value.

### 7.5.1 Fixed: `radio.c` was zeroing all of `RAC.SR3`, not just the two bits stock clears

`tests/hardware-lockstep/*-rac.bin` (eight capture pairs, spanning
pre-`TXEN`, the very first capture of the session, both channel-37 and
channel-38 runs, and both the IRQ8 and IRQ9 checkpoints) all agree:
`RAC.SR3` (`0xA802004C`, offset `+0x4C` in the dump) reads **exactly
`0x08F08A31` on stock, every single time, including the capture taken
before any `TXEN` or IRQ activity at all** — and reads `0x00000000` on
open at every equivalent checkpoint.

**This was a real bug in this project's own earlier work, not a stock
mystery.** The `EFR32_RAC_SR3_OFFSET` comment already in `radio.c` cited
three sources — a live trace showing stock clears *bit 14* of `SR3` as the
first store of its TX-start sequence, this project's own
`test_rac_sr3_clear_alias` QEMU test documenting stock clearing *bit 30*
via the CLR alias, and static decomp showing `fn_0x894`/`fn_0xf9c` reading
this register — but the code that landed from that evidence clears the
*entire 32-bit register* (`*radio_reg32(...) = 0U`) at *sequencer install*
(`radio_reset()`), not two specific bits at *TX-start*. Checking
`0x08F08A31` in binary confirms bits 14 and 30 are both already `0` in
every capture, consistent with stock clearing exactly those two bits once,
early, and never touching the rest of the register — not with a wholesale
zero. Directly confirmed against the verified stock initializer
(`stage2-m33.dis:30187` onward, the same `0x12d58` sequence this project's
own comment already cites): it clears exactly three registers via three
`str`s to `+0x40`/`+0x44`/`+0x48` (`SR0`/`SR1`/`SR2`) and **no fourth store
to `+0x4C`** — stock's install path never touches `SR3` at all.

**Fixed:** removed the full `SR3 = 0` write from `radio_reset()`, and added
a targeted `SR3_CLR = BIT(14) | BIT(30)` write (via the CLR alias) as the
first store of `radio_tx_enable()`, matching both the bits and the timing
the cited evidence actually describes. QEMU 74/17/14 still pass (the CLR-
alias test is generic register-mechanism coverage, independent of this
driver-level fix). **Not yet validated on hardware.** This does not by
itself explain `0x08F08A31`'s *other* bits or where they come from — see
§7.5.2 — but it stops this driver from actively destroying a value it
doesn't understand, which the prior code did on every reset.

### 7.5.2 `RAC.SR3`'s remaining bits: read-only everywhere software touches it, origin still unknown

**Superseded in part by §7.59:** stock's M33 does write `RAC.SR3` (base + `#0x4c`).

All five literal-pool references to `RAC.SR3`'s base address in the stock
sequencer object have now been traced by hand (raw byte offsets `0xa44`,
`0x564`/`0x66c`, `0x133a`, `0x13a0`, `0x2b44` — found via a direct byte
scan of the binary rather than `objdump -D`; see the methodology note
below). **All five are reads, never writes**: single-bit tests via
`lsls`+`bmi`/`bne` idioms at bits 1, 31, 6 (twice, at two different call
sites) and 25, every one used purely as a branch condition. Combined with
§7.5.1's M33-side result (stock's own, verified install sequence never
touches `SR3`), this is now a complete check of every direct literal
reference to this register on *both* cores, and every single one is a
read. The open question has narrowed from "who writes `SR3`" to "is it
ever written by software at all, or does it carry a nonzero hardware
reset value" — plausible for a chip-specific trim/calibration word,
implausible for QEMU's current all-zero-on-reset model of it.

**Strengthened the "both cores" check from "the verified install
sequence" to "the entire RAIL library, every member object file":** raw
byte-scanned all ~90 object files in
`librail_efr32xg22_gcc_release.a` (unlinked, so each retains its own
local literal pool, unlike the final linked `zephyr.elf` where pools get
merged into a shared `.rodata` region beyond any single instruction's
PC-relative reach — a real pitfall hit while trying this check against
the linked binary first: four apparent byte-pattern "hits" there turned
out to sit in `.rodata`, too far from any `.text` instruction for a
Thumb `ldr` to ever reach, and don't exist in the unlinked archive at
all, so they're coincidental data, not real references) for both
`0xA802004C` and its non-secure alias `0xB802004C`. **Zero hits in any
RAIL object file, in either form.** Only `generic_efr32xg22_release_seq.o`
(the M0+ sequencer object, already fully traced above) references this
address at all. So it is not just that stock's M33-side RAIL never
*writes* `SR3` — it never references it in any way, not even a read; the
M0+ sequencer is the only software on either core that touches this
register at all.

**Also checked and also negative:** the obvious indirect path — a write
to `SR3` reached via register arithmetic from an already-loaded nearby
base, the way `fn_0xd38`'s own `RAC_GATE_BASE_VAL_SRC` is computed from
`RAC_0X18`'s literal rather than its own — rather than `SR3`'s own
dedicated literal. All seven literal-pool references to `RAC+0x40` (the
nearest named register, 12 bytes before `SR3`) were checked for a nearby
`+0xC`-offset access from the same base register; none exists within any
of their immediate-use windows, only self-offset (`#0`), `#0x6c`, and one
unrelated register's `#4`. This was the last concrete static-analysis
lead available; confirming or refuting the hardware-reset-value
hypothesis from here requires actual hardware access. Recommended
hardware step unchanged from the prior pass:
dump this chip's own `DEVINFO` page (`0x0FE08000`..`0x0FE083FF`) and check
whether any documented field there, or the register's own behavior across
a real power-on reset (not just `SYSRESETREQ`), explains `0x08F08A31`.

**Done, and also negative: `DEVINFO` has no exact 32-bit match for
`0x08F08A31` anywhere in its 1 KiB (checked directly on this hardware).
The separate `USERDATA` page (`0x0FE00000`..`0x0FE003FF`, a common
Silicon Labs location for factory manufacturing/calibration tokens,
distinct from `DEVINFO`) was also checked — it is completely erased
(`0xFFFFFFFF` throughout), ruling it out entirely rather than just not
matching. Neither of the two plausible factory-calibration pages this
project knows about explains the value. The "real power-on reset, not
just `SYSRESETREQ`" half of this recommendation has also not been
attempted — this project has not power-cycled the board specifically to
test whether `SR3`'s value changes, only to rule out stale soft-reset
state as the explanation for a *different* finding (§7.5.4's splice-hang
investigation), which is not the same test.

**Now done: a genuine power cycle, with stock flashed, specifically to
test `SR3`.** Read `0xB802004C` immediately before, had the user power-
cycle the board, read it again after: `0x08F08A31`, identical, both
times. This rules out stale/retained state as the explanation
definitively (not just "probably not stale," which is as far as §7.5.4's
test could support) — the value is established fresh and identically on
every real boot with stock flashed, not carried over from a previous
session. Combined with the already-exhaustive software-write search
(§7.5.2: every direct literal reference on both cores traced, all reads;
the M33's own verified install sequence never touches it), the most
likely remaining explanation has shifted from "retained state" to
"established very early by something this project has not yet located
the write site for" — plausibly before either the M33 application image
or the M0+ sequencer object ever gets control (a boot-ROM-level or
otherwise pre-application mechanism), or via a write pattern this
project's literal-pool-based searches would not have caught (e.g. an LDMA
transfer with a register-computed rather than literal destination,
already flagged as not fully ruled out in SS7.5.2).

**The LDMA half of that is now ruled out, cleanly.** `LDMA_BASE`
(`0x40040000` secure / `0x44040000` non-secure, from the public device
header) appears **zero times** anywhere in `stage2-m33.dis` — not a
register-computed destination with an unresolved base, not used at all.
This firmware does not use the LDMA peripheral for anything, so it
cannot be how `SR3` gets its value. The remaining open possibility is
narrower now: a boot-ROM-level or otherwise pre-application mechanism,
since every write path within either user-controlled image (M33
application, M0+ sequencer object) has now been checked and ruled out.
This is the clearest open thread for whoever continues this.

**Searched and not found as a literal:** `0x08f08a31` does not appear
anywhere in `stage2-m33.dis` (the full stock M33 ELF disassembly, where
`objdump` has real section/symbol metadata and reliably distinguishes
`.word` literal-pool data from code) as a direct literal or a `movw`/`movt`
pair (checked both halfwords, `0x8a31`/`0x08f0`, separately). The stock
sequencer object required a different check: it is a flat, metadata-less
20 KiB binary, so `objdump -D -b binary` disassembles its embedded literal
pools *as if they were code* (confirmed directly — the known gate-pulse
literal `0xa8021040`, read correctly via raw bytes earlier in this same
project, shows up in that disassembly's text as bogus instructions like
`asrs r0, r0, #1` rather than any recognizable hex string), making a text
grep over it worthless for this question. Scanning the raw 20 480 bytes
directly for the exact 4-byte little-endian sequence — and separately for
each of its two halfwords, in case of a `movw`/`movt`-style split load —
finds zero matches anywhere in the object. So: genuinely absent from both
objects, checked by a method appropriate to each's actual structure. One
gap remains: traced M33-side register bases reaching a `#0x4c` offset
access (`SYSTEM_GetUnique()`'s `DEVINFO+0x4C` read, `USART_Reset()`'s
USART-relative write) ruled both out as unrelated; no exhaustive
register-taint pass over every `RAC_BASE`-literal reference has been done,
so a RAC-relative write built from a register computed far from its
literal load is not fully ruled out, just not found by the checks above.

**Working hypothesis, not yet confirmed:** the combination of (constant
across the whole session) + (not a literal anywhere in either object)
suggests this is *assembled at runtime from per-chip calibration/trim
data* — most plausibly `DEVINFO` (`0x0FE08000`, `efr32bg22_devinfo.h`),
the factory-programmed calibration page every EFR32 series-2 part has,
whose fields are genuinely chip-specific and would plausibly look like an
arbitrary bit pattern rather than a clean mask. If true, this changes what
"the fix" is: hardcoding `0x08F08A31` into `radio.c` would be exactly the
kind of magic-number/non-portable-constant this project's own standard
rules out (worse, it would be *wrong* on a different physical unit, not
just inelegant) — the correct fix would be reading the same `DEVINFO`
field(s) stock reads and packing them into `SR3` the same way, which
requires identifying which fields those are. **Not yet done**: this
project has not captured this specific chip's own `DEVINFO` page to test
whether its fields combine to `0x08F08A31`, nor traced the `#0x4c`-offset
accesses above to confirm a `DEVINFO`-to-`SR3` data path exists at all.
Recommended next hardware session: dump `0x0FE08000`..`0x0FE083FF` and
check for sub-fields that combine (directly or via a simple shift/OR) to
`0x08F08A31`.

**Reframing this whole thread, from the public register model, not more
disassembly:** fetched Silicon Labs' own `RAC_S_register.py`/
`RAC_S_field.py` (`build/reference-models/`, the same public generated
model already cited elsewhere in this file for other registers — not
previously fetched for `RAC` specifically). It defines `RAC_S.SR3` at
exactly `0xA8020000+0x04C`, access `read-write`, **documented reset value
`0x00000000`**, and an entirely **empty description** on both the
register and its one 32-bit field — Silicon Labs' own model assigns it no
hardware meaning at all; it is a generic software scratch word (the same
shape as `SR0`/`SR1`/`SR2`, all four presumably meant for firmware's own
bookkeeping, not fixed hardware status bits).

This flips the framing that has driven this section since §7.5.1: the
*official* reset value is `0`, which is exactly what the open stack reads
— the open stack is not failing to establish some hardware default, it is
correctly observing it. Stock's `0x08F08A31` is the anomaly, and it is
Silicon Labs' own software (somewhere this project has not located,
pre-application per the exhaustive checks above) writing a value into a
register documented as blank scratch space — most plausibly as an
undocumented handshake between Silicon Labs' own boot/bootloader stage and
their own RAIL library, not a hardware requirement for radio operation at
all.

**More importantly, checked directly rather than re-deriving from old
notes: does the open stack's own *shipped* code even read `SR3`
anywhere?** No. `radio_seq.S` has zero references to it. `radio.c` only
*writes* its CLR alias (bits 14/30, §7.5.1's fix) — a one-way write,
never a read, never a branch condition. So whatever `SR3` actually holds
cannot be influencing any decision the open stack's running code makes
today. The `fn_0x894`/`fn_0xd38` `far_abort` gate this section spent real
effort establishing (`5c85b73`) is real, but it only matters *if that
dispatch logic is ported* — and this project's own charter deliberately
never ports it. **This closes out `SR3` as a live suspect for
`ACTIVETXFCD` as currently blocked**: it is a genuinely interesting,
still-unsolved mystery (worth the "Recommended next hardware session"
above if anyone wants to chase it further), but not something on the
open stack's actual execution path, and not what is standing between the
current code and a working stage 3. The real blocker is still open and
needs a different thread, not more `SR3` archaeology.

### 7.5.3 Fixed: the SEQIF-ack wait didn't retry its own CLR write

Direct instruction-by-instruction comparison of `radio_seq.S`'s bounded
SEQIF-ack loop (end of §7.2's gate-pulse sequence) against stock's actual
`0x7c8` disassembly found a real divergence, independent of and in addition
to §7.4/§7.5: stock's `0x7c8(mask)` re-issues `str mask, [RAC.SEQIF_CLEAR]`
on *every* loop iteration, immediately before re-reading `RAC.SEQIF` and
re-testing the mask —

```
7ca: ldr r1, [pc, #...]   @ RAC.SEQIF_CLEAR
7cc: str r0, [r1, #0]     @ re-issue the clear, every iteration
7ce: ldr r1, [pc, #...]   @ RAC.SEQIF
7d0: ldr r1, [r1, #0]
7d2: ands r1, r0
7d4: bne.n 0x7ca          @ loop back to the re-clear, not just the re-read
```

`radio_seq.S`'s prior version wrote the CLR value once, then only polled
`RAC.SEQIF` without ever re-asserting the clear:

```
ldr r0, =EFR32_RAC_SEQIF_CLEAR
ldr r1, =EFR32_RAC_SEQIF_TXWARM_ACK_MASK
str r1, [r0]              @ single write, outside the loop
11: ldr r0, =EFR32_RAC_SEQIF
    ...                    @ loop only re-reads, never re-writes
```

If the single write doesn't fully take effect on real silicon (a plausible
race this project cannot rule out without the hardware tooling blocked in
§7.6), stock's loop self-corrects by re-asserting the clear every spin;
the prior `radio_seq.S` version would instead spin uselessly to its
`EFR32_RAC_TXWARM_WAIT_BOUND` timeout. Fixed by moving the CLR write inside
the loop body, matching `0x7c8` exactly. QEMU 74/17/14 all still pass;
**not yet validated on hardware** (same blocker as §7.6) — this is a
plausible contributor to the `ACTIVETXFCD` divergence, not a confirmed
fix for it, since it affects a SEQIF handshake bit, not `FRC.STATUS`
directly.

### 7.5.4 The splice-into-stock hardware test: a real hang, later isolated to the splice vehicle, not the C lifts

**Superseded in part by §7.53:** a fresh debugger session can itself cause this hang.

The validation the user asked for — patch the five verified C lifts
(`fn_0x7ac`/`_0x7be`/`_0x7c8`/`_0x894`/`_0x1bda`, relocated to free SEQRAM
space at local `0x3000`, redirecting only the five real call sites inside
IRQ8/IRQ9's bodies; `sequencer-decomp/build_patched_stage.sh` automates
the whole pipeline) into a byte-faithful copy of the real stock SEQRAM
image, bake that into the real proprietary RAIL blob, and see if the
beacon still transmits — was run to completion on real hardware. Result:
**it does not transmit; it hangs before `main()` ever runs.**

This is a real, deterministic, content-specific hang, not a tooling
artifact — checked carefully given this session's earlier false alarms
with flaky UART capture:

* Reproduced identically on back-to-back reflashes of the *same* patched
  image with no other firmware interleaved (rules out a one-off flash
  write glitch).
* Unmodified stock, reflashed immediately *after* a patched-image hang,
  boots and advertises perfectly on the first try every time (rules out
  retained-SEQRAM-state corruption from a prior run, which this project's
  own `radio.c` comments already note SEQRAM is subject to — "SEQRAM
  retains across an M33 reset" — since stock recovers from it instantly
  while the patched image does not).
* A real halt (`DHCSR` confirms genuine `S_HALT`, not a stale cached
  state) catches the M33 every time at the exact same address,
  `0x00013056`, inside `sli_rail_cd65c06e3b1af867d6257be45870f13b` — an
  **unbounded** busy-wait (`ldr`/`lsls`/`bmi` spinning on bit 0 of a word
  at the SEQRAM non-secure alias `0xB0004080`) that runs before Zephyr's
  own unconditional `*** Booting Zephyr ***` banner, which is why nothing
  at all appears over UART.

**Not yet root-caused.** `0xB0004080` (local `0x4040+0x40`) is well above
both the original object's end (`0x2f6c`) and this patch's appended block
(`0x3000`..`0x320c`) — this patch's SEQRAM copy does not directly write
that address. The sequencer's own reset handler (local `0x2ade`, the
already-documented 19-entry calibration-table init loop) was checked and
does not call any of the five relocated functions directly, so the M0+'s
*own* boot path isn't obviously implicated by a direct read — but it may
still be the M0+ never reaching whatever step sets this flag, for a
reason not yet traced (a callee of a callee, or a side effect of this
patch's copy-length change exposing or disturbing memory some other part
of the system assumed was free). This needs either single-stepping the
M0+ directly (AP3, blocked earlier in this project by the same Secure-
debug-lock reset unreliability as the M33) or tracing every caller of the
five relocated functions' *callees* for a path back to one of them, which
has not been done.

**What this does and does not say about the C lifts themselves:** each
lift still independently compiles and, by itself, reproduces the original
disassembly's control flow and touched addresses (the match-decompilation
methodology this whole section follows). This splice test is strictly
stronger than that — exercising the lifts inside the real running system,
across every code path that reaches them, not just the one this project
verified by hand — and it is *this* test that fails. That is exactly why
the user asked for it rather than trusting the per-function compile-time
check alone.

**Follow-up finding that complicates "the lifts are wrong":** the hang at
`0x13056` is in M33-side RAIL code this patch never touches, not sequencer
code. Rebuilding the identical patched blob with
`CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE` swept across `0` (boots fine, reaches
`main()`, later fails harmlessly on the pre-existing allocation issue),
`1` (a *different* failure — a hard fault, bus fault on vector-table read,
before `main()`), and `8192` (this section's silent hang) shows the outcome
is sensitive to this unrelated Kconfig value. Comparing `arm-none-eabi-size`
output rules out a RAM-pressure explanation directly: stock-with-`8192`
(works) and this patch-with-`8192` (hangs) have **byte-identical
`data`+`bss`** (`856`+`28761` both); the only difference is `.text` being
672 bytes larger (exactly this patch's appended functions, which this exact
early boot path never calls). The remaining plausible explanation is a
marginal, pre-existing timing race already latent in stock's own RAIL init
(the M33 polling a SEQRAM flag the M0+ is expected to set, with no apparent
timeout) that this patch tips over via an incidental flash-layout/timing
shift — not necessarily a logic error in the lifted C. This does not
exonerate the lifts (the splice test still fails, and that is the result
that matters for "does this work end to end"), but it means the failure
mode may be a test-sensitive pre-existing fragility in stock's own
synchronization, not proof the ported logic is behaviorally wrong.

**Resolved: a decisive isolation test exonerates the lifts completely.**
Built a third variant — same `0x2f6c`→`0x320c` SEQRAM copy-length
extension, same 524 bytes occupying `0x3000`..`0x320c`, but with **zero**
call-site redirection: all five of IRQ8/IRQ9's `bl` sites left pointing at
the original, unmodified `0x7ac`/`0x7be`/`0x894`/`0x1bda`/`0x7c8`, verified
byte-identical to the reference capture everywhere except the
now-unreferenced appended bytes. This "null" variant — which never calls
any lifted C at all — **hangs identically** at the same `0x13056` busy-wait,
with the same malloc-arena sensitivity. This conclusively separates two
previously-conflated things: the C lifts' correctness (never in question
again after this test) and the splice *vehicle*'s correctness (the
relocate-and-extend-the-copy-length mechanism itself, which is broken
independent of what content fills the extra space or whether anything
calls it). The earlier "it fails" framing in this section's title was
accurate about the test outcome but wrong about what it implicated — it
implicates this project's own splice methodology, not the match-decompiled
functions. Not yet root-caused *why* extending the copy length breaks
something (the leading hypotheses — a timing shift against the M0+'s own
startup, or corrupting SEQRAM content some other part of the system reads
as persistent/retained state — are unchanged from the discussion above),
but the lifts themselves no longer carry any of the blame.

**Further isolation: more than one, unrelated kind of modification breaks
it, even with the length unchanged.** Two more variants, both built and
tested on real hardware: (a) the exact same `0x2f6c`→`0x320c` length
extension with *one byte* flipped at a minimal copy increment
(`0x2f6c`→`0x2f70`, 4 bytes) — hangs identically; (b) the length left
**completely untouched** at `0x2f6c` (same as stock, object occupies the
identical flash address range), with a single byte flipped at offset
`0x2000` — **also hangs identically**, confirmed by direct register halt
at the same PC, not just UART silence.

Caveat on (b), checked afterward rather than assumed: that specific byte
is *not* inert padding — it is the low byte of a `movs r0, #0` immediate
inside the object's own code (an early-return path), so the edit actually
changed `r0`'s loaded value from `0` to `0xff` at that point, a real
(if small and localized) code change, not a neutral content probe. That
weakens (b) alone as evidence for "truly any byte, anywhere" — but it
does not weaken the combination of (a) and (b): the null-call-site variant
from the previous finding only appends unrelated bytes *past* the
original 12140-byte object and touches nothing within it, including
nothing near offset `0x2000`, and that variant also hangs identically.
Three structurally different modifications (relocate real calls and
extend the copy length; extend the copy length by 4 bytes with inert
appended content and no call redirection; leave the length and everything
in the appended region alone and change one real instruction's immediate
deep inside the object) converge on the same failure, which is still best
explained by some form of whole-object integrity gate rather than any one
of these specific edits mattering on its own.

**Lockstepped the exact divergence point with `bg22_reset_to_breakpoint`
and single-stepping (hardware only; QEMU has no model of this proprietary
blob to compare against).** Breakpointing forward from reset through
`RAIL_VerifyTxPowerCurves` (`0x18a8c`, returns `0` — not an obvious
failure signal) and into the M33-side PA/TX-power calibration helper at
`0x13040` shows stock and the single-byte-modified build agree on every
register at every checkpoint up through the *first* of two back-to-back
internal busy-waits in that helper (both pass it near-instantly). Single-
stepping 45 instructions from there, diffed line-for-line between stock
and the modified build, finds the **exact** first divergent instruction:
at `pc=0x13058` (`lsls r1, r1, #31`, testing bit 0 of a value just read
from `0xB802C040`), stock's `r1` is `0` (bit clear — wait exits) and the
modified build's `r1` is `1` (bit set — the following `bmi` branches back
to `0x13056` forever). Both builds reach this exact instruction with every
other register identical; only this one status bit differs, every single
time.

**This status bit is not new or unnamed — it's already documented in §1.1
as part of the "RF mailbox" producer/consumer protocol**: `0xB802C000` is
one of two non-secure mailbox endpoint windows this project already
identified (flags/status at `+0x040`, enable at `+0x044`), used for M33⟷M0+
cross-core doorbell signaling, serviced on the M0+ side by
`RDMAILBOX_IRQHandler`. The M33 is not waiting on an isolated calibration
register; it is waiting for the M0+ sequencer to **service and acknowledge
a mailbox message**. Modifying any byte of the sequencer object — content
unrelated to the actual message-handling code, by position — prevents
that acknowledgment from ever arriving. The leading explanation is some
form of integrity gate (most likely on the M0+ side, possibly checked
before `RDMAILBOX_IRQHandler` is allowed to run or before the M0+ is
considered "ready" to service mailbox traffic at all) that fails on any
modification and is not yet located in either object's disassembly.

**Ruled out: stale state from this session's many `SYSRESETREQ`-only
reboots.** Before accepting the above, checked whether the stuck bit was
an artifact of this session having reset the board via software dozens of
times without ever a true power cycle (plausible for a PLL/calibration
engine with its own independent, rarely-power-on-reset state). A genuine
physical unplug/replug was performed and the single-byte-modified build
was reflashed and retested: it hangs identically, confirmed via
`bg22_reset_to_breakpoint` timing out waiting for `0xfc34` (the return
point past the entire calibration helper) exactly as before. This is a
real, content-dependent effect, not accumulated soft-reset corruption.

**Net effect on the project's open questions:** the splice-into-stock
hardware test cannot currently validate *any* modification to this
sequencer object, including ones (like this project's own verified C
lifts) that are logically correct — the test vehicle itself is blocked by
whatever gate this is, upstream of anything the lifted functions do. This
also reopens relevance for `ACTIVETXFCD` (§7.4): if the open stack's own
firmware ever needs to pass through an equivalent mailbox-service gate
and does not, the same failure mode is a plausible unifying explanation,
though this has not been checked against the open stack directly.

**Confirmed QEMU divergence, and confirmed (not just assumed) that it is
currently inert for the open stack.** Checked directly: QEMU's model
(`vendor/qemu/hw/arm/efr32bg22.c:1867-1879`) unconditionally returns `0`
for `RFMAILBOX_FLAGS` on both endpoints regardless of any guest write —
codified by its own existing test, `test_rfmailbox_no_peer`
(`efr32bg22-test.c:1633`), which asserts exactly that: write bit 1 via the
SET alias, read back `0` anyway. So the real-hardware busy-wait this
section just lockstepped (spinning on bit 0 of this same register) can
never be reproduced in QEMU — every read reports "ready," so the wait
exits immediately regardless of whether the sequencer blob is intact or
tampered with. **This is a real divergence from exact silicon semantics,
being recorded here per this project's standing rule, not swept under the
existing stale comment** ("the peer endpoint and its delivery timing have
not been recovered" — written before this section's hardware lockstep
findings existed).

It is not, however, a quick fix. QEMU has no emulated M0+ core at all, so
there is no peer to ever clear a real latch. Turning `FLAGS` into a
genuine stored bit instead of a hardcoded `0` would not make the model
*more* faithful — it would trade today's "always ready" simplification
for a *different*, equally unfaithful "always stuck after the first
write" hang, since nothing in the model would ever clear it again. A
faithful fix needs either an emulated M0+ peer or a known trigger
condition for the specific bit real silicon gates on — and bit 0's real
derivation is exactly as unresolved as `RAC.SR3`'s (§7.5.2): not
something to guess at just to make the model do *something* different.

**Checked whether this matters for `ACTIVETXFCD` right now, rather than
leaving the §7.5.4-opening speculation unresolved: it doesn't, currently.**
Neither `radio.c` nor `radio_seq.S` contains any reference to the
mailbox addresses (`0xA8028000`/`0xA802C000` or their aliases) anywhere —
confirmed by direct search, zero hits beyond one unrelated provenance
comment citing a *different* RAIL archive member
(`sli_rail_dadf1c52f26190429dcacf7151424c68`, a `RXENSRCEN` write-pattern
citation) than the one this hang lives inside
(`sli_rail_cd65c06e3b1af867d6257be45870f13b`). The hang itself is in
stock's M33-side RAIL power-curve calibration helper, which only stock's
own RAIL library ever calls, at boot, before any TX attempt — the open
stack replaces that whole call path with its own code and never executes
it. So this specific gate is not currently on the open stack's own TX
path in either QEMU or real hardware; it is a genuine, now more precisely
understood limitation of the splice-test vehicle specifically, not (yet
shown to be) a missing piece of stage 3 itself.

### 7.6 Hardware status: real progress confirmed, `ACTIVETXFCD` still unset

**Update: the open stage-3 build (`build/radio-port-hardware-2`, with
both §7.5.1's `RAC.SR3` fix and §7.5.3's SEQIF-ack retry applied) was
flashed and run on real hardware with its built-in bring-up trace
(`CONFIG_EFR32_RADIO_BRINGUP_TRACE`, `tests/radio-port`).** Real,
observable forward progress since the previous "hangs after `TXEN`"
characterization: the trace reaches `txen status-before=0x10400000
status-after=0x58000000` — decoding `RAC.STATUS.STATE` (bits `[27:24]`),
`before=0x0` (OFF) → `after=0x8` (TXWARM) — and a direct post-mortem halt
afterward reads `RAC.STATUS=0x9400000`, i.e. `STATE=9` (**TX**), *steady*,
not aborted back to OFF. This is further than this project's own prior
sessions recorded reaching.

**But `ACTIVETXFCD` is still unset.** The same halt reads
`FRC.STATUS=0x01100200`: `ACTIVETXFCD` (bit 5) = `0`, `FRAMEOK` (bit 9) =
`1`, `FSMSTATE` (bits `[24:20]`) = `17`. Reading this project's own added
`EFR32_SEQ_TRACE_STATUS_BEFORE`/`_AFTER` words (`0xB0004FB0`/`0xB0004FB4`,
§7.5's gate-pulse instrumentation) shows **identical** values before and
after the TXWARM gate-pulse sequence (`0x01100200` both times) — the gate
pulse itself produces no observable `FRC.STATUS` change at the point
measured. So: real RAC-level progress (reaches and holds TX, doesn't
abort), but the FRC frame-coder engagement this project has been chasing
since §7.4 still never happens. The SR3/SEQIF-ack fixes did not resolve
the central open question; the `FRC.STATUS` value itself also differs
from the one previously documented in §7.4 (`0x00001000`, `RXWORD=1`)
for the same checkpoint, so *something* about the system's behavior has
changed, just not the bit that matters.

**Done: the timing comparison this note recommended.** Single-stepping
turned out to be the wrong tool (debug-halting this chip freezes the
radio peripheral clocks too — confirmed directly: polling `FRC.STATUS`
twenty times in a row *while halted* reads a constant `0x0` every time,
only changing once the core is actually resumed and allowed to run
freely). The working method instead: `bg22_reset_to_breakpoint` to catch
each build at its own `RAC.CMD.TXEN` write (stock: `0xe3a0`, right after
`stage2-m33.dis:0xe39a`; open: `0x16606`, right after
`radio_tx_enable()`'s own TXEN write at `0x16602`), `resume`, a real
(non-single-step) `sleep`, then `bg22_require_sram_execution` again to
re-halt and read. Bisected on stock: `ACTIVETXFCD` is still `0` at 10 ms
after `TXEN`, already `1` by 30 ms (coarse — most of that window is this
measurement's own re-halt overhead, not real radio timing, so this
bounds the *measurement*, not necessarily the hardware). At a clearly
sufficient 100 ms, stock reads `FRC.STATUS=0x20` (`ACTIVETXFCD=1`) and
`RAC.STATUS=0x400000` (`STATE=4`, **RXPD** — it has completed TX and
moved on). The open stack at the identical 100 ms checkpoint reads
`FRC.STATUS=0x1100200` (`ACTIVETXFCD=0`, unchanged from immediately after
`TXEN`) and `RAC.STATUS=0x9400000` (`STATE=9`, **still TX**, not stuck
earlier in the sequence and not aborted back to `OFF` either — genuinely
holding steady in the TX state with the frame coder never engaging). This
is now a clean, timing-eliminated comparison: stock finishes a complete
TX cycle and advances in well under 100 ms; the open stack does not
advance at all given 10x that margin.

**Tried and did not move the needle: `fn_0x894`'s own entry/exit gate
bracket.** `fn_0x894` (the dispatcher §7.3 and §7.6 both correctly decline
to port wholesale) unconditionally sets `RAC+0x44` bit 16
(`RAC_0X44_BIT_FUNCTION_GATE`) on entry and clears it — plus does an
unconditional *direct* (non-aliased) zero of the whole register — at
exit, regardless of IRQ number or RAC state; confirmed in
`sequencer-decomp/src/fn_0x894.c` lines 147/235/240. This bracket, unlike
the dispatch logic inside it, is simple, unconditional, bounded hardware
mechanics — the same category already established safe to port in §7.2 —
and was entirely absent from `radio_seq.S`. Added it (bracketing the gap
between the `0x7be`-equivalent gate clear and the `0x1bda`-equivalent
three clears, matching the established call order exactly). QEMU 74/17/14
still pass; confirmed present in the rebuilt sequencer binary's own
disassembly before testing. **Result: no change at all** — the identical
`FRC.STATUS=0x1100200`/`RAC.STATUS=0x9400000` at the identical 100 ms
checkpoint, byte for byte. This is a genuine negative result, not a test
artifact (the breakpoint address was re-verified unchanged and the new
instructions were confirmed present in the binary actually flashed).
Kept the fix anyway — it is still a faithful reproduction of stock's own
unconditional behavior, not a guess, and does not regress anything — but
it rules out this specific bracket as (the whole of) the missing piece.

**Ruled out: software TIFS.** `radio_tmr_tifs_set()`, `radio_tmr_hcto_configure()`,
and the rest of the `radio_tmr_*` family are pure software bookkeeping on
real hardware — every hardware-facing line in them is guarded by
`CONFIG_EFR32_RADIO_QEMU_TIFS`, a simulator-only path. None of them write a
PROTIMER compare, a RAC HCTO, or any other register on real silicon;
`radio_switch_complete_and_rx()`/`_tx()`/`_disable()` are likewise
unconditional `EFR32_RADIO_TODO()` no-ops outside that same guard. The
official Silicon Labs EFR32BG22 errata (Rev 0.6, July 2021,
`silabs.com/documents/public/errata/efr32bg22-errata.pdf`) also lists no
TIFS/RAC/FRC/sequencer item; its only two radio errata (`RADIO_E302`
data-whitening selectivity, `RADIO_E303` RAIL packet filters with header
enabled) are unrelated. TIFS governs RX-after-TX turnaround in any case,
which this project never reaches: the *first* TX itself already fails to
complete. This does not rule out a TIFS-*adjacent* hardware precondition
(PROTIMER/RAC state some other code path should establish but doesn't),
only the TIFS implementation as currently (not) written.

**Confirmed: `radio_tx_enable()`'s actual BUFC/RAC write order is correct.**
Re-reading the current source (not the possibly-stale prose a few sections
above) line by line: `radio_pkt_tx_set()` itself only `memcpy`s into a
software staging buffer and sets flags — it performs no hardware access at
all. The real write sequence runs inside `radio_tx_enable()`, in order:
`efr32_tx_fifo_publish()` (`BUFC.CMD=CLEAR`, `BUFC.CFG=512`-byte config,
`BUFC.ADDR`=staging pointer, then each byte individually through
`BUFC.WRITEDATA` — matching the already-documented live DWT-watchpoint
observation of stock silicon's own per-byte `WRITEDATA` pattern, not a
prefetch-by-count scheme); then `efr32_packet_stage_arm()`; then the SR0
TX-prepare set/clear and `SEQ_RX_AFTER_TX_INHIBIT` clear already discussed
above; then, last, `RAC.CMD = TXEN`. So FIFO data is fully staged before
TXEN in the compiled instruction stream, not merely in trace-log order.

### 7.7 Broader decompilation pass: `fn_0x19d0` and the structure around it

**Superseded in part by §7.51:** there is a sixth vector, IRQ11.

Per this project's own standing charter (full, genuine understanding of
the proprietary blob, not just whatever slice bears on `ACTIVETXFCD`),
revisited the object with a wider net: a raw byte scan for every
literal-pool reference to `FRC.CMD` (`0xA800402C`) in
`generic_efr32xg22_release_seq.o` finds **four** hits, not the one
already covered by `fn_0x894` (§7.3's `FRC_CMD_FRAMEDETRESUME`, gated
narrowly and already shown TXWARM/TX-irrelevant). The other three all sit
inside one previously-unexamined function at local `0x19d0`
(ends `0x1a04`), now fully lifted and verified in
`sequencer-decomp/src/fn_0x19d0.c`.

`fn_0x19d0` unconditionally, every time it runs: calls `fn_0x7d8()`,
zeroes `0xAA0000D8` and sets `0xAA0000F4=10` (a `BUFC.BUF[N]` pair this
project hasn't assigned an index to yet — doesn't match the
already-identified `BUFC.BUF[1]` struct at `0xAA000040`), calls
`fn_0x7ea()` with `fn_0x7d8`'s own return value (**not** a literal, as
this lift's first draft wrongly assumed from memory instead of reading
the real disassembly — caught only by diffing the compiled output
against `generic_efr32xg22_release_seq.dis:0x19d0-0x1a04` byte for byte,
per this project's standing verify-by-compiling rule), clears
`RAC+0x40`'s CLR alias bits 16/17 and `RAC+0x44`'s CLR alias bits 2/31
(four bits this project hadn't previously named — distinct from the
already-documented bits 0/1/16/18 on these same two registers), sets the
`0xA8005064`/`0xA8006064` "unclaimed" pair's bit 4 and
(`+0x54`) bit 27, writes `FRC.CMD = TXSUBFRAMERESUME` (bit 6), and
clears a SEQRAM-local byte at `0x3E72`. Its own incoming argument is
confirmed dead (clobbered by `fn_0x7d8`'s return before it's ever read).

`fn_0x19d0` has five call sites (`0x237e`, `0x2658`, `0x28bc`, `0x2900`,
`0x2998`). The first, at `0x237e`, sits inside a function at `0x2362`
which opens with a direct `bl 0x894` — calling the shared dispatcher
outright, not through IRQ8/IRQ9's own entry points (`0x2578`, already
fully resolved in §7.2). **Correction, found by reading this object's own
vector table directly (raw byte scan of its first `0x60` bytes, same
technique already used for every literal pool in this object — word 0 =
`0x3e30`, the already-established stack top; words 24/25 =
`0x2579`/`0x25c9` independently confirm the already-known IRQ8/IRQ9
entries): `0x2362` is not an orphan helper, it is IRQ13's own entry
point (word 29 = `0x2363`).** `fn_0x2390` (word 16 = `0x2391`) is IRQ0's
entry point, and `fn_0x23a0` (word 17 = `0x23a1`) is IRQ1's. A fourth new
vector, word 26 = `0x2645`, is IRQ10's entry at local `0x2644`, not yet
examined. All four are genuine interrupt vectors sharing this object's
dispatcher conventions, not internal helpers reached from IRQ8/IRQ9 —
and the open stack's own `radio_seq.S` deliberately enables *only*
IRQ8/IRQ9 in `NVIC.ISER0` ("Enable only those bounded peripheral
events"), so IRQ0/IRQ1/IRQ10/IRQ13 never fire at all on the open build.
That is this project's own existing, deliberate scope boundary, not an
oversight — but it leaves one real open question: whether *stock's* own
NVIC/SEQIEN enables any of these during the real hardware TX scenario
this project is chasing. No stock NVIC.ISER0 capture is in hand yet, so
this is unconfirmed either way.

`fn_0x2362` tests `RAC.STATUS` bit 19 and branches between a five-call
chain (`fn_0xd34`/`fn_0x1a18`/`fn_0xc02`/`fn_0x19d0(1)`/`fn_0x13b0`) or
skipping straight to its ack, either way acknowledging `RAC.SEQIF` bit 29
via the shared `fn_0x7c8` helper (§7.2) — a *different* bit than IRQ8's
own bit 24. `fn_0x2390` is simpler: calls `fn_0x894`, then unconditionally
acks bit 16. Both are now fully lifted and verified in
`sequencer-decomp/src/fn_0x2362_0x2390.c`.

`fn_0x23a0` (IRQ1) is longer and now also fully lifted and verified, in
`sequencer-decomp/src/fn_0x23a0.c`. It opens with the *exact* same
`bl 0x7ac; movs r0,#5; str [0xA802113C]=5; bl 0x7be` gate-pulse prologue
IRQ8 uses (`0xA802113C` is a RAC register this project hadn't named
before — `RAC+0x13C`'s own SET alias) — a real shared convention across
at least two vectors, not a coincidence — then diverges completely: reads
`RAC+0x40`'s raw (un-aliased) register for the first time this project
has done so (previously only pulsed via its SET/CLR aliases), testing
bits 0 and 9 (meanings not otherwise established); conditionally calls
`fn_0xa76`; calls `fn_0x894`; copies a SEQRAM-local word pair
(`0x3FB8`/`0x3FBC` → `0x3E48`); and, gated on that same bit 0, runs a
busy-wait loop computing `now - target` from `PROTIMER+0x20` against a
SEQRAM-local word at `0x3ED0`, with a wraparound correction that adds
**`PROTIMER+0x38`** — the *exact same register* §7.3.1 already
established feeds `fn_0xd38`'s own "distance" computation — until the
result crosses a bare `0xFF01` threshold (unit not established; not a
register address, so not treated as a provenance-commented peripheral
constant the way addresses are elsewhere in this project). The two
functions are not shown to be the same mechanism, but sharing this one
specific register across both is a concrete new lead on what
`PROTIMER+0x38` actually is. Finishes by conditionally calling
`fn_0x388`, then `fn_0xace`/`fn_0xb74`, clears `RAC+0x40` CLR-alias bit
0, and acks `RAC.SEQIF` bit 17.

This lift's first draft had a real bug, caught only by diffing the
compiled output against the original instruction-by-instruction (the
standing verify-by-compiling rule, again): the wraparound correction's
guard compiled from `target > now` where the original's `bcc` only skips
the correction when `target < now` *strictly* — meaning the original
applies the correction on equality too, which `>` silently excluded.
Fixed to `target >= now`; recompiling then showed gcc had independently
turned the `0xFF01` loop threshold into an equivalent `<= 0xFF00`
comparison (one `movs`+`lsls` instead of a 32-bit literal load) — a
cheaper, bit-identical encoding of the same comparison, confirmed
equivalent, not a second bug.

`fn_0x2644` (IRQ10) is the simplest of the four: calls `fn_0x894`, then
conditionally calls `fn_0xc02` gated on bit 6 of `RAC+0x144` — a register
this project hadn't named before — then unconditionally calls
`fn_0x19d0(1)` and `fn_0x13b0`, then acks `RAC.SEQIF` bit 26. Now fully
lifted and verified in `sequencer-decomp/src/fn_0x2644.c`, matching the
original byte for byte (same shift immediates, same branch polarity, same
literal pool value).

**Status: `fn_0x19d0`, `fn_0x2362`, `fn_0x2390`, `fn_0x23a0`, and
`fn_0x2644` — all five of this object's vectors beyond IRQ8/IRQ9 that
this pass turned up — are now fully lifted and verified.** `fn_0xc02`
(called from both `fn_0x2362` and `fn_0x2644`) is also now fully lifted
and verified, in `sequencer-decomp/src/fn_0xc02.c`: on `RAC_GATE_BASE`
bit 0 clear, clears two RAC bits via their CLR aliases, then
unconditionally touches seven more RAC offsets — two of them (`0x10`,
`0x140`) *direct*, un-aliased whole-register writes, unlike every other
RAC access this project has decompiled so far. This single function names
nine previously-unidentified RAC offsets
(`0x10`/`0xE0`/`0x108`/`0x130`/`0x140`/`0x144`/`0x160`/`0x16C`/`0x174`).
`fn_0x1a18` (also called from `fn_0x2362`) is the largest fully-traced
function from this pass: a mode-selecting hardware setup sequence gated
entry/exit on a raw, un-aliased `RAC+0x44` bit 17 (early-return if clear
on entry; unconditionally cleared on exit — the same "service and ack"
shape already established for `RAC.SEQIF` bits, here on a different
register and convention). It reads a one-time snapshot of `RAC+0x3FC`
and branches its whole body on that snapshot's bits 0/1, selecting one of
three mutually exclusive value pairs written into `RAC+0x148` (both via
its SET alias and, later, its CLR alias), and calls `fn_0x21d8` four
times (arguments `1,1,4,1`, not yet examined — interspersed between
hardware writes, the shape fits a short calibration-settle delay
primitive, not confirmed). Names fourteen previously-unidentified
addresses: `RAC+0x3FC`/`0x11C`/`0x114`/`0x148`/`0xB4`/`0x188`, plus two
more offsets (`RAC+0x140`, `RAC+0x160`) this project had already named
from *different* functions touching them through *different* mechanisms
— `fn_0xc02.c` writes both directly/raw; this function touches the same
two offsets through their CLR aliases instead, a real, now-documented
case of one register reached two different ways by two different
callers. Also names two `MODEM` offsets (`0xC4`, `0x24C`). Fully lifted
and verified in `sequencer-decomp/src/fn_0x1a18.c`; the first draft
missed a real write (folding a separate `RAC+0x11C` SET-alias store into
the later `RAC+0x148` one), caught the same way as every other bug this
pass has found — diffing the compiled output against the original
instruction-by-instruction, not trusting the hand-trace on the first
pass.

`fn_0x21d8` is now also fully lifted and verified, in
`sequencer-decomp/src/fn_0x21d8.c`, confirming `fn_0x1a18`'s own
"calibration-settle delay" guess outright: it is a pure CPU-cycle
busy-wait with no hardware register access at all, parameterized
linearly (`count = arg*195 - 111`, decrementing by `30` until negative)
and always returning a dead `0`. The real-time duration is not asserted
-- it depends on the M0+ core's actual clock rate, which this project has
not confirmed, the same stance already taken for `fn_0x23a0`'s unitless
`0xFF01` threshold. First draft compiled the loop away entirely (the
optimizer correctly proved it has no observable effect other than the one
that matters — consuming time), silently breaking this project's
byte-for-byte control-flow verification standard; fixed by marking the
loop counter `volatile`, which is also the behaviorally-correct
annotation for a delay loop, not just a verification workaround.

The remaining as-yet-unexamined callees (`fn_0x13b0`,
`fn_0xa76`, `fn_0x388`, `fn_0xace`, `fn_0xb74`) remain to be traced and
lifted (`fn_0x13b0` and `fn_0xa76` are now done — see below, after
`fn_0x80c`/`fn_0x1af4`; `fn_0x388` is also now done — see §7.50).
None of this changes the `ACTIVETXFCD` picture yet — every one of
these vectors is outside the open stack's own enabled-IRQ set by design,
and no direct evidence ties any of them to the real TXWARM/TX path this
project's own hardware traces exercise. **Checked, not left open: the
`SEQ_LOCAL_FNPTR_3850`/`3854` indirect-callback slots (§7.3) are ruled out
as that link** — a whole-object scan finds them referenced nowhere outside
`fn_0x894` itself, and a scan of stock's own known-working M33 image
(`build-stage2/zephyr/zephyr.bin`) for every secure/non-secure alias of
both addresses finds zero hits either, so nothing in any image this
project can inspect ever initializes them. The `PROTIMER+0x38` connection
to `fn_0xd38` is the one new thread worth pulling on next.

**A second, directly TXWARM-relevant reachability path for `fn_0x1a18`
was found, traced, and then refuted on real hardware -- recorded in full
because the refutation is itself the valuable result.** `fn_0x894`
(called by both IRQ8 and IRQ9 directly, §7.2) has its own previously-
unexamined branch: when `RAC.SR1` (`RAC+0x44`) bit 17 is set and
`RAC.STATUS2.CURRSTATE` is not `TX` (true for `TXWARM` itself), it calls
`fn_0x80c()` — which just re-tests the already-known `0x3EB8`/`0x3F68`
flag pair (§7.3's own "byte/word flag pair... dereferenced by this object
itself") — and on a false result, calls **`fn_0x1af4`**, a function not
previously examined, which **unconditionally calls `fn_0x1a18`** — the
same large "mode-selecting hardware setup" function this pass already
fully lifted (`sequencer-decomp/src/fn_0x1a18.c`), previously only known
reachable via the unreachable IRQ13 vector (`fn_0x2362`). Since
`fn_0x1a18` has its own internal entry gate on the *same* `SR1` bit 17,
reaching `fn_0x894`'s branch at all would guarantee `fn_0x1a18` runs its
full body too — a real, structurally direct link from IRQ8/IRQ9 into a
function this project has already fully decompiled and found does real
PA/LNA/RF-path/`RAC.TX`/`RAC.CMD` hardware setup, not obviously
job-dispatch policy.

**Tested directly on real hardware rather than left as a static
hypothesis.** Flashed stock (`build-stage2/zephyr/zephyr.bin`), set a
hardware breakpoint right after the real `RAC.CMD = TXEN` write
(`stage2-m33.dis:0xe39a`, breakpoint at `0xe3a0`), caught it via
`bg22_reset_to_breakpoint`, and read `RAC.SR1` (`0xA8020044`) directly:
**`0x00000000`** — bit 17 clear. Resumed, slept 100 ms (the already-
established sufficient margin for a full TX cycle, §7.6), re-halted, and
read it again: **still `0x00000000`**, confirmed a second time across a
fresh reset/catch cycle. So on stock itself, during a real BLE
advertising TX, `RAC.SR1` bit 17 is never set at all, from `TXEN` through
full TX completion — `fn_0x894`'s branch into `fn_0x1af4`/`fn_0x1a18` is
not taken in this scenario. This specific path is not the missing piece,
confirmed by measurement rather than assumption, and this thread can be
closed rather than revisited.

**Lifted `fn_0x80c` and `fn_0x1af4` anyway, on the standing principle that
a function existing in stock is itself a reason to understand it, not
just a reason to chase it while it looked load-bearing.** Both fully
verified in `sequencer-decomp/src/fn_0x80c.c`/`fn_0x1af4.c`, matching the
original instruction-for-instruction, branch-for-branch, and literal-for-
literal. `fn_0x80c` is exactly the small predicate already described
above: `(0x3EB8 & 0x6) != 0 && REG32(0x3F68) not in {2,3}`. `fn_0x1af4`
branches once on `RAC.SR1` bit 2 (not bit 17 -- a *different* bit of the
same register than the one gating entry to this function from
`fn_0x894`), does a small, symmetric-but-not-identical pair of writes
into `RAC.SR0`'s SET alias and the SEQRAM word at `0x3F64`/the
"unclaimed" register at `0xA8005068` depending on two bits of a masked
`FRC.SEQIF` read, then unconditionally calls `fn_0x1a18` and `fn_0x572`
before returning. Nothing in this body is job-dispatch *policy* in the
sense `radio_seq.S`'s header excludes -- it's bounded register pokes
gated on read values, the same shape as `fn_0x894`'s own gate bracket --
but it is reached, on real stock hardware, exactly as often as `RAC.SR1`
bit 17 is set during a real TX, which the hardware test above already
measured as never. `fn_0x572` and `fn_0x64c` remain unexamined.

**`fn_0x13b0` (one of the "remaining as-yet-unexamined callees" listed
above) is now also fully lifted and verified**, in
`sequencer-decomp/src/fn_0x13b0.c`, matching the original
instruction-for-instruction (confirmed via the standing compile-and-diff
rule; the first draft misplaced one `RAC.RADIOEN` CLR-alias write and
mis-attributed the function's final store as a second `RADIOEN` write
when it is actually `RAC.SYNTHENCTRL`'s CLR alias clearing
`SYNTHSTARTREQ`, not `VCOSTARTUP` again — both caught by the diff, not
the hand-trace). It is an analog RF/synthesizer bias sequence: pulses
`SYNTH.CMD`, writes a four-step ramp (`0x1000`, `0x200`, `0x100`,
`0x68fd`) to an unnamed `RAC+0x15C` offset with no public field name,
clears `RAC.SYNTHENCTRL.VCOSTARTUP` then later `SYNTHSTARTREQ`, pulses
`SYNTH.VCDACCTRL`'s CLR alias, conditionally sets `RAC.RADIOEN`'s
`PREEN`/`RFBIASEN` bits gated on a SEQRAM flag byte, and finishes by
snapshotting `RAC.STIMER` (already established as live counter state,
§7.43) into a SEQRAM scratch cell. Reachable only from `fn_0x2644`
(IRQ10) and `fn_0x2362`/`fn_0x2390` (IRQ13) — confirmed via grep across
every decompiled file in this directory, neither `fn_0x894` nor
`fn_0xd38` reference it — so, like its callers, it is unreachable on the
open build by the same already-established IRQ8/IRQ9-only scope
argument. Decompiled for completeness, not because it changes the
`ACTIVETXFCD` picture.

**`fn_0xa76` and `fn_0xa80`, the two calls `fn_0x13b0` makes before
touching `SYNTH`/`RAC.RADIOEN` at all, are now also fully lifted and
verified**, in `sequencer-decomp/src/fn_0xa76.c`/`fn_0xa80.c` —
confirmed, same as every other lift this pass, by compiling and diffing
against the original rather than trusting the hand-trace. Together
they're a clock-retimer teardown pair bracketing `fn_0x13b0`'s synth
reconfiguration: `fn_0xa76` (a true leaf — no push/pop, matching its
original) clears `RAC.XORETIMECTRL.XORETIMEENRETIME` then
`RAC.DIGCLKRETIMECTRL.DIGCLKRETIMEENRETIME` (both via their CLR
aliases); `fn_0xa80` then busy-waits on `RAC.XORETIMESTATUS`'s
`XORETIMECLKSEL` bit to clear, clears `XORETIMECTRL.XORETIMERESETN`,
busy-waits on `RAC.DIGCLKRETIMESTATUS`'s `DIGCLKRETIMECLKSEL` bit to
clear, clears `DIGCLKRETIMECTRL.DIGCLKRETIMERESETN`, and finally clears
`RAC.SYMMDCTRL.SYMMDENRSDIG`. All five fields and both status registers
are already public (`RAC_S_register.py`/`RAC_S_field.py`); this lift's
contribution is establishing the actual sequencing (disable retime →
wait for clock-select to settle → hold the retimer in reset → tear
down the symbol-rate modulator's digital enable), not new names.
`fn_0x7ac`/`fn_0x7be` (called mid-body of `fn_0x13b0`) are left as
opaque calls in this lift and remain unexamined, alongside the other
still-outstanding callees `fn_0xace`, `fn_0xb74` (`fn_0x388` is also
now done — see §7.50).

### 7.8 Full `FRC` register-bank diff, stock vs. open, same checkpoint

Tried a broader diagnostic instead of guessing which single field
matters: dumped all of `FRC`'s first 64 words (`0xB8004000`..`0xB80040FC`,
non-secure alias — the secure alias `0xA8004000` reads back all-zero
through the debugger with Secure debug disabled, a real pitfall hit and
corrected here, not a finding) on stock and on the open
`radio-port-hardware-2` bring-up build, both ~30 ms after their own
`RAC.CMD = TXEN` write, and diffed every word against the public
`FRC_S_register.py` names.

**Real, static config differences found, none of them a clean
explanation:**

* `IEN` (`+0x06C`): stock `0xA000` (`BUSERROR`, `TXRAWEVENT`); open
  `0x11` (`TXDONE`, `RXDONE` — exactly what `radio.c` explicitly enables).
  `IEN` only gates whether a pending bit raises a CPU interrupt; it does
  not gate whether `FRC.STATUS` itself advances, so this is a real,
  recorded divergence but not a plausible cause of `ACTIVETXFCD` staying
  clear.
* `RXCTRL` (`+0x04C`): stock sets `ACCEPTCRCERRORS` and
  `RXFRAMEENDAHEADBYTES=4`; open leaves both clear. RX-path configuration,
  active during a TX-only test only if some shared internal state machine
  needs it set regardless of direction — not established either way.
* `SNIFFCTRL` (`+0x084`): differs in several sniff/monitor-mode bits.
  Diagnostic/monitor configuration, not an obvious TX-completion gate.
* `WCNTCMP0` (`+0x020`): stock `0xFF`, open `0xA`. **Not treated as a bug**
  — `radio.c`'s own comment at the `WCNTCMP0` write site already
  documents that this starts at a `0xFF` sentinel and gets dynamically
  replaced with the real remaining word count once the BLE header's
  length byte is parsed; `0xA` is consistent with that replacement having
  already happened for a short test packet by the time this capture was
  taken, not with the sentinel having leaked through unwritten. Comparing
  against stock's *own* real advertising packet (a different length) at
  the same wall-clock offset is not a controlled comparison -- this
  needs re-running with byte-identical packet content/length on both
  sides to mean anything.

**Net result:** a real, useful diagnostic pass that surfaced genuine
config differences worth having on record, but none of them is a
demonstrated cause of the `ACTIVETXFCD` gap, and the comparison itself is
confounded by stock and open transmitting different-length packets in
this test. The concrete next step, if this thread is picked back up: redo
this exact dump with both builds transmitting the *same* fixed-length
test payload, so `DATABUFFER`/`WCNT`/`WCNTCMP0`'s differences (all
plausibly just packet-length artifacts here) can actually be trusted
either way.

### 7.9 `RAC.STCTRL` lines up exactly with a live hardware value, and the "19-entry calibration loop" is NVIC priority setup, not radio calibration

**Superseded in part by §7.59:** the "hardware-latched flag" inference for `RAC.SR3` is withdrawn.

Chasing `RAC.STCTRL`'s own real-hardware value (found as a genuine, non-
packet-dependent config difference in §7.8: `0` on the open stack vs.
`0x01061A7F` on stock) back to its write site in the sequencer object
surfaced real new structure, not just one register.

**The write site, `fn_0x77a`, is called from exactly one place: the
sequencer's own reset handler (local `0x2ade`), immediately after the
already-documented "19-entry calibration-table init loop."** That loop
(`tests/sequencer-probe/README.md`/§7.5.4 both already named it without
resolving what it configures) calls a helper, `fn_0x2a60(index, value)`,
fourteen times with sequential indices `0..13` and value `1`, then five
more times with indices `15`/`17`/`19`/`23`/`27`. Its two possible target
base addresses resolve to **`0xE000E400`** (`NVIC.IPR0`, external
interrupt priorities) and **`0xE000ED1C`** (`SCB.SHPR1`, internal
exception priorities) — both standard, architected Armv6-M/Armv8-M core
addresses, not vendor radio registers. Byte-addressable `NVIC.IPR[n]`
means index `n` directly selects `IRQn`'s priority byte: **this loop sets
priority `1` for, among others, IRQ8 and IRQ9 themselves** — the exact
two interrupts this project's own `radio_seq.S` services. The open
stack's reset handler never calls anything like this and never touches
`NVIC.IPR`/`SCB.SHPR` at all (checked directly: zero references), so
IRQ8/IRQ9 run at whatever priority the core resets to (architecturally
0 — the *highest* priority, not lower as stock explicitly chooses). Not
yet established whether this specific difference has any observable
effect; recorded here because it's a real, concrete, previously-
unidentified configuration gap, not because it's confirmed load-bearing.

**Then, gated behind the loop, `fn_0x77a` itself — fully lifted and
verified in `sequencer-decomp/src/fn_0x77a.c`, matching the original
exactly.** It writes a hardcoded constant to `RAC.STCTRL` — decomposed by
that register's own two documented fields (`RAC_S_field.py`: `STCAL`
bits `[23:0]` = `0x061A7F`, `STSKEW` bit `24` = `1`) rather than left as
one opaque `0x01061A7F` blob, even though `STCAL`'s own numeric content
is still an opaque empirical value this project has no theory for, the
same epistemic status as the `PATRIM`/`IFADCTRIM` trim words elsewhere
in this register block. (Checked and ruled out as a lead: a text search
for "STCAL" elsewhere in the SDK tree only turns up `SYSCFG.CFGNSTCALIB`
— "Configure Non-secure Sys-Tick Cal." — matched purely by the substring
"STCALIB" inside "CFGNSTCALIB", a different peripheral register
entirely, not evidence about this field.) The one real, structural
observation: `STCTRL`'s own bit layout (`STCAL[23:0]`, `STSKEW[24]`) is
identical to the standard Armv6-M `SysTick.CALIB` register's own layout
(`TENMS[23:0]`, `SKEW[24]`, `NOREF[25]`) that this same function reads a
few lines later — plausibly `RAC`'s internal "Sequencer Timer" is modeled
the same way `SysTick` is, with `STCAL` playing an analogous calibration-
tick-count role for whatever clock domain it runs on. That's a structural
parallel, not a confirmed explanation of the number itself. The combined
value is the *exact* one independently measured live on stock hardware
in §7.8, closing that loop. `fn_0x77a` then configures this core's own
private Armv6-M
`SysTick` timer from its own standard `SYST_CALIB.TENMS` field (reload =
`TENMS` times a named, documented-as-stock's-own-choice `25` multiplier,
enabled from the processor clock, no interrupt). Ordinary, well-understood
ARM core
housekeeping, not a radio-specific mechanism.

**The real significance is what gates reaching it at all.** Immediately
before calling `fn_0x77a`, the reset handler busy-waits
(`while ((RAC.SR3 & BIT(17)) == 0) {}`) and then only proceeds if
`RAC.SR3` bit 25 is also clear — both already among this project's five
known `RAC.SR3` literal-pool references (§7.5.2), previously catalogued
as "reads," now connected to a real consequence for the first time.
**Stock does not hang at this busy-wait on real hardware** (it boots and
transmits normally), which means `RAC.SR3` bit 17 must already read `1`
by the time the M0+ core's *own reset handler* reaches this line — well
before either examined firmware image (M33 application, M0+ sequencer
object) has done anything at all. Combined with §7.5.2's already-
exhaustive software-write search (zero write sites found anywhere, in
either object or the entire unlinked M33 RAIL library), this is now a
much sharper, hardware-pinned characterization than "boot-ROM-level
mechanism, origin unknown": `RAC.SR3` bit 17 behaves like a genuine
hardware-latched readiness flag (plausibly an oscillator/PLL-lock-style
signal physically wired into this otherwise-generic scratch register's
bit, not a software-written value at all) that becomes true extremely
early in the M0+ core's own boot, independent of any firmware this
project can inspect — not evidence against the official register
model's documented `0` reset value (that's still correct for what
software can write), just evidence that *something* drives this specific
bit from outside software before the M0+'s own reset handler runs far
enough to observe it. This does not, by itself, change the §7.5.2
conclusion that `SR3` is not on the open stack's own execution path
today; it does make "how does this bit actually become 1 on real
silicon" a substantially better-posed question than it was before this
section, for anyone who picks the `SR3`-origin thread back up.

### 7.10 Ported §7.9's findings into `radio_seq.S` and tested on real hardware: no change

Per standing direction not to leave a promising-looking finding
undeployed just because its root cause isn't fully understood: ported
§7.9's two findings into `radio_seq.S`'s reset handler, before the
`NVIC.ISER0` enable, matching stock's own ordering. **NVIC priority 1
for IRQ8 and IRQ9** via single-byte `strb` stores to `NVIC.IPR_BYTE[8]`/
`[9]` (`0xE000E408`/`0xE000E409`, architected, byte-addressable, leaving
neighboring IRQs' priority bytes in the same 32-bit word untouched —
only these two IRQs are reproduced; stock's other ~17 priority-loop
entries configure IRQs this core never unmasks, so they're moot here and
not ported). **`RAC.STCTRL`'s write and this core's own `SysTick`
setup**, unconditionally — deliberately *without* stock's own `RAC.SR3`
bit-17 busy-wait gate, which would hang this core forever given `SR3`
reads `0` throughout the open stack's own boot (§7.5.2), the same
bounded-instead-of-unbounded precedent already used elsewhere in this
file. Every new constant named and, where the value itself is opaque
(`STCTRL.STCAL`), decomposed by field with that fact stated plainly
rather than hidden — not a magic number, just an honestly-labeled
unexplained one.

Verified before testing: the file reassembles cleanly, and the rebuilt
`radio-sequencer.bin` (from the real `tests/radio-port` hardware
bring-up build, incremental `ninja` rebuild) contains the exact expected
literal values (`0x01061A7F`, `0xE000E408`, `0xE000E409`) — confirmed by
direct byte search, not assumed. All three QEMU suites re-run unchanged
(74/17/14; the sequencer-executing qtest injects its own synthetic
SEQRAM snippet and doesn't exercise this file's real compiled content at
all, so this is an expected no-op there, not a meaningful pass).

**Flashed to real hardware and tested directly.** Caught the same
`TXEN`+1 breakpoint (`0x16606`, address held steady across the rebuild),
resumed, slept 30 ms, re-halted. `RAC.STCTRL` reads back `0x01061A7F` —
confirming that write executed correctly on real silicon, not just in
the binary. `RAC.STATUS` still reads `0x09400000` (`STATE=9`, `TX`,
holding steady, nothing broken by the new code). **`FRC.STATUS` still
reads `0x01100200` — `ACTIVETXFCD` still clear, byte-for-byte identical
to every capture before this change.** (The `NVIC.IPR` bytes are not
independently checkable this way: they are each Cortex-M core's own
*private* peripheral, not bus-addressable from the other core, so
reading `0xE000E408`-style addresses through the M33's own debug
connection reads the M33's own NVIC, not the sequencer's — a real
limitation of this debug setup, not evidence either way about whether
that specific write took effect.)

**Net result: refuted by direct measurement, the same standard every
other hypothesis in this document has been held to.** Neither the NVIC
priority change nor the `STCTRL`/`SysTick` setup has any observable
effect on `ACTIVETXFCD` in this scenario. Both are kept in `radio_seq.S`
regardless — they are still a faithful, now-properly-named reproduction
of real stock behavior this project had not replicated before, and
removing them would only re-introduce a real (if apparently inert)
fidelity gap. The `ACTIVETXFCD` blocker remains open; this closes two
more candidate explanations with evidence rather than leaving them as
untested speculation.

**A plausible reason the NVIC priority half was never going to be
observable, found while verifying QEMU's own fidelity for this same
change in parallel (a separate agent task, `vendor/qemu` commit
`883560fef0`):** QEMU models the M0+ sequencer core's own `NVIC` as
genuine Armv6-M with only **2 priority bits implemented**, not the full
8 the register's own byte width allows for — architecturally normal for
Armv6-M, where implementations commonly support as few as 2 bits, with
the unimplemented low bits always reading `0`. Under a 2-bit field,
stock's own priority value `1` and the architectural reset default `0`
may quantize to the *same* effective priority. This was verified in the
simulator, not on real silicon, but there is no reason to expect the
real EFR32BG22's own Cortex-M0+ implements more priority bits than its
simulator model does — if anything, the simulator should be modeling
the real part's own implemented-bits count. This doesn't change §7.10's
hardware result (still refuted either way), but it's a plausible
*architectural* reason that specific half of the port was never going to
move anything, independent of whatever is actually gating
`ACTIVETXFCD`. The other two areas checked in that same verification
pass (`RAC.STCTRL`'s write mask, and the M0+ core's own private `SysTick`
actually counting) were both already correctly modeled; no QEMU fix was
needed for either.

### 7.11 Stage 1 (Arduino/BGAPI) as a lockstep target: a real finding, and a methodology bug that produced two false ones

Rebuilt stage 1 — Silicon Labs' own open-source Arduino core, the
`ble_minimal` example, built against the same proprietary BGM220 RAIL
archives as stock-stage2 (`hardware-bringup.md`'s original stage-1
artifacts no longer existed on disk; rebuilt from scratch via `arduino-
cli` against `github.com/SiliconLabs/arduino`, installed into
`/home/gavin/Projects/owo-bg22-proprietary-baselines/arduino-cli-home/`).
Compiled and flashed cleanly; `RAIL_StartTx` is reachable and catchable
by a plain symbol breakpoint (`nm` still exposes real RAIL public API
symbols even though internal helpers are hash-named `RAILINT_<hash>` by
Silicon Labs' own build, independent of this project's own compiler
flags). Motivation: stage 1 calls BGAPI directly
(`sl_bt_legacy_advertiser_start` → `RAIL_StartTx`), with no Zephyr/
OpenISA LLL layer in between — fewer *software layers* than stock-
stage2, confirming the premise behind trying it.

**That premise didn't translate into easier static disassembly, though
— and that's a real, recorded lesson, not just friction.** The prebuilt
RAIL library is whole-program LTO'd, which both strips meaningful
internal names (Silicon Labs' own `RAILINT_<hash>` convention) and, more
disruptively, loses Thumb/ARM interworking state across large stretches
of `.text` for `objdump`'s default disassembly mode — large regions
print as nonsense ARM-mode instructions where real Thumb code lives,
requiring `--disassembler-options=force-thumb` plus manually computing
bin-file-offset-to-VMA mappings by hand to get a trustworthy
disassembly. Fewer software layers did not mean an easier-to-read
binary; if anything, stock-stage2's own (non-LTO, Zephyr) build was
easier to statically trace than this one.

**What did work well: hardware watchpoints did not trigger reliably on
this chip (tried both the secure and non-secure `RAC.CMD` aliases,
`bg22_reset_to_breakpoint`-style polling after setting each), consistent
with this project's other secure-debug-disabled limitations — but
breakpoints at known RAIL symbols, combined with single-stepping/
re-breakpointing to walk forward from there, worked perfectly.** This is
the technique that actually cracked the binary: breakpoint at
`RAIL_StartTx` (caught reliably, ~250ms after reset every time),
disassemble forward from there with `force-thumb`, then breakpoint at
specific candidate instructions and read registers live to resolve
branches a pure static read could not (argument values, which path a
conditional actually takes) — faster and more reliable than continuing
to fight the LTO'd disassembly in isolation.

**The finding: `RAIL_StartTx`'s own top-level internal dispatcher
(`fn@0x2951c`, called one level inside the public `RAIL_StartTx` stub at
`0x3708a`) unconditionally clears `RAC.SR0` bit 1 via its CLR alias for a
plain advertisement, before any of the already-known TXEN-adjacent
handshake this project's own `radio_tx_enable()` already reproduces.**
Confirmed live, not inferred from static reading alone: breakpointed the
gating branch for a sibling, conditionally-touched bit (bit 9, same
function) and read `r0`/`r5` directly —`r5` (`RAIL_TxOptions_t`) is `0`
for a plain `ADV_IND` from this sketch, and `r0` (an internal helper's
return value) is `0` at the branch that would skip the bit-9 block,
confirming that block *is* skipped and bit 1's clear executes
unconditionally on the only path this scenario actually takes.

**Ported it, tested on real hardware — and this whole thread, through
the "six-cycle margin" and the "binary-searched 1-nop-wide threshold"
below, turned out to be built on a methodology bug, not a real hardware
finding. Retracted in full; recorded here so the mistake isn't repeated,
not as a result to trust.**

The test pattern used throughout was: set a breakpoint right after
`TXEN`, catch it via `bg22_reset_to_breakpoint`, `resume`, `sleep N`,
then `bg22_require_sram_execution` and read registers. **That pattern
never removed the breakpoint before `resume`.** `radio_tx_enable()` is
called repeatedly during real advertising (the OpenISA stack re-arms TX
periodically); if a *later* call reaches the same breakpoint address
again during the `sleep`, the core halts there a second time, and the
subsequent `bg22_require_sram_execution` silently observes whatever
state exists at *that* re-trigger — not "N ms of settled state after the
first `TXEN`" as assumed. The apparent "regression" and the apparent
"1-nop-wide timing cliff" were both artifacts of this: different
instruction counts shift exactly when later calls land relative to the
sleep window, making the breakpoint re-trigger (and therefore what gets
measured) depend on instruction count without any real hardware timing
margin being involved at all.

**Caught by re-running the same tests with `rbp <addr>` added before
`resume`, which is the fix.** Every single one of the "broken" cases
reverses under the corrected methodology:

* The `SR0` bit-1 port, re-tested at the same `TXEN`+30ms checkpoint
  with the breakpoint properly removed: `RAC.STATUS=0x09400000` (`TX`,
  holding) — matching the unmodified baseline exactly, not the
  `0x10400000` "regression" originally reported.
* Six `nop`s at the same insertion point, re-tested the same way:
  `RAC.STATUS=0x09400000` (`TX`, holding) — also matching baseline, not
  the originally-reported `OFF`.
* Checked at 5/10/20/30ms individually, both with and without the
  breakpoint bug present, to confirm the mechanism: with the bug
  present, results were inconsistent run to run in exactly the way a
  stray re-trigger would produce; with `rbp` added, every checkpoint
  from 5ms through 30ms reads the same steady `TX` state, for every
  instruction count tried.

**What survives from this whole thread, and what doesn't.** The
earlier, purely static/single-breakpoint finding that stock's
`RAIL_StartTx` clears `RAC.SR0` bit 1 for a plain advertisement still
stands — that was established by reading registers at single, one-shot
breakpoints (no `resume`+`sleep` involved), which this bug cannot have
affected. What does **not** survive: the claim that porting it regresses
anything, the "six-cycle timing margin," the binary-searched "exactly
one `nop` wide" threshold, and every piece of reasoning built on top of
those ("this is likely the most load-bearing fact in this document,"
the race-condition speculation, the queued single-step experiment to
find the 1-to-2-nop transition). None of that was real; there is no
evidence of any timing sensitivity at this call site at all. `SR0` bit 1
remains unported, not because porting it is dangerous, but because
(per the original, still-valid finding) it's already `0` on both images
at `TXEN` regardless, so clearing it again changes nothing either way.

**The actual lesson, worth keeping:** any `bg22_reset_to_breakpoint`-
then-`resume`-then-`sleep`-then-recheck test on this project must either
remove the breakpoint first (`rbp <addr>`) or use a breakpoint address
that cannot legitimately be hit twice, or the "later" read is measuring
an uncontrolled, unknown point in time, not the intended one. This
applies to every test of this shape anywhere in this document; none of
the *other* sleep-based tests happened to hit this specific failure mode
(most used one-shot captures without a `resume`+`sleep` afterward, or
targeted code that genuinely runs once), but this specific pattern is
now flagged for anyone writing a new one.

### 7.12 `ACTIVETXFCD` status: every hypothesis tried so far, consolidated

**Superseded by §7.60:** root cause found (the synthesizer was off during TX).

A long session's worth of hypotheses have now been tried and ruled out
on this specific question. Consolidated here as one reference, so
nobody re-spends effort re-testing any of these:

**Ruled out, with hardware or official-source evidence, not just static
reasoning:**

* `RAC.SR3` (§7.5.2, §7.9) — open code never reads it anywhere; its
  documented reset value (`0x00000000`) matches what the open stack
  already observes; not a fidelity gap.
* `fn_0x894`'s `SEQ_LOCAL_FNPTR_3850`/`3854` indirect-callback slots
  (§7.7) — written by no image this project can inspect, on either
  core; most likely dead code on stock too.
* `fn_0x894`'s `RAC.SR1` bit-17-gated path into `fn_0x1af4`/`fn_0x1a18`
  (§7.7) — directly measured on real stock hardware: bit 17 is `0`
  throughout a real TX, so this path is never taken even on stock.
* The NVIC priority setup and `RAC.STCTRL`/`SysTick` setup recovered
  from the sequencer's own reset handler (§7.9, §7.10) — ported,
  hardware-tested (correctly, after the §7.11 methodology fix), no
  observable effect either way.
* `RAIL_StartTx`'s `RAC.SR0` bit-1 clear (§7.11) — real on stock, but a
  no-op to port: the bit already reads `0` on the open stack at `TXEN`
  regardless.
* A broad `FRC` register-bank diff at the same `TXEN`+30ms checkpoint
  (§7.8) — found real config differences (`IEN`, `RXCTRL`,
  `SNIFFCTRL`), none plausibly causal; `WCNTCMP0`'s difference explained
  by the driver's own documented dynamic-length replacement, not a bug.
* `FRC.PKTBUFCTRL`'s `PKTBUFTHRESHOLDEN` — `0` (disabled) on both
  builds consistently; not a divergence.
* The channel-37 `SYNTH`/`MODEM`/`RAC` calibration words already
  characterized in §4.2 — injected on real hardware, no change.

**Confirmed working, not in question:** `RAC.STATUS` reliably reaches
and holds `TX` (state 9); `RAC.STCTRL` writes correctly; the BUFC
per-byte staging sequence matches stock's own observed pattern;
`RAC.SR3`'s two bits get cleared at the right point; the SEQIF-ack retry
bug is fixed; the M0+ sequencer's own gate-pulse/ack mechanics match
stock exactly, byte-for-byte, for everything this project has ported.

**Not yet tried, genuinely still open:**

1. **A trustworthy, synchronized live snapshot of the M0+ sequencer
   core's own state during a real, in-flight TX attempt** (§7.11's final
   thread). Attempted and inconclusive this session — connecting to the
   M0+ core's own debug AP (AP3, `efm32.seq` in
   `tests/sequencer-probe/xg22-dual-core.cfg`) appears to interact badly
   with whatever resets the chip (either an implicit reset on examine,
   or the M33's own `SEQRESET`/`SYSRESETREQ` activity, not yet
   distinguished), and running both cores' targets simultaneously
   introduced SWD-bus contention that broke the already-reliable M33-
   side breakpoint catch. Needs either a verified-genuine-halt proof for
   `efm32.seq` (the same shape as `bg22_require_sram_execution`, which
   does not exist for this target yet) or a way to inspect the M0+ core
   without disturbing the M33's own timing-sensitive boot sequence.
2. ~~**`fn_0xd38`'s own main body beyond the already-decoded `far_abort`
   gate and distance<8 subset**~~ **Closed, §7.13.** Fully re-verified;
   found and fixed one real production bug (the `distance` computation's
   register addresses), confirmed it changes the measured value but not
   observable behavior at this project's own test checkpoint. The
   function's remaining logic (a polling loop, a calibration block, a
   7-byte record write) is understood but deliberately not ported, same
   rationale as §7.3.1's bit-22 pulse.
3. ~~**A from-scratch review of what, if anything, stock's BLE host/
   controller stack does between `sl_bt_legacy_advertiser_start`
   returning and the actual `RAIL_StartTx` call**~~ **Closed, §7.14.**
   Mapped the full call chain down to `RAIL_StartTx`; found and tested
   one concrete idea from it (`FRC.CMD.CONVINIT` re-armed per-TX, not
   just per-session) -- ruled out, no change on hardware. Finished
   tracing the one remaining unexplored branch (a function-pointer-gated
   state-machine wait) and found it touches only `RAC.SR0`/a RAM word,
   never `FRC` -- which also means the idea's original motivation (an
   unattributed `FRC.CMD` call site from §4.2) does not hold up as
   described; corrected in §7.14 rather than left standing.

### 7.13 Thread 2 pursued: `fn_0xd38`'s full body re-verified, and a real (but behavior-preserving) production bug fixed

Took up open thread 2 from §7.12. `sequencer-decomp/src/fn_0xd38.c`'s
second half (0xde6 onward) was marked "lower confidence" since the
project's early days, carried forward even through §7.3.1's own
re-verification pass (which only re-checked the `far_abort` gate and the
`distance < 8` subset, not everything past it). Going through the rest
instruction-by-instruction against the raw literal pool (re-dumped
directly via `od -A x -t x4` over `original/0xd38.bin`, not trusted from
the prior disassembly reading) found it was not just low-confidence --
it had several concrete, wrong claims in it:

* **The `distance` computation's own inputs were wrong.** The prior
  lift read `PROTIMER_0X38`/`FRC_0X1C` via fabricated pc-relative
  literal loads. The real instructions (`ldr r2,[r6,#56]`/`ldr
  r0,[r6,#88]`) are **not** pc-relative loads at all -- they are offset
  reads off the *same* `r6` register the whole second half keeps loaded
  as a base pointer to `OBS_BLOCK_AA000054` (`BUFC.BUF[1].XWRITE`,
  §7.3). The real addresses are `0xAA00008C` and `0xAA0000AC`. The bit
  extraction for the first term was also backwards: `lsls r2,r2,#29;
  lsrs r2,r2,#29` isolates bits `[2:0]`, not bits `[31:29]` as
  previously written (a left-shift by 29 discards everything above bit
  2 before the right-shift ever runs).
* **An entire polling loop and an entire calibration block were
  missing.** The real code loops back to local `0xdaa` (re-running most
  of the function's body, including the `far_abort` gate) as long as
  `OBS_BLOCK_AA00011C` bit 8 stays set after a calibration block (local
  `0xe5a..0xea8`) runs; that calibration block touches a previously
  unnamed register (`0xA8014008`) and a previously unnamed SEQRAM-local
  byte flag (`0x3EC0`), neither mentioned anywhere in the project before
  now.
* **The function's own tail was wrong in several ways**: two addresses
  (`0x10e8`/`0x10ec`) were wrongly assumed to be `SEQ_LOCAL_3E73`/
  `SEQ_LOCAL_3EDC`; they are actually a different SEQRAM-local (`0x385C`)
  and a previously unnamed peripheral register (`0xA8005084`). The
  7-byte record's `[6:7]` field read `PROTIMER_0X11C`; it actually reads
  `MODEM_0X44`. And, the biggest structural miss: by the time either of
  the function's two convergence points (`0xebc`/`0xed0`) are reached,
  the stack slots originally holding `stack_flag` and `bit6_of_gate`
  have been **overwritten** with the evolving `seqif_saved` and
  `local_3f64_plus8` values -- so the function's real return value is
  the final `seqif_saved`, not `stack_flag`, and its final call to
  `fn_0x572` passes `(flags, local_3f64_plus8)`, not `(stack_flag,
  state_nibble)` as previously written.

All of this is now fully re-lifted in `fn_0xd38.c` (compiles cleanly,
`-Wall -Wextra`, every literal pool address re-verified against the raw
bytes) with an extensive header comment documenting the correction.

**One of these -- the `distance` computation's wrong addresses -- was
not just a decomp-notes error. It had already been ported into
production** (`radio_seq.S`'s `EFR32_PROTIMER_0X38`/`EFR32_FRC_0X1C`,
§7.3.1's "conservative, deterministic subset," hardware-tested and kept
in the tree). Fixed the same way in `radio_seq.S`: the two operands are
now `EFR32_OBS_BLOCK_AA000054_0X38`/`_0X58` (`0xAA00008C`/`0xAA0000AC`),
and the bit extraction is a plain `& 0x7` instead of the wrong `lsrs
r1,r1,#29`. QEMU 77/17 still pass. Assembled, rebuilt
(`build/radio-port-hardware-2`, 158204 B flash, +8 B from the one extra
literal pool word), and re-tested on real hardware at the same `TXEN`+
30ms checkpoint used throughout §7.6, with the breakpoint properly
removed before `resume` (§7.11's methodology fix):

* `EFR32_SEQ_TRACE_DISTANCE` now reads **`0x40`** (64) instead of the old
  wrong-address `0x1ffe` (8190) -- confirming the fix changed what gets
  measured, not just what it's named.
* `RAC.STATUS`/`FRC.STATUS` at the same checkpoint: **byte-for-byte
  identical to baseline** (`0x09400000`/`0x01100200`), both before and
  after the fix. Both `64` and `8190` are `>= 8`, so the conservative
  pulse this project ported was never reachable either way -- the wrong
  addresses happened to produce a wrong number that was *also* on the
  "never fires" side of the threshold. This is a real, verified
  silicon-fidelity bug (QEMU and any future static analysis of this
  function would have computed a different, equally wrong, number from
  the old addresses) with zero observable behavior change for this
  project's own current test scenario -- recorded per this project's own
  "every divergence must be recorded and fixed" rule regardless of
  whether it happens to matter for `ACTIVETXFCD` today.

**Everything else found in this pass (the loop, the calibration block,
the tail's real addresses/return value) remains deliberately unported**,
for the same reason §7.3.1 already gave for not porting the bit-22
pulse's full gating logic: the broader job-dispatch state it depends on
(`flags`, built from state this project does not reproduce) is not
understood well enough to know it's safe to fire unconditionally. Thread
2 from §7.12 is now closed as "fully re-verified, one real bug fixed,
nothing new found that unblocks `ACTIVETXFCD`." Threads 1 and 3 remain
open.

### 7.14 Thread 3 closed: link layer traced to `RAIL_StartTx`; the per-TX `FRC.CMD.CONVINIT` idea it suggested, tested and ruled out; a mis-attribution corrected

Took up open thread 3 from §7.12: tracing stock's BLE host/controller
stack (stage 1, §7.11) between `sl_bt_legacy_advertiser_start` returning
and `RAIL_StartTx`, via the call chain `sl_bt_legacy_advertiser_start` ->
`sli_bt_cmd_legacy_advertiser_start` -> `advertiser_start` -> ... ->
`hci_le_set_extended_advertising_enable` -> `ll_hciCall`, eventually
reaching the link-layer radio launch helpers `ll_radioTxStart`/
`ll_radioFirstTxSchedule`. Both call `ll_radioSetTxTransitions()`
immediately before `RAIL_StartTx`/`RAIL_StartScheduledTx` -- and
`ll_radioSetTxTransitions` is a thin wrapper around the **public** RAIL
API `RAIL_SetTxTransitions()`.

This connects directly to an existing, previously unexploited note in
§4.2: stock's static disassembly found three direct stores of
`FRC.CMD = 0x201` (`RXABORT | CONVINIT`), two from the named RAIL
initialization helper this project's own `radio_setup()` already
reproduces, and a third "in a larger RAIL state-transition helper" --
left unidentified at the time. `RAIL_SetTxTransitions` **is** that
helper: stock calls it (and, through its call chain, re-issues
`FRC.CMD.CONVINIT`) immediately before **every** TX attempt, immediate
or scheduled -- not once per session, the way `radio_setup()` currently
does it. (Stock's own wrapper caches the last-requested transition
target and skips the real `RAIL_SetTxTransitions()`/register-write call
when the request hasn't changed since the previous TX, so even stock
does not literally rewrite `FRC.CMD` on *every single* call when
advertising repeats the same transition target -- but it is reachable
from the TX path itself, not just from one-time init, which `radio_setup()`-only
timing cannot reproduce if anything between setup and the first `TXEN`
needs it freshly re-armed.)

**Tested the direct hypothesis on real hardware: re-issuing
`FRC.CMD = EFR32_FRC_CMD_RXABORT | EFR32_FRC_CMD_CONVINIT` (both already-named
constants, no new magic numbers) in `radio_tx_enable()` immediately before
`RAC.CMD = TXEN`.** QEMU 77/17 still passed. At the standard `TXEN`+30ms
checkpoint (breakpoint properly removed before `resume`, §7.11): **no
change** -- `RAC.STATUS=0x09400000`/`FRC.STATUS=0x01100200`, byte-for-byte
identical to every prior reading. Reverted; not kept in the tree, since
unlike the already-ported SR0/SR1 gate mechanics this was a hypothesis
test with a clean negative result, not a confirmed stock behavior worth
preserving for its own sake.

**What this does and does not establish.** It rules out "the frame coder's
convolution path simply needs re-arming once per TX attempt" as the
`ACTIVETXFCD` blocker -- a concrete, testable idea, now eliminated rather
than left as a guess.

**Correction, found by finishing the trace rather than leaving it as "not
yet traced": the attribution of §4.2's third `FRC.CMD` call site to
`RAIL_SetTxTransitions` does not hold up, at least not for stage 1's RAIL
build.** The function pointer at `0x29878` (reached from
`RAILINT_4316ca4fc8f26ef86197e5e28c021e83`'s `CORE_EnterCritical`-guarded
call) is not a hidden `FRC.CMD` write -- it is a state-machine gate: while
`RAC.STATUS.STATE == 11`, busy-wait on `RAC.SR0` bit 16 clearing (re-reading
`STATUS` each iteration); for any other state, return a simple two-value
classification (state is `9`/TX or `12`, vs. not) with no register writes
at all. The call chain's only actual side effects, fully traced now, are
the already-documented `RAC.SR0`-CLR-alias write (clearing bit 2, the same
bit this project's own `EFR32_RAC_SR0_WAIT_BIT` already names) and a
RAM-resident state-word update at `0xB0003EF0` -- nothing touches `FRC` at
all, in either the gated success path or the busy-wait path. So either
§4.2's original stage-2 disassembly attributed that third call site to a
*different* RAIL helper that merely happens to run near a state-transition
change (stage 2 and stage 1 are different RAIL library builds/versions,
and §4.2 never named the helper precisely enough to re-locate it in stage
1), or the attribution was simply a guess that didn't hold up to a full
trace. Recorded here rather than left standing uncorrected, per this
project's own rule to fully understand a disproven hypothesis before
moving on. Thread 3 is now closed as "link-layer call chain to
`RAIL_StartTx` fully mapped; the one concrete idea it suggested is tested
and ruled out; the specific attribution that motivated the idea does not
hold up either, corrected rather than left stated."

### 7.15 Thread 1 closed: the M0+ TXWARM handler isn't stuck anywhere -- proven directly, not inferred, so AP3 live-CPU tooling wouldn't have helped

Thread 1 from §7.12 asked for a trustworthy way to inspect the M0+
sequencer core's own live state (PC, registers) during an in-flight TX,
on the theory that it might be caught doing something relevant partway
through. Re-reading `radio_seq.S`'s own control flow first (rather than
immediately fighting the AP3/dual-core OpenOCD SWD-contention problem
again) found a cheaper, more direct way to answer the *specific* question
that tooling was for.

`efr32_radio_seq_exception` is one shared handler for both `IRQ8`
(TXWARM) and `IRQ9` (TX); the label `2:` that both paths converge on sits
**after** the TXWARM-only block, meaning the TX/`IRQ9` entry always skips
both of that block's bounded wait loops entirely and returns almost
immediately -- it was never a candidate for being "stuck." The TXWARM
entry, though, has two genuinely bounded poll loops (`EFR32_RAC_TXWARM_WAIT_BOUND`
= `0x10000` iterations each): one polling `RAC.SR0`'s `WAIT_BIT`, one
polling `RAC.SEQIF`'s TXWARM-ack bit. Critically, **the hardware's own
`RAC.STATUS` state machine does not visibly wait on either loop** --
`RAC.STATUS` reaching `TX` (state 9) says nothing about whether the M0+
firmware that reacted to the *preceding* TXWARM interrupt ever finished.
If either loop were silently timing out (spinning for its full bound
before giving up), the M0+ core could in principle still be active,
spinning, well past the point `RAC.STATUS` already reads `TX` -- a
genuinely plausible mechanism, and a case where catching the M0+ core's
live PC really would have been the right tool.

**Tested directly instead of inferring: added two diagnostic trace
words** (`EFR32_SEQ_TRACE_WAIT_BIT_REMAINING`/`_SEQIF_ACK_REMAINING`,
`radio_seq.S`), recording each loop's remaining counter (`r3`) at the
exact point it exits -- `0` means it ran out the full bound without the
condition ever being met; the untouched bound value means it exited on
the very first check, no spinning at all. QEMU 77/17 still pass.
Hardware-tested at both the standard 30ms and 100ms checkpoints (same
methodology as §7.6/§7.11, breakpoint removed before `resume`): **both
words read exactly `0x10000` -- the untouched bound -- every time.**
Both loops succeed on their very first iteration; the M0+ TXWARM handler
is not spinning, has never been spinning, and returns to `WFI` within
microseconds of being entered.

**This closes the thread by disproving its premise, which is a complete
answer, not a non-answer.** The M0+ core is provably idle, not doing
anything relevant, for the entire 30-100ms window this project has been
probing `RAC.STATUS`/`FRC.STATUS` at. Building the AP3 dual-core live-CPU
inspection tooling thread 1 asked for would not have revealed anything
new about `ACTIVETXFCD`, because there is nothing happening on the M0+
side left to catch -- the stuck state is entirely in the RAC/FRC
autonomous hardware state machine, which this project's existing M33
register access already inspects exhaustively (§7.4-§7.14). All three
threads from §7.12 are now closed.

### 7.16 Two new hypotheses from `FRC.CTRL`'s public fields, both tested and ruled out

With §7.12's three threads closed, re-examined `FRC.CTRL` directly: the
public register model (`FRC_S_register.py`) names its `TXFCDMODE` field
("TX Frame Coder Mode") -- a field that, by name alone, looks like the
single most direct candidate for controlling whether the TX frame coder
can ever become active. The open HAL never writes a named `EFR32_FRC_CTRL_*`
constant anywhere (`grep` for `FRC_CTRL` in `radio.c` returns nothing),
which looked at first like exactly the kind of missing-register gap this
whole investigation has been looking for.

**It is not.** `radio_phy_set(PHY_LEGACY/PHY_1M, ...)` already applies
`efr32_ble_1m_delta_table`, a public, Zlib-licensed, directly-cited
configurator table (`ble_efr32xg22_configurator_out.c:474-481`) that
includes `FRC.CTRL = 0x000407A0` -- decoding to `TXFCDMODE=2`,
`RXFCDMODE=2`, `BITSPERWORD=7`, `RATESELECT=0`, `TXPREFETCH=0`,
`SEQHANDSHAKE=0`. Read `FRC.CTRL` directly on real hardware (no TX
needed, it's static config): **`0x407a0`, exactly matching.** Since this
value is copied verbatim from stock's own public source, it cannot be
the divergence -- stock and the open stack necessarily agree here by
construction. (Flagging the gap anyway: this value, like the rest of
`efr32_ble_1m_base_table`/`_delta_table`, is a cited-source magic number
in a raw address/value table, not decomposed into named fields --
real for every entry in these tables, not just this one. Worth a
dedicated pass if/when the project wants to resolve that category of
"open-source completeness" gap; out of scope to do wholesale here.)

**Second hypothesis, same category, also tested: `FRC.CMD.STATEINIT`**
(`FRC_S_field.py` bit 11, write-only) resets FRC's own frame-state
machine -- plausible because `FRC.STATUS.FSMSTATE` (bits `[24:20]`) is
observed stuck at a constant `17` before and after every TX attempt
(§7.6), and because the open HAL's existing `FRC.CMD` init write only
sets `CONVINIT` (the *convolutional encoder's* init, bit 9) which is
irrelevant for an uncoded 1M PHY (`FECCTRL=0`) -- so the existing
"FRC init" write may not be touching the right state machine at all.
Tested issuing `FRC.CMD.STATEINIT` immediately before `RAC.CMD.TXEN`
(already-named constant, no new magic number). QEMU 77/17 still passed.
Hardware-tested at the standard 30ms checkpoint: **no change** --
`RAC.STATUS=0x09400000`/`FRC.STATUS=0x01100200`, identical to every
prior reading. Reverted; not kept, same as §7.14's `CONVINIT` test.

Both negative results are concrete progress, not dead ends: they rule
out FRC's own static configuration and its two most plausible init/reset
commands as the cause, narrowing further toward either a genuinely
undocumented control path, or a precondition from somewhere this project
has not yet looked at all.

### 7.17 `BUFC.CMD.PREFETCH`: a strong-looking lead from the codebase's own history, tested and understood, not just ruled out

`efr32_tx_fifo_publish()` (the function run before every `TXEN`) writes
`BUFC` channel `CLEAR`/`CFG`/`ADDR` then each byte individually through
`WRITEDATA` -- but never issues `BUFC.CMD.PREFETCH` (bit 1, the public
`BUFC_BUF_CMD` field named in `build/reference-models/efr32bg21_bufc.h`).
`radio_pkt_rx_set()`'s own existing comment already flags the asymmetry:
*"Unlike TX, its [RX] helper does not issue PREFETCH"* -- strongly
implying stock's TX-side helper does. `EFR32_BUFC_CMD_PREFETCH` itself
was defined but completely unused anywhere in `radio.c`, which looked
exactly like a leftover from a previously-tried, then-abandoned fix.

**Tested directly: issuing `BUFC.CMD.PREFETCH` immediately after the
per-byte `WRITEDATA` loop, before `TXEN`.** QEMU 77/17 still passed.
Hardware-tested at the standard 30ms checkpoint: **no change** --
identical `RAC.STATUS`/`FRC.STATUS` to every prior reading. Reverted.

**Understood why, not just logged as a miss.** `EFR32_BUFC_WRITEOFFSET`
(offset `+0x008`, a real public register, distinct from the per-byte
`WRITEDATA` port at `+0x018`) is how `RAIL_SetTxFifo`'s *zero-copy*
setup path declares "N bytes are already present starting at `ADDR`"
*without* touching them one at a time -- and `PREFETCH`'s role, per the
project's own older characterization of that path (`efr32_radio_table_apply`-adjacent
code history, and the already-present comment this section started
from), is to tell `BUFC` to treat that already-set `WRITEOFFSET` as
valid and begin consuming from it. Our `WRITEDATA`-based per-byte fill
(deliberately chosen to match stock's own DWT-observed *per-packet*
write pattern, not its one-time FIFO-setup path) already advances
`WRITEOFFSET` as a side effect of each write -- so there is no new
information for `PREFETCH` to convey in this path; it would only matter
for the *other*, bulk/zero-copy `WRITEOFFSET`-assignment path this
project deliberately does not use. This is why `PREFETCH` tests as a
no-op here: not because the register doesn't matter, but because it
belongs to a different FIFO-population mechanism than the one actually
exercised. Consistent with the already-present code comment that
explicitly warns against conflating the two
("`SetTxFifo's initial-count path were RAIL_WriteTxFifo`").

### 7.18 `RAC.CMD.FRCWR`/`FRCRD`: named exactly for this handoff, both orderings tested, no change

`RAC.CMD` (the same write-only command register `TXEN`/`TXDIS`/`RXDIS`
already use) has two fields this project had never considered:
`FRCWR` (bit 10) and `FRCRD` (bit 11), both `write-only`
(`RAC_S_field.py`). By name alone these look like exactly the RAC-side
trigger this whole investigation has been missing -- a command telling
`FRC` to start writing (transmitting), distinct from `TXEN`'s own role
of committing `RAC`'s own state machine to the TX sequence (synth/PA
ramp) without necessarily also kicking `FRC`'s engine.

**Tested both plausible orderings on real hardware** (same low-risk
category as the already-used `RAC.CMD` bits): `TXEN` then `FRCWR`, and
`FRCWR` then `TXEN`. QEMU 77/17 passed for both. Hardware-tested at the
standard 30ms checkpoint: **no change in either ordering** --
`RAC.STATUS=0x09400000`/`FRC.STATUS=0x01100200`, identical to every
prior reading. Reverted both; not kept.

This does not fully explain *why* `FRCWR` has no effect (unlike §7.17's
`PREFETCH`, no deeper mechanism was found to explain the null result --
it is a clean negative, not yet a *understood* one). Worth remembering
if a future pass finds evidence that `FRCWR`/`FRCRD` require some other
precondition this project hasn't established, rather than re-testing
blind.

### 7.19 A fresh, full `FRC` register-bank diff against real stock hardware: one dead end closed, one methodology caveat worth recording

With `build-stage2/zephyr/zephyr.bin` (the project's own long-standing
stock reference image) still available, re-ran a full, fresh `FRC`
register-bank capture (every word, offset `0x000`..`0x120`) against
real stock hardware at the same `TXEN`+30ms checkpoint used throughout
this document, and diffed it against the open stack's own capture at
the identical checkpoint.

**First, closed a real dead end along the way.** Stock's actual
`RAC.CMD.TXEN` store is at `0xe39a` (confirmed directly); the
instruction immediately after it (`0xe3a0`, the breakpoint address
already used throughout this document -- no citation error, re-checked)
is `blx r3` into a function pointer loaded from a RAM-resident callback
table (`0x200001d0`+`0x4`). Caught stock live and read the resolved
address directly from the register: it resolves to a completely trivial
stub (`movs r0,#0; bx lr`) -- a RAIL extensibility point that is a no-op
in this configuration. Nothing to port, nothing to replicate; a genuine
"nothing here" result, not a loose end.

**The register-bank diff itself found real differences, but almost all
of them resolve to a single, useful methodology lesson rather than new
leads.** `WHITEINIT` (`0x038`, stock `0x67` vs. open `0x65`) and
`WCNTCMP2` (`0x028`, stock's channel-tracking reuse of this register,
stock `0x27`=39 vs. open `0x0`... decoded as channel 37) **agree with
each other on which channel each build was actually on at the moment
of capture** (`WHITEINIT = 0x40 | channel` per BLE's standard whitening
seed derivation: `0x67 = 0x40|39`, `0x65 = 0x40|37`) -- confirming stock
had already cycled forward to a *later* advertising event on channel 39
by 30ms, while the open stack is still parked on its first and only
attempt, channel 37. Every other BUFC/FRC "in-progress" counter that
differed (`DATABUFFER`, `WCNT`, `WCNTCMP0`, `OTACNT`) is consistent with
the same explanation: these are live, continuously-updated progress
counters, not static config, so comparing them at a fixed wall-clock
offset compares *different advertising events*, not the same moment on
two builds. **This is a real caveat for any future register-snapshot
comparison of this kind**: a fixed-time checkpoint is only a trustworthy
comparison for registers that are genuinely static configuration,
written once and never touched again (`DFLCTRL`, `MAXLENGTH`, `CTRL`,
`PUNCTCTRL`, `AUTOCG`, `FCD0..3` -- all of which matched exactly, as
expected) -- not for anything that updates live during a TX, where stock
simply being faster and further along produces a difference that looks
like a config bug but isn't one.

**`RXCTRL` (`0x04c`, stock `0x262` vs. open's own `0x60`, exactly
matching what `radio_setup()` writes) and `SNIFFCTRL`/`TRAILRXDATA`
differences are plausibly real scope gaps** (stock's full legacy
advertiser listens for scan/connect requests after each advertisement;
this project's minimal OpenISA-contract driver does not), not
necessarily anything connected to `ACTIVETXFCD` specifically -- flagged
for completeness, not pursued further here since they're RX-side, and
`ACTIVETXFCD`'s own TX-side engagement is what's blocked.

No new `ACTIVETXFCD` lead from this pass, but a real methodology
finding worth keeping: **if this kind of snapshot diff is tried again,
restrict it to confirmed-static registers, or capture at `TXEN`+a few
hundred microseconds instead of tens of milliseconds**, to avoid
comparing two builds that are no longer on the same advertising event.

### 7.20 `FRC.WCNTCMP0` proves the frame coder *does* process the real packet before stalling; `TXSUBFRAMERESUME` tested, no change

`radio_pkt_tx_set()` writes `FRC.WCNTCMP0 = EFR32_FRC_DYNAMIC_LENGTH_PENDING_WORD_COUNT`
(`0xFF`, confirmed the *only* write site in `radio.c`) once, well before
`TXEN`. On real hardware, at the standard checkpoint, it reads back
**`0xA` (10)** -- not `0xFF`. Since nothing in this project's own code
writes it again, this is a genuine, hardware-driven update: `FRC`
parsed our real packet's own length byte (`DFLCTRL`'s `DFLOFFSET=1`
field, the BLE PDU's own self-reported length) and correctly computed
`total_frame_length - 1` for an 11-byte frame -- exactly consistent
with the project's own minimal legacy-advertisement PDU (2-byte header
+ a small `AdvA`/`AdvData` payload). This is a materially different,
more specific finding than "the frame coder never engages": **it reads
and correctly interprets the real packet's header before stalling.**
Whatever is missing is strictly downstream of length determination, not
upstream of it.

**Timing: the stall point is reached in microseconds, not milliseconds.**
Swept `sleep 0/1/2/5` ms after `TXEN` (bounded below only by
`bg22_require_sram_execution`'s own ~10ms halt overhead): `RAC.STATUS`,
`FRC.STATUS`, and `FRC.WCNTCMP0` are already fully settled at their
final, permanently-stuck values at every one of those checkpoints,
identically. There is no gradual approach to the stuck state visible at
this resolution -- whatever decides the outcome happens essentially
immediately after `TXEN`, then holds forever.

**Tested the next concrete idea from `FRC.CMD`'s remaining bits:
`TXSUBFRAMERESUME` (bit 6)**, named for exactly the shape of problem
`WCNTCMP0` now points to -- resuming a paused subframe feed, as
opposed to the two already-tried init bits (`CONVINIT`, `STATEINIT`)
which reset state rather than resume a pause. Added immediately after
`RAC.CMD.TXEN` (already-named constant, no new magic number). QEMU 77
still passed. Hardware-tested at the standard checkpoint: **no
change** -- identical `RAC.STATUS`/`FRC.STATUS`/`FRC.WCNTCMP0` to every
prior reading. Reverted; not kept.

This narrows the remaining search space concretely: the blocker is not
in `BUFC`'s data path (bytes are visibly reaching `FRC` and being
parsed correctly), not in `FRC`'s own length-determination logic, and
not released by any of `FRC.CMD`'s resume/init bits tried so far
(`CONVINIT`, `STATEINIT`, `TXSUBFRAMERESUME`). The remaining untried
`FRC.CMD` bits (`FRAMEDETRESUME`, `INTERLEAVEWRITERESUME`,
`INTERLEAVEREADRESUME`, `CONVRESUME`, `CONVTERMINATE`,
`INTERLEAVEINIT`, `INTERLEAVECNTCLEAR`, `BLOCKINIT`, `RXRAWUNBLOCK`) are
mostly interleaver/convolution-specific, which an uncoded 1M PHY
(`FECCTRL=0`) should not need -- plausible next candidates only if that
assumption turns out wrong.

### 7.21 `RAC.STATUS.TXENS` reads 0 while `STATE=9` claims TX; `RAC.CMD.PAENSET` tested, no change -- and stock can't be directly checked at this resolution

Decoding the *entire* `RAC.STATUS` register (not just the `STATE` field
this whole investigation has read so far) against its public fields
found something not previously looked at: at the standard checkpoint,
`TXENS` (bit 30, "TX Enable Status" -- the actual hardware TX-enable
circuitry's own status, distinct from the logical `STATE` field) reads
**`0`**, even though `STATE=9` claims TX is active. (`SEQSLEEPING`,
bit 22, also reads `1` -- consistent with, not contradicting, §7.15's
already-established finding that the M0+ core finishes its handler and
sleeps almost immediately.)

**Could not verify this against stock directly.** Stock's own TX
completes and advances to `RXPD` (state 4) faster than this project's
`bg22_require_sram_execution`-based catch-and-rehalt technique can
resolve -- tried with zero added sleep (just the technique's own ~10ms
minimum overhead) and stock was *already* at `RXPD` every time, three
runs in a row. For a tiny legacy-advertisement payload at 1 Mbps, the
entire PHY-level transmission plausibly completes in under a
millisecond, well below this tool's resolution floor. So whether
`TXENS=1` momentarily on stock during its own (much shorter) hold in
`STATE=9` is unconfirmed, not ruled in or out -- this finding stands on
its own reasoning (a TX-enable *status* bit reading 0 during a state
nominally called "TX" is intrinsically suspicious), not on a
stock comparison.

**Tested the direct, named hypothesis anyway: `RAC.CMD.PAENSET`**
(bit 12, a manual PA-enable override, same write-only command register
as `TXEN`). Added immediately after `TXEN`. QEMU 77/17 passed.
Hardware-tested: **no change** -- `TXENS` still reads `0`,
`RAC.STATUS`/`FRC.STATUS` otherwise identical to every prior reading.
Reverted; not kept.

This leaves `TXENS=0` as a genuinely open, unresolved observation --
possibly a real divergence (the hardware TX-enable circuit never
actually latches on, which would explain everything downstream
failing to engage), possibly a normal transient this project simply
cannot catch at the right moment to compare. A trustworthy way to
observe stock's own `TXENS` value *during* its brief hold in `STATE=9`
(not just after it has already moved on) would resolve this -- the
M0+ live-state tooling question closed in §7.15 was about the M0+ CPU
specifically, not about catching the M33-visible `RAC.STATUS` register
at finer time resolution, so that specific gap is not actually closed
by this project's existing tooling.

### 7.22 The M0+ sequencer's own direct TXWARM/TX dispatch path writes no `FRC.CMD` bit at all; a fresh look at two never-analyzed stock traces; `RAC.SR0` bit 19 tested, no change

Revisited the hypothesis that the real `FRC` activation trigger lives
inside the M0+ sequencer's own job-dispatch logic (`fn_0x894`/`fn_0xd38`),
which this project's charter deliberately does not port. Checked it
directly against the decompiled code rather than leaving it as a guess:
`fn_0x894`'s `FRC.CMD.FRAMEDETRESUME` write is gated behind `irq_number
== 5` (not `8`/`9`); `fn_0x19d0`'s `FRC.CMD.TXSUBFRAMERESUME` write (the
same bit already tested and ruled out from the M33 side in §7.20) lives
in a function reached only via `IRQ13`'s own vector, never called from
`fn_0x894`/`fn_0xd38` at all. **For the plain `TXWARM`/`TX` path this
project's open stack actually exercises, the M0+ sequencer's own direct
dispatch code (`fn_0x894` -> `fn_0xd38`, the only two functions the
vector table reaches for `IRQ8`/`IRQ9`) does not write `FRC.CMD` at
all.** This specific hypothesis does not survive checking against the
real code -- both stacks' dispatch logic is a no-op for `FRC` here, so
the missing trigger is not hiding in the excluded M0+ policy logic.

**Found and used two never-analyzed stock traces.** `stage2-m33-prepare.json`
and `stage2-m33-tx-prepare.json` (pre-existing capture files, never
referenced anywhere in this document before now) single-step a stock
M33 TX-preparation window and record every store. Most of what they
contain overlaps material already tried: `RAC.SR3` bits 14/30 (§7.5.1,
already fixed), `SYNTH.FREQ`/`RAC.DIGCLKRETIMECTRL`/`RAC.XORETIMECTRL`
(§4.2's "channel-37 calibration word" injection, already tested and
confirmed negative). One item was genuinely new: a clear of `RAC.SR0`
bit 19 via its CLR alias, never previously isolated or tested on its
own (this project's own SR0 usage only ever touches bits 0-3/5-7/18).
**Tested it directly: no change on real hardware.** Reverted.

The traces also surfaced two still-unidentified write sites at
`0xA8016000+0x10C` and `0xA8015000+0x10C`/`+0xEC` (neither matches any
offset this project's own `OBS_BLOCK_A8015020`/`OBS_BLOCK_A8016100`
constants already use, nor any peripheral in the fetched public
register models -- `0xA8015000`/`0xA8016000` are not `AGC`/`RFCRC`/
`MODEM`/`SYNTH`/`RAC`/`FRC`'s base addresses).

### 7.23 The four new write sites from §7.22's traces, tested exactly as captured: no change

Replayed all four stock-captured values from `stage2-m33-tx-prepare.json`
(`0xB801610C=0x000FFFFF`, `0xB801510C=0x00025CEB`, `0xB80160EC=0x0FFFF8FF`,
`0xB80150EC=0x0C2F2891`) in the exact order the trace shows them,
immediately before `RAC.CMD.TXEN` -- the same relative position stock's
own M33 TX-prepare code writes them. QEMU 77 still passed. Hardware-tested
at the standard checkpoint: **no change** -- `RAC.STATUS`/`FRC.STATUS`
identical to every prior reading. Reverted; not kept.

This does not prove these four registers are irrelevant in general --
only that replaying them with this project's own minimal, already-correct
setup produces no observable difference, consistent with every other
static-value-injection result this project has found (the §4.2 "channel-37
calibration word" test reached the same conclusion for a different set of
registers). Combined with §7.22's structural finding (the M0+ sequencer's
own dispatch path writes no `FRC.CMD` bit for a plain TX either), the
weight of evidence now points away from "one more missing static register
value or command bit" and toward either a precise write-*ordering*/timing
dependency this project has not yet isolated, or a hardware-autonomous
precondition neither stack's software explicitly asserts.

### 7.24 The first concrete ordering test: `RAC.DIGCLKRETIMECTRL`/`XORETIMECTRL` positioned exactly as stock writes them -- still no change

Pursued the ordering/timing hypothesis §7.23 pointed toward with the
first genuinely testable case: `tests/hardware-lockstep/stage2-m33-tx-prepare.json`
shows stock writing `RAC.XORETIMECTRL` then `RAC.DIGCLKRETIMECTRL`
in a specific position -- between `SYNTH.FREQ` and `FRC.WCNTCMP2` --
but this project's own `radio_freq_chan_set()` has never written either
register at all, in any position. The earlier §4.2 "channel-37
calibration word" test injected these same two values, but there is no
record that it matched this exact position in the channel-set sequence,
so this is a real, not-yet-tried case, not a re-run.

Added both writes to `radio_freq_chan_set()`, positioned exactly between
`FREQ` and `WCNTCMP2` to match the trace precisely. QEMU 77/17 passed.
Hardware-tested at the standard checkpoint: **no change** --
`RAC.STATUS`/`FRC.STATUS` identical to every prior reading. Directly
confirmed the writes actually landed (`RAC.XORETIMECTRL` read back
`0x770`, `RAC.DIGCLKRETIMECTRL` read back `0x430`, both exactly as
written) before accepting this as a real negative, not a silent
failure. Reverted; not kept.

This specific ordering test came back negative, but it does not close
the broader ordering/timing hypothesis -- it only rules out *this one*
relative position. The rest of stock's tx-prepare sequence (§7.22's
four still-unidentified writes, already tested at this same relative
position in §7.23 and also negative) has now been tried in-place too;
what remains untried on the ordering axis is the *timing* dimension
specifically (how soon after channel setup `TXEN` follows, not just
which writes precede which), which this project's current call
structure (`radio_freq_chan_set()` and `radio_tx_enable()` as two
independent public functions, called from different points in the LLL)
has not been checked against stock's single-interrupt atomicity at all.

### 7.25 A major architectural finding: the first TX's own launch path goes through a full kernel-thread wakeup, measured at ~94us, that stock's hardware-timed path does not have

Traced the *first* TX's actual call path all the way from the LLL's
advertising-event `prepare` callback to confirm the timing hypothesis
§7.24 pointed toward, rather than continuing to guess at individual
registers. The result is a genuinely different architectural finding
from everything else in this document so far.

**The mechanism, confirmed by reading the code, not inferred.**
`lll_adv.c`'s `prepare` callback (reached once per advertising event)
calls `radio_reset()`, `radio_phy_set()`, `chan_prepare()` (channel 37
setup via `radio_freq_chan_set()`), then `radio_tmr_start(1, ticks_at_start,
remainder)` -- **not** `radio_tx_enable()` directly. `radio_tmr_start()`
is `efr32_radio_launch_schedule()` (`radio.c`): it arms a **Zephyr kernel
scheduler tick/clock-event compare** (`cntr_radio_cmp_set()`, documented
in `cntr.c`'s own header comment as "Tick counter of the ticker... on
the kernel's scheduler clock service", `32.768 kHz`, and explicitly
marked `UNVALIDATED ON HARDWARE`) -- **not `PROTIMER`**, the
radio-specific hardware timer this project's own code marks unimplemented
in thirteen separate places (`EFR32_RADIO_AIR_TODO("PROTIMER (timebase,
compare, captures) and RTC syn")`, etc.). When that generic tick compare
fires, its ISR calls `efr32_radio_launch_wake()` ("ISR-safe: publish only
a generation and wake the pre-created job"), which posts a deadline event
and wakes a **dedicated kernel thread** (`efr32_radio_tx_job_thread`,
`K_HIGHEST_THREAD_PRIO`). Only once the Zephyr scheduler actually context-
switches into that thread does it call `radio_tx_enable()`, which finally
issues the real `RAC.CMD.TXEN` write.

So for the very first TX of an advertising event, the real hardware
`TXEN` write is separated from "channel setup completed" by: an ISR
entry, a kernel scheduler wakeup/post, a thread context switch, and
only then the function call chain into `radio_tx_enable()`'s own
earlier setup (`SR3` clear, BUFC publish, `SR0` prepare, etc.) before
`TXEN` itself. Stock's own equivalent is virtually certain to be driven
by `PROTIMER`'s hardware compare/capture system, synchronized to the
same timebase the rest of the radio's digital logic runs on -- a
fundamentally different, hardware-paced mechanism, not a generic OS
timer tick with software ISR/thread latency layered on top.

**Measured, not guessed, how much latency this actually costs.** Added
two diagnostic cycle-counter timestamps (`k_cycle_get_32()`, reverted
after measuring, not kept): one at `efr32_radio_launch_wake()`'s TX
branch (the moment the tick compare ISR decides to wake the thread),
one at `radio_tx_enable()`'s own entry (the moment the thread actually
starts running). QEMU 77 passed throughout. On real hardware, read
back directly: a consistent **3141-cycle gap, `93.6us`** at this
board's `32.768 MHz` (`CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=33554432`)
core clock -- the actual, measured kernel-thread-wakeup latency between
"scheduled launch decided" and "`radio_tx_enable()` running", for the
very first TX of an event specifically. (Subsequent channels within the
same event, `lll_adv.c`'s continuation path, call `radio_tx_enable()`
directly from ISR context with no thread wakeup at all -- this specific
latency is unique to the very first TX, the one this whole document's
`ACTIVETXFCD` checkpoint has been testing against.)

**What this does and does not establish.** It is a confirmed,
measured, structurally real difference between this project's TX-launch
path and what stock's `PROTIMER`-driven path almost certainly looks
like -- not a guess, and not yet disproven the way the sixteen register-
level hypotheses in §7.14-§7.24 were. `93.6us` is well within RAIL's
documented general TX-scheduling margins, so this does *not* by itself
prove the delay is disqualifying -- but if `FRC`'s frame-coder-activation
logic depends on a much tighter hardware-timed window relative to
channel/`BUFC` setup completing (plausible for a tightly-coupled digital
block, not yet confirmed either way), a `~94us` software-scheduled gap
could easily blow through it. This is the first hypothesis in the whole
investigation that is architectural rather than register-level, and the
first that would require a real engineering effort (actual `PROTIMER`
integration, not a register poke) to test conclusively -- recorded here
in full rather than attempted as a hasty, risky change to a scheduling
mechanism that other code (RX launch, general event timing) also
depends on.

### 7.26 A cheap, bounded test of §7.25's latency: crashed with a `UsageFault`, not a clean answer either way

Before accepting that testing §7.25 requires full `PROTIMER` integration,
tried one cheap, reversible experiment first: made
`efr32_radio_launch_wake()`'s TX branch call `radio_tx_enable()` directly
from the tick-compare ISR (the same `atomic_cas` pattern already used in
`efr32_radio_tx_job()`, just invoked from the ISR instead of waking the
thread) -- matching the zero-added-latency path `lll_adv.c`'s own
continuation channels already use, with no new magic numbers and nothing
obviously ISR-unsafe in `radio_tx_enable()`'s own body (checked first: no
mutexes, semaphores, or sleeps). This does not implement real `PROTIMER`
timing; it only removes the §7.25 software delay, as the cheapest
possible test of whether *that specific latency* matters.

QEMU 77/17 passed. **On real hardware, this crashed with a `UsageFault`**
(caught directly: `bg22_require_sram_execution` reported `current mode:
Handler UsageFault` immediately after resuming) -- not a clean "no
change," and not evidence either way about `ACTIVETXFCD`. Reverted
immediately; confirmed the hardware recovered fully (normal `Thread`-mode
halts, `RAC.STATUS=0x09400000` exactly as the established baseline) once
back on the unmodified build.

**This is itself worth recording, not just discarding as a failed
experiment.** Calling `radio_tx_enable()`'s full call chain directly from
this specific ISR is unsafe as currently written -- plausibly a stack-depth
issue (interrupt stacks are typically much smaller than thread stacks, and
`radio_tx_enable()`'s chain through `efr32_tx_fifo_publish()`/
`efr32_packet_stage_arm()`/etc. was never verified against an ISR-sized
stack), though the exact cause was not root-caused here. **Any future
attempt to reduce or remove this latency (including real `PROTIMER`
integration) needs to account for this: the fix cannot simply move the
existing `radio_tx_enable()` call earlier in the same call chain without
also addressing whatever caused this fault.** §7.25's architectural
finding stands; this experiment answers "how would you cheaply test it"
with "you can't, this way" rather than resolving the hypothesis itself.

### 7.27 §7.26's `UsageFault` root-caused: confirmed `STKOF` (stack overflow), not a register/timing clue

Root-caused §7.26's crash rather than leaving it as "plausibly a
stack-depth issue." Re-applied the exact same ISR-bypass edit, then
caught the fault with a hardware breakpoint (`bp 0x341c 2 hw` on
`z_arm_fault`'s entry, armed before `bg22_reset_to_breakpoint`'s
`SYSRESETREQ` pulse -- the FPB comparator survives the reset and fires
the instant Non-secure code reaches it, per the reliable-breakpoint
mechanism already documented above `bg22_reset_to_breakpoint`) --
catching the fault *before* Zephyr's own handler logs and clears the
sticky `CFSR` bits (`vendor/zephyr/arch/arm/core/cortex_m/fault.c`'s
`usage_fault()` etc. deliberately write back `SCB->CFSR |=
..._Msk` after decoding each fault reason, which is why a post-hoc read
after the system settled into `arch_system_halt()`'s spin loop saw
`CFSR=0`, `HFSR=0` -- those were already consumed by the time anyone
could inspect them that way, not absent).

At the breakpoint (`z_arm_fault(uint32_t msp, uint32_t psp, uint32_t
exc_return, _callee_saved_t *callee_regs)`'s entry, before its
prologue executes, so `r0`-`r3` still hold its arguments exactly as
passed): `CFSR=0x100000`. Bit 20 of `CFSR` is `UFSR` bit 4
(`UFSR` occupies `CFSR[31:16]`), which is **`STKOF`: stack overflow
detected during exception stacking.** Reading the would-be exception
stack frame at `r0` (the faulting `MSP`) returned all zeroes --
consistent with, not contradicting, `STKOF`: the ARMv8-M architecture
specifically skips writing the stack frame when stacking itself would
overflow, to avoid compounding the corruption, so an unwritten frame is
the expected signature, not a separate anomaly.

**Conclusion:** calling `radio_tx_enable()`'s full call chain directly
from `efr32_radio_launch_wake()`'s tick-compare ISR overflows the
interrupt/`MSP` stack. That call chain normally only ever runs on
`efr32_radio_tx_job_thread`'s own (larger) stack; the ISR stack was
never sized for it. This fully explains §7.26's crash as a pure
stack-depth artifact of *that specific bypass*, not a hardware-timing
response or a new clue about `ACTIVETXFCD` -- §7.25's architectural
finding (the ~94us kernel-thread-wakeup latency) stands exactly as
recorded, neither strengthened nor weakened by this. A real test of
§7.25 still requires either growing the interrupt stack specifically
for this path (a real but orthogonal fix, not attempted here, since it
changes memory layout project-wide for a diagnostic) or actual
`PROTIMER` integration, as already concluded in §7.25.

Reverted the ISR-bypass edit (`git diff --stat` confirmed empty);
rebuilt the clean baseline; QEMU 77/17 re-confirmed passing.

### 7.28 A real QEMU fidelity divergence, found and fixed: `RAC.STATUS.TXENS`/`SEQACTIVE` during `TX`

Separately from the `ACTIVETXFCD` search itself, checked whether
`vendor/qemu`'s device model (`hw/arm/efr32bg22.c`) actually reproduces
what this whole investigation has repeatedly, directly measured on real
hardware -- per the standing dual-goal mandate's second half ("every
divergence you find must be recorded and fixed"), this is exactly the
kind of check that shouldn't wait for someone to notice it by accident.

`efr32_rac_status()` previously derived `SEQACTIVE` (bit 28) as `state
!= OFF` and `TXENS` (bit 30) as `state == TX` -- the naive assumption
that these status-mirror bits simply track the coarse `STATE` field.
This directly contradicts the baseline this entire document has
measured over and over (`RAC.STATUS == 0x09400000` after the real
`RAC.CMD.TXEN` sequence settles, `STATE=9/TX` with `SEQACTIVE=0`,
`TXENS=0`) -- not a one-off, the single most-repeated measurement in
this whole investigation. QEMU's model and its own `test_rac_tx_commands`
qtest both asserted the opposite (`TXENS=1` on `TXEN`), meaning the
simulator was *not* faithfully reproducing real silicon's behavior for
this exact, heavily-tested scenario.

**Fixed**, scoped to exactly what's confirmed: `TXENS` no longer has
any state that sets it (there is no confirmed state where real hardware
shows it as 1, so -- same precedent as `ACTIVETXFCD` just above it in
the same file -- it is left unset rather than guessing a new trigger).
`SEQACTIVE` is now `0` specifically for `state == TX` (confirmed), left
as the prior `state != OFF` formula for every other state (unconfirmed,
not disproven -- no equivalent real-hardware RX measurement exists, so
changing it would just swap one guess for another). `SEQSLEEPING` (bit
22) stays entirely unmodeled, for the same "don't synthesize an
unconfirmed trigger" reason. Updated `test_rac_tx_commands` to assert
the hardware-confirmed `TXENS=0`/`RXENS=0` during `TX` instead of the
previous (disproven) `TXENS=1` expectation. QEMU 77/17 re-confirmed
passing with the fix.

This does not resolve `ACTIVETXFCD` itself -- the open stack's real
hardware and the now-corrected QEMU model agree with each other on this
point, both showing the same "stuck" status word. But it closes a real,
confirmed instance of the simulator previously disagreeing with silicon
on a register this investigation depends on reading correctly in every
other test on this chip.

### 7.29 A second real QEMU fidelity divergence, found and fixed: `FRC.WCNTCMP0`'s dynamic length

**Superseded in part by §7.38:** `WCNTCMP0` is `field + MINLENGTH`, not the formula here.

`vendor/qemu` had no special-case handling for `FRC.WCNTCMP0` at all --
it just held whatever software last wrote (the open stack's bootstrap
sentinel `0xFF`), never updating it, even though §7.20 already
established that real FRC genuinely parses the packet's own length
byte (`WCNTCMP0` reads `0xA` for an 11-byte frame). Got a second,
independent confirmed data point before implementing anything (per
[[feedback-rigor-and-methodology]] and the `ACTIVETXFCD`/`SEQSLEEPING`
precedent of never synthesizing from one data point): temporarily grew
`tests/radio-port`'s advertising payload from 1 byte to 4 bytes (a
reversible, deliberately small diagnostic change, reverted immediately
after measuring) and re-measured on real hardware -- `WCNTCMP0` moved
from `0xA` to `0xD`, exactly `+3` for `+3` payload bytes. This confirms
a clean 1:1 linear relationship, not just one coincidental number.

Both measurements fit exactly: `WCNTCMP0 == DFLOFFSET + <the
DFLCTRL-configured length field, read from the real staged packet
bytes> + 3` (BLE's fixed 3-byte CRC, added because this project's one
and only `DFLCTRL` configuration has `DFLINCLUDECRC=0`). `DFLOFFSET=1`
plugged in: `1 + 6 + 3 = 10` and `1 + 9 + 3 = 13`, both exact.

**Implemented in `efr32_rac_capture_tx_snapshot()`** (which already had
the real staged packet bytes in hand for the RF-medium snapshot
mechanism) -- a new `efr32_frc_dynamic_length_wcntcmp0()` helper reads
the real, currently-configured `DFLCTRL` fields (`DFLMODE`, `DFLOFFSET`,
`DFLBITS`, `DFLSHIFT`, `DFLBITORDER`, `DFLINCLUDECRC`) generically
rather than hardcoding this one configuration, extracts the length
field from the actual packet bytes, and only updates `WCNTCMP0` when
`DFLMODE==1` -- the only value this project's firmware ever configures,
and the only one with any real-hardware confirmation. Other `DFLMODE`
values have no public description text in the generated field model
and are deliberately left alone (matching the `ACTIVETXFCD` precedent:
don't synthesize what isn't confirmed). Added
`test_frc_wcntcmp0_dynamic_length`, encoding both real measurements
directly. QEMU 78/17 (one new test) confirmed passing.

### 7.30 Single-stepping over the real `TXEN` write: independent reconfirmation, one new fact, no new lead

Checked whether `FRC.STATUS`'s `FRAMEOK`(9)/`FSMSTATE=17`(20-24) bits
pre-exist before `TXEN` or appear only afterward (if they pre-existed,
they'd just be stale init state, not part of this stall). Broke
*inside* `radio_tx_enable()` right at its own `TXEN` store
(`0x16602` in the current build), read `FRC.STATUS` there: `0x0`
(neither bit set) -- confirms they're a direct effect of the `TXEN`
sequence, not leftover state. `RAC.STATUS` at that same pre-`TXEN`
point: `0x10400000` (`STATE=OFF`, `SEQSLEEPING` already `1`) -- a new,
minor fact: `SEQSLEEPING` is already set even while fully `OFF`, before
any `TXEN` is issued, so it's not something that only becomes true once
stuck in `TX`.

Then single-stepped *one instruction* past the `TXEN` store and
immediately read both registers (debugger still halted, no `sleep`):
already `FRC.STATUS=0x01100200`, `RAC.STATUS=0x09400000` -- byte-for-
byte identical to the fully-settled value measured 450ms later. This
independently reconfirms, via a completely different technique than
§7.20's `sleep 0/1/2/5`ms sweep, that the whole transition to the stuck
state completes faster than any halt-based observation can catch mid-
flight. Consistent with (not contradicting) the earlier, already-
documented finding above this section that single-stepping "froze" the
observed value while repeatedly polled *during* a halt -- here the full
transition had already completed by the time even one single-step's
worth of real wall-clock halt-latency had passed, so there was no
"before" moment left to catch. **This closes off "try to catch it mid-
transition via single-stepping" as a method, on both sides of this
investigation** (already established for the open stack above; now
confirmed the same physical limit applies here too) -- not a new lead,
a method ruled out with a cleaner explanation than before.

### 7.31 A new, concrete, data-plane fact: `BUFC`'s TX `READOFFSET` never advances past the 2-byte header

Every prior check of this stall looked at status/control bits
(`FRC.STATUS`, `RAC.STATUS`) -- never at whether `FRC` actually *reads*
any of the staged packet bytes out of `BUFC`'s TX FIFO. Checked this
directly at the settled baseline: `BUFC`'s TX channel `WRITEOFFSET`
(real address `0xba000014`, `EFR32_BUFC_TX_BASE_OFFSET` + `WRITEOFFSET`
sub-offset) reads `0xB` (11, the full staged frame -- software wrote
all of it, as expected), but `READOFFSET` (`0xba000018`) reads `0x2` --
**`FRC` consumed exactly the 2-byte header (matching `WCNTCMP0`'s
confirmed length-byte parse) and never advanced to read a single
payload byte.** This is new: a hard, byte-level, data-plane
confirmation of where the stall actually sits, not an inference from a
status flag. It's consistent with every status-bit finding so far
(`ACTIVETXFCD=0`, `FSMSTATE=17` stuck, `WCNTCMP0` correctly computed)
but is a stronger, more direct form of evidence than any of them.

**Tried a faster iteration technique on the strength of this: live
`FRC.CMD` pokes via the debugger (`mww` while halted, `resume`, a short
`sleep`, re-halt, re-check) instead of a full edit-rebuild-reflash cycle
per hypothesis**, using `READOFFSET` advancing past `2` as a more
sensitive success criterion than `ACTIVETXFCD` alone (a bit could
plausibly nudge a byte or two through without the whole "active frame
coder" status bit ever lighting up). Tried `FRAMEDETRESUME` and
`TXSUBFRAMERESUME` (the latter already compile-time tested in §7.20
against `FRC.STATUS` only, not `READOFFSET`) this way: both clean
negatives, `READOFFSET` stays at `0x2` either way. **Tried `BLOCKINIT`
this way too and it caused a real `HardFault`/CPU lockup** (OpenOCD's
"clearing lockup after double fault" auto-recovery) -- not a useful
signal, and risky: **do not live-poke `FRC.CMD.BLOCKINIT` on this chip
again** outside of a full compiled, reviewed, revertible change.
Recovered with a full fresh reflash; hardware reconfirmed back to the
exact same baseline (`RAC.STATUS=0x09400000`, `READOFFSET=0x2`)
afterward, so no lasting damage, but the live-poke technique itself
carries real risk for some command bits and should be used cautiously.

**This narrows the open question usefully, even without resolving it:**
the real blocker is specifically "why does `FRC` never issue a second
`BUFC` read" after the header -- not a vague "frame coder doesn't
start" -- which is a sharper, more falsifiable question for whatever
comes next (including evaluating a real `PROTIMER` integration attempt
against this exact, now very concrete, success criterion: does
`READOFFSET` ever pass `2`).

### 7.32 `PROTIMER` is confirmed, with real captured data, to be completely disabled on the open stack -- and a cheap live test of enabling it crashed

**Superseded in part by §7.39:** `PROTIMER.EN` reads 1: enabled, never started.

Found `tests/hardware-lockstep/` (a differential hardware/QEMU debugger
with `compare.py`, plus already-captured register-bank snapshots for
both stacks, built earlier in this same project but not yet mined for
this specific question). Diffed its pre-existing
`stage{2,3}-pre-txen-protimer.bin` captures directly (`cmp`/`od`, no
new hardware session needed for this part): stock's capture shows a
visibly *running* counter (changing, non-zero words at the offsets a
free-running counter would occupy) and a nonzero word at `+0x8`
(`0x00011102`); the open stack's capture is all-zero at every one of
those same offsets. **This is real, captured, measured proof --
not inference from "13 places marked unimplemented" -- that
`PROTIMER` is completely unclocked/disabled on the open stack, and
already fully running on stock before `TXEN` is ever issued**, i.e.
independent of any specific TX event, consistent with §7.25's
`cntr.c`-not-`PROTIMER` finding but now backed by a direct hardware
comparison rather than static code reading.

Live-reread this on fresh hardware to confirm it still holds (it does:
`PROTIMER_NS_BASE+0x8` reads `0x0`, `+0x20` -- the same offset
`vendor/qemu`'s own model already guesses is the counter, and matches
where stock's capture showed a changing value -- also reads `0x0`,
confirming the counter is frozen). **Tried the cheapest possible test
of this: live-poked `PROTIMER_NS_BASE+0x8` to stock's exact captured
value (`0x00011102`) via the debugger** (not a compiled change -- same
fast-iteration technique as §7.31). The write landed (read back
exactly as written), but **it caused a real crash during the
following `resume` window** (halted generically afterward, `addr2line`
resolving the PC to `uart_silabs_usart.c:158` -- plausibly Zephyr's own
fatal-handler crash dump trying to print over UART, not necessarily
the real original fault site; not root-caused with the same rigor as
§7.27 since this was a side probe, not a blocking investigation). The
counter at `+0x20` still read `0` afterward (the poke did not start it
running), and `READOFFSET`/`FRC.STATUS`/`RAC.STATUS` were all
unchanged -- no effect on the actual stall either way. Recovered via a
full reflash; hardware reconfirmed back to the exact same baseline.

**Do not live-poke `PROTIMER`'s registers again outside a reviewed,
compiled change with proper IRQ handling in place.** The crash is
itself informative: enabling `PROTIMER` isn't a harmless no-op even if
nothing currently uses it -- it most plausibly starts firing an
interrupt this firmware's vector table has nothing registered for,
which is exactly the kind of prerequisite real `PROTIMER` integration
(§7.25) would need to handle properly as part of that larger effort,
not bypass with a bare register poke. This doesn't move `ACTIVETXFCD`
forward directly, but it upgrades "`PROTIMER` integration is unproven
but promising" to "`PROTIMER` is confirmed completely off, confirmed
non-trivial to even just turn on safely" -- real scoping information
for whoever attempts that integration next.

### 7.33 A proper, safe `PROTIMER` enable test, built as a kept standalone probe: clean negative, narrows the hypothesis

Built `tests/protimer-probe/` (`probe.S` + `build.sh` + `README.md`,
kept as a permanent, reusable artifact per the user's explicit request,
not reverted the way every other hardware-tested change this session
has been) -- standalone Cortex-M33 assembly, independent of Zephyr and
the real driver, that does only the `PROTIMER` enable sequence
(`EN`/`CTRL`/two further config writes/`CMD=START`, matching stock's
own observed init sequence) with NVIC's IRQ36 deliberately left masked
-- no vector-table risk, unlike §7.32's crash. Injected via the
debugger into SRAM and run from a halt-point right before the real
firmware's own `TXEN` write, then handed back to the real firmware to
continue -- this reuses the real, already-correct `FRC`/`BUFC`/`RAC`
bring-up instead of hand-duplicating the ~30-write PHY config table in
a from-scratch image (real risk of exactly the kind of transcription
error §7.13 already caught once).

**Two real debugging pitfalls hit and fixed while building this (both
documented in the probe's own `README.md` so they aren't rediscovered):**
the probe's writes silently had no effect the first time, because it
used the *secure* `0xa801c000` `PROTIMER` window -- the probe inherits
the host firmware's Non-secure CPU state, same as every other radio
register access in this project, and needs the `0xb801c000` alias, not
the secure one, even though reads/writes to the wrong window neither
faulted nor erred. Separately, the injected code took an immediate
`MemManage` fault the first time it was run after
`bg22_reset_to_breakpoint`: Zephyr's MPU marks SRAM execute-never, and
disabling it (`mww 0xe000ed94 0`) *before* that proc's real
`SYSRESETREQ` does not persist across the reset -- it must be disabled
again afterward, immediately before injecting/running anything.

**With both fixed, the probe ran cleanly (confirmed via its own
completion marker, no fault) and genuinely started `PROTIMER`'s
counter** (`0xb980` immediately after the probe, `0x1031a2` ~450ms
later after the real firmware's `TXEN` ran to its usual hold -- a real,
live, incrementing counter, not a stuck write). **Clean negative
result: `WRITEOFFSET`/`READOFFSET`/`WCNTCMP0`/`FRC.STATUS`/`RAC.STATUS`
all read byte-for-byte identical to the established baseline** (`0xB`/
`0x2`/`0xA`/`0x01100200`/`0x09400000`) with `PROTIMER` running the
entire time. Merely having `PROTIMER` powered and counting is **not**
sufficient to unstick `FRC`'s frame coder.

**What this does and doesn't rule out.** It cleanly eliminates the
simplest version of the hypothesis ("`FRC` just needs `PROTIMER`'s
clock/power to be present somewhere in the system"). It does **not**
rule out the IRQ-handshake version (does `FRC` need to actually receive
a *serviced* `PROTIMER` interrupt, or a specific compare/capture event,
not just a running counter) -- testing that needs a real installed
IRQ36 handler, which this probe deliberately avoided (§7.32's crash was
most plausibly exactly that unhandled-interrupt case). That remains the
next concrete, well-scoped experiment if this is picked up again: either
a from-scratch bootable image with a real vector table (like
`tests/hardware-lockstep/m33-seq-txwarm.S`), or Zephyr's own
`IRQ_CONNECT()`/software-ISR-table mechanism to install a real (even if
trivial) handler before unmasking IRQ36 in this same injection
technique. Hardware reconfirmed back to the exact baseline afterward.

### 7.34 `PROTIMER.IEN` all-ones: it genuinely reacts (`IF` pends, partially acks), but neither pending-unserviced nor partially-acked changes anything

Checked first whether `PROTIMER` even *tries* to request an interrupt
at all: at the unmodified baseline, `PROTIMER.IEN` (`+0x070`) reads `0`
and `NVIC`'s pending register (`ISPR1`, `0xE000E204`) shows nothing for
IRQ36 -- consistent with everything else established, `PROTIMER` is
fully inert. This chip's `IF`/`IEN` bit meanings are unresolved
(§7.9), but "pending" can be checked via `NVIC` without unmasking the
interrupt (pending latches independently of enable), which is a safe
way to test "does anything happen" before escalating.

Built a second kept probe, `tests/protimer-probe/probe-ien-sweep.S`
(same base enable sequence as `probe.S`, plus `PROTIMER.IEN = 0xFFFFFFFF`
-- a blanket sweep, explicitly not a field-decoded value, since the
named-bit meanings aren't known). With that in place: `ISPR1` bit 4
(IRQ36) sets, and `PROTIMER.IF` reads `0x3` after a real `TXEN`
sequence -- **`PROTIMER` genuinely reacts to the real TX attempt**, not
inert even with `NVIC` itself left masked (no vector-table risk, same
as `probe.S`). But with the interrupt still unserviced for the usual
450ms settle window, `READOFFSET`/`FRC.STATUS`/`RAC.STATUS` are
unchanged from baseline -- pending-but-unserviced doesn't help either.

Separately checked whether merely *acknowledging* the pending flag
(writing its `CLR` alias, `0xb801e064`, no vector dispatch needed) has
any effect on its own: `IF` went from `0x3` to `0x1` immediately after
writing all-ones to the alias -- one bit (bit 1) is a real, clearable
one-shot latch; the other (bit 0) re-asserts instantly, consistent with
a persistent status-style condition rather than a one-shot event. (A
follow-up attempt to then let the real firmware continue and recheck
hit a test-harness gotcha, not a hardware finding: a bare `resume`
with no address just continues from wherever the core is currently
halted, which after `bg22_require_sram_execution` is that fixture's
own tiny proof code, not the real firmware's genuine run point --
`bg22_require_sram_execution` is destructive to the real PC/SP by
design, so "ack now, then let the real firmware keep going and
recheck" needs a non-destructive halt for the ack step, not attempted
here. Caught before being reported as a real negative result.)

**Net position after §7.32-7.34: the simplest and the next-simplest
versions of the `PROTIMER` hypothesis are both now cleanly ruled out**
(clock-only; pending-but-unserviced). **What remains is specifically
whether the CPU must actually take the IRQ36 exception and run real
handler logic** -- genuinely testing that needs a real installed vector
(either a from-scratch bootable image with its own vector table, or a
`VTOR`-redirected SRAM-resident copy of the real table with just IRQ36
overridden) rather than another register poke. This is a larger,
riskier undertaking than anything tried so far in this sub-thread and
was not attempted this round.

### 7.35 The real-IRQ-servicing test attempt: confounded by the test apparatus itself, not a real finding either way

**Superseded by §7.36:** the abort's attribution here was wrong.

Attempted the experiment §7.32-7.34 identified as the remaining gap:
does `FRC` need the CPU to actually *take* the IRQ36 exception, not
just have it pending? Built `tests/protimer-probe/probe-irq-handler.S`
(kept, per the standing instruction to keep this whole probe family as
evidence): since this build has no `CONFIG_DYNAMIC_INTERRUPTS`
(`_sw_isr_table` is flash/const, confirmed via `nm`) and `VTOR_NS`
reads `0` (flash-resident, confirmed by having the probe itself read it
from the CPU's own Non-secure perspective rather than trusting a
debugger-side read, which risks hitting the wrong TrustZone bank), the
probe copies the live 64-word vector table into SRAM, patches only the
IRQ36 entry to a trivial ack-and-return handler, repoints `VTOR_NS` at
the copy, then does the same `PROTIMER` enable/`IEN`-sweep as §7.34 and
unmasks IRQ36.

Caught one real bug before testing on hardware: the first draft placed
the copied table at `0x20000800`, inside this *same probe's own
180-byte code* (`0x200007d0`-`0x884`, confirmed via `size`) -- the copy
loop would have overwritten its own not-yet-executed instructions,
including the handler, mid-run. Moved the table to `0x20000900`,
clear of the probe's own code.

**Result: with IRQ36 genuinely enabled and taken, the TX attempt
aborted entirely** -- `RAC.STATUS` returned to `0x10400000` (`OFF`,
matching the pre-`TXEN` baseline), `FRC.STATUS=0`, `WCNTCMP0=0xFF`
(never even parsed) -- reproduced identically on a second run.
Surprising enough that, per this project's own standing rule (a
surprising result should be isolated before being accepted), built an
isolation variant (`probe-irq-handler-no-enable.S`) that does
everything identically *except* never unmasking IRQ36 in `NVIC` --
confirmed via `ISER1` reading back with bit 4 clear, so the exception
was never taken. **It reproduced the exact same abort-to-`OFF`
result.** This proves the abort has nothing to do with taking the
interrupt: it is an artifact of the `VTOR` table-copy destination
itself. `0x20000900` falls inside live Bluetooth host-stack globals
(`acl_conns`/`db_hash`/`gatt_sc`, confirmed via `nm -S` -- this
firmware links at ~99.68% SRAM utilization, there is no genuinely free
256-byte region anywhere), and corrupting that state is plausibly what
causes the host stack to detect something wrong and abort the
advertising attempt, confounding the result completely.

**This is a negative methodological finding, not a resolved
hypothesis.** The IRQ-handshake sub-hypothesis (does `FRC` need a
*serviced* `PROTIMER` interrupt) remains genuinely untested, not
disproven -- this attempt to test it was invalidated by its own side
effects before it could say anything real. Testing it properly needs a
destination for the copied vector table (and the handler, if not
inlined in already-clear code) that doesn't live inside active
Bluetooth host state -- which this packed-RAM firmware doesn't have
anywhere nearby `0x200007d0`'s established scratch convention. The
earlier, lower-risk probes in this family (`probe.S`,
`probe-ien-sweep.S`) only ever touched `bt_workq` (clear in their
narrow pre-`TXEN` test window, evidenced by their results reproducing
the exact expected baseline both times) -- the jump specifically to
also touching `acl_conns`/`db_hash`/`gatt_sc` is what broke this one.
A real attempt at this test needs either a from-scratch bootable image
with its own vector table (sidestepping the real firmware's RAM
entirely, like `tests/hardware-lockstep/m33-seq-txwarm.S`) or finding
a genuinely unused RAM window in this specific firmware image (not
attempted here). Hardware reconfirmed back to the exact baseline
afterward, both times.

### 7.36 §7.35's attribution retracted: it's the `VTOR` redirect itself, not the destination -- still not root-caused

§7.35 attributed the abort to the copied vector table's SRAM
destination corrupting live Bluetooth GATT globals
(`acl_conns`/`db_hash`/`gatt_sc`). **That attribution was wrong, caught
by the same "verify before moving on" discipline that caught the
original false positive.** Moved the destination to `0x20007d00` --
256-byte-aligned (`VTOR`'s low bits are hardware-forced to 0 for
anything less, a bug in the first attempt's `0x20000900` too, though
it happened not to matter there) and entirely within HCI-command/ACL-TX
`net_buf` pools instead (`_net_buf_hci_cmd_pool`/`_net_buf_fragments`/
`net_buf_data_acl_tx_pool`/`_net_buf_acl_tx_pool`) -- territory this
connectionless, no-HCI-command smoke test should never touch. **The
exact same abort happened anyway**, including in the isolation variant
that never unmasks IRQ36.

Went further: built `probe-vtor-only.S`, which does *only* the 64-word
table copy and the `VTOR` write -- no `PROTIMER` touch, no `NVIC`
touch, nothing else at all. **Still aborts to `OFF`, already by just
+30ms after resuming** (confirmed via a shortened checkpoint, not the
usual 450ms). Tried to root-cause it with the same technique §7.27
used successfully (a hardware breakpoint on `z_arm_fault`'s entry,
armed before resuming, to catch a real fault before Zephyr's handler
clears `CFSR`): **the breakpoint was never hit within a 2-second
window.** This is itself informative -- whatever causes the abort is
most plausibly not a CPU fault at all, but some other software path
(a scheduling/timing decision elsewhere in the LLL or kernel) reacting
to the `VTOR` change, or a fault type/path this specific catch doesn't
cover. Not root-caused further; flagged honestly as unresolved rather
than guessed at.

**Net effect on the investigation:** redirecting `VTOR` on this live
firmware -- for any destination tried, regardless of `PROTIMER`
involvement -- reliably and near-immediately aborts the TX attempt via
an unknown mechanism that isn't a caught CPU fault. This closes off
"inject a real IRQ36 handler via a `VTOR`-redirected SRAM table into
the running production firmware" as a technique entirely, not just a
RAM-destination problem to route around. Testing the IRQ-handshake
sub-hypothesis (still genuinely untested) needs a from-scratch bootable
image with its own vector table instead -- there is no way to install a
real interrupt handler in-place on this firmware image via this
approach. All three `VTOR`-touching probes (`probe-irq-handler.S`,
`probe-irq-handler-no-enable.S`, `probe-vtor-only.S`) are kept, along
with this retraction, so the dead end and why it's a dead end are both
on record. Hardware reconfirmed back to the exact baseline afterward.

### 7.37 Starting a from-scratch probe the safe way: a real execution trace instead of hand-transcribing the PHY table

With live `VTOR`-redirect on the running firmware closed off (§7.36),
the only way left to test the IRQ-handshake sub-hypothesis is a
from-scratch bootable image with its own vector table. Hand-transcribing
the ~150-write PHY config table into assembly for that image would
reintroduce exactly the transcription-error risk this project has
already been burned by once (§7.13's `PROTIMER_0x38`/`FRC_0x1C` mixup).
Used `tests/hardware-lockstep/trace_stores.py` (already built earlier
in this project for M0+ IRQ8/IRQ9 traces) instead: caught the real M33
at `radio_phy_set()`'s entry via the usual `bg22_reset_to_breakpoint`,
then single-stepped it live, recording every MMIO store as JSON.

Got 159 real, directly-executed register writes this way -- the entire
base PHY config table plus `radio_setup()`'s own writes, byte-for-byte
authoritative because they're what the chip actually did, not
re-derived from reading source. (`--mask-interrupts` turned out unsafe
for a span this long: it stops `SysTick` from ever ticking, so a
`k_sleep`/timeout wait anywhere in the path spins forever once
interrupts are masked -- confirmed by resolving the resulting "stuck"
PC to `z_thread_halt` in `kernel/sched.c`, ordinary scheduler code, not
a crash.) Retrying without `--mask-interrupts` hit unrelated host-system
memory pressure (other long-running processes, not this project's own
tooling) that made further real-hardware single-stepping impractically
slow for this session; not a hardware or technique problem, just bad
timing. Saved the partial capture as
`tests/hardware-lockstep/radio-setup-trace-partial.json` (kept, per the
standing instruction for this probe family) rather than losing it --
it already covers the single largest, most error-prone part of what a
from-scratch image would need, and resuming the trace later just needs
`--current` from its exact stopping PC, without `--mask-interrupts`.

### 7.38 The trace reached `TXEN` -- and corrected a real bug in §7.29's own QEMU fix

Resumed §7.37's trace (removing `--mask-interrupts` fixed the hang; it
had been stuck in ordinary `z_thread_halt` scheduler code waiting on a
`SysTick` that masking prevented from ever ticking) and it ran all the
way to the real `RAC.CMD.TXEN` store this time -- `"complete": true,
"boundary": "stop-address"`. Found several genuinely new things on the
way (`RAC.CMD.RXDIS`/`TXDIS` issued as a reset-to-known-state step
before the real `TXEN`; `RAC.SR3`'s CLR-alias write, matching the
already-established "software only clears bits 14/30, never reads it"
finding; two still-unidentified `0xb8005xxx`/`0xb8006xxx` writes; two
SEQRAM-local writes, `0xB0003EC4`/`0xB0003EEC`, a likely M33-to-M0+
handoff), but the most important finding was in the real staged packet
bytes themselves.

The trace captured the actual 11 bytes written to `BUFC`'s
`WRITEDATA` port -- the real BLE `ADV_IND` PDU this project's own test
harness transmits. Byte 1 (the PDU's `Length` header field, per the BLE
spec) is `0x09` (9), not the `6` §7.29 assumed when it built
`test_frc_wcntcmp0_dynamic_length` and derived
`WCNTCMP0 == DFLOFFSET + field + 3`. **That formula was never actually
checked against a real captured header byte -- it was back-derived
from an assumed value that turned out to be wrong, and it only
"passed" because the qtest it shipped with used the same wrong
assumption.** With the real byte value (9) and the real measured
`WCNTCMP0` (`0xA` = 10), the true relationship is simply `field + 1`,
which equals `field + MINLENGTH` using `DFLCTRL`'s already-named
`MINLENGTH` field (value `1` in this project's only-ever-configured
`DFLCTRL`). The second §7.29 measurement (`WCNTCMP0 == 0xD` after
growing the test payload by 3 bytes) is consistent: the real Length
byte in that case is `12` (not `9` as previously labeled), and
`12 + 1 = 13 = 0xD` fits exactly.

**Fixed in `vendor/qemu`:** `efr32_frc_dynamic_length_wcntcmp0()` now
returns `field + minlength` instead of `dfloffset + field +
EFR32_BLE_CRC_LENGTH_BYTES`; the now-unused `DFLINCLUDECRC` extraction
and the `EFR32_BLE_CRC_LENGTH_BYTES` constant were removed rather than
left dangling. Updated `test_frc_wcntcmp0_dynamic_length` to use the
real captured header values (9 and 12) instead of the old assumed ones
(6 and 9) -- same expected `WCNTCMP0` results, now for the right
reason. QEMU 78/17 re-confirmed passing. This is exactly the kind of
divergence the project's own rigor standard exists to catch: a fix
that passed its own self-constructed test can still be wrong, if the
test's inputs were assumed rather than verified against real hardware.

### 7.39 Correction: "`PROTIMER` confirmed completely off" (§7.32) was imprecise -- `EN` was never actually checked

While assembling a from-scratch image from the two trace JSONs, reading
`radio_setup()` in full showed it unconditionally writes
`PROTIMER_BASE + 0x004 = 1` (the same `EN.EN` one-bit latch every other
radio block has) on every boot, alongside the other six blocks'
enables. §7.32's "confirmed completely off" language was checked
against real hardware, but only at offsets `+0x8` (the `CTRL`-like
register) and `+0x20` (the counter) -- **`+0x4` (`EN`) itself was never
actually read in this investigation.** Rechecked directly:
`PROTIMER_NS_BASE+0x4` reads `0x1` on real hardware, exactly matching
`radio_setup()`'s own source.

**This does not change any conclusion already reached** -- `probe.S`'s
own writes already included a (redundant, since it was already `1`)
`EN=1` before testing `CTRL`/the counter/`IEN`, so every §7.32-7.35
result stands. It only corrects the characterization: `PROTIMER`'s bus
clock is enabled from boot like every other radio block; what's never
configured is its `CTRL` register, timer-state inputs, and start
command -- i.e. it's enabled but never actually started, not
"completely off." Worth remembering for anyone building on this
investigation: check a register's own exact offset directly before
describing a whole block as "off," even when a nearby, related
register at a different offset genuinely is.

### 7.40 The IRQ-handshake sub-hypothesis, finally tested cleanly: ruled out

Built `tests/activetxfcd-probe/probe.S` -- a from-scratch, standalone
bootable M33 image, with its own real vector table from reset, sharing
nothing with Zephyr's RAM layout (sidestepping §7.35/§7.36's dead end
entirely, since there is no `VTOR` redirect on a live image here at
all). It replicates `radio_setup()`'s clock/EN bring-up,
`radio_reset()`'s exact sequencer install/release sequence (both read
directly from `radio.c`), then replays the 184 real captured MMIO
stores from §7.37/§7.38's two execution traces (`replay-table.inc`,
generated from the trace JSONs, not hand-transcribed), then adds
`PROTIMER`'s real enable sequence with a genuine installed `IRQ36`
vector this time, then issues the real `RAC.CMD.TXEN`. The real open
M0+ sequencer image is preloaded into SEQRAM via a separate debugger
`load_image` step, extracted directly from the already-running
firmware's own SEQRAM contents (confirmed correct: its first word is
exactly `EFR32_SEQ_STACK_TOP`).

**One real bug caught before it produced a false result:** the first
build replayed all 200 captured stores verbatim, including two
addresses in ARM's system-control space (`0xE000ED04` = `SCB.ICSR`
with `PENDSVSET` set, `0xE000E204` = NVIC's pending-clear) -- real
writes Zephyr's own scheduler made during the original trace, which
are meaningless and unsafe to replay on a bare-metal image with no
real scheduler. The CPU took a genuine `PendSV` exception straight
into the vector table's generic fault stub the instant that store
executed. Filtered both addresses out of `replay-table.inc` (184
entries remain) and rebuilt.

**With that fixed, the image ran correctly and the real `IRQ36`
genuinely fired and was serviced** -- caught live, mid-handler
(`current mode: Handler External Interrupt(36)`, `PC` at the handler's
own entry). It never returned to the main `spin` loop: `PROTIMER.IF`
bit 0 is sticky (matching §7.34's finding exactly -- a blanket ack
doesn't fully clear it), so the CPU spent the entire 450ms test window
continuously re-entering and re-servicing the real interrupt, not
idling. **`FRC.WCNTCMP0` reads `0xA`, proving the real `TXEN` write
executed and `FRC` genuinely parsed the real header exactly as on
production hardware** (this value is a hardware side effect, never a
value this image's replay table writes). Despite continuous, real,
serviced `IRQ36` activity for the entire window:
`READOFFSET` reads `0x2`, `WRITEOFFSET` reads `0xB`, `FRC.STATUS`
reads `0x01100200`, `RAC.STATUS` reads `0x09400000` -- **byte-for-byte
identical to the established stuck baseline.**

**This is a clean, definitive negative result, not confounded like
§7.35's attempt.** Taking and continuously servicing a real `PROTIMER`
interrupt -- not just having it clocked (§7.32-7.33), not just having
it pending (§7.34) -- does not unstick `FRC`'s TX datapath. All three
`PROTIMER` sub-hypotheses this investigation identified are now ruled
out with real evidence. The real trigger for `ACTIVETXFCD`/the
`READOFFSET`-stuck-at-the-header symptom (§7.31) remains unidentified;
`PROTIMER`, in every form tested, is not it. Hardware reconfirmed back
to the exact baseline afterward.

**What would still be worth trying, not yet attempted:** a real
(non-trivial) `IRQ36` handler doing what stock's shared `FRC_IRQHandler`
actually does (read/ack `FRC.IF`/`IEN` and `BUFC`'s pending/enable pair
first, per §7.9's disassembly findings, not just a blanket `PROTIMER`
ack) -- this test's handler was deliberately minimal and may not be
representative of what a real handshake would look like. That is a
new, different, well-motivated hypothesis, not a retest of this one.

**Checked cheaply, without a new hardware test, and it's already
closed too.** At the settled stuck baseline: `FRC.IF=0x40020c04`,
`FRC.IEN=0x11` (`TXDONE`/`RXDONE`, ordinary, unrelated to this stall);
`BUFC.IF=0x4`, `BUFC.IEN=0x0`. A stock-faithful handler computes
exactly `IF & IEN` before acking anything -- and that intersection is
provably `0` for both blocks (`FRC`'s pending bits, 2/10/11/17/30,
share nothing with its enabled bits 0/4; `BUFC` has nothing enabled at
all). **A more faithful handler could not behave any differently from
the blanket-ack one already tested, because there is nothing enabled
for it to find.** This means the real blocker is not something `FRC`
or `BUFC` are trying to signal via an interrupt at all, from any angle
tested in this whole `PROTIMER`/IRQ thread -- it's a silent internal
stall in `FRC`'s own datapath (`FSMSTATE=17`), with nothing visible to
any of the interrupt-enable/pending machinery checked so far.

### 7.42 A genuinely new lead: `SYNTH.STATUS.INLOCK=0` at the stuck baseline -- the PLL is not locked

**Superseded by §7.60:** the `INLOCK=0` comparison here was between mismatched RAC states.

With the entire interrupt-handling thread closed, checked two blocks
this investigation had never directly examined at the settled stuck
baseline: `MODEM` and `SYNTH`. `MODEM.STATUS`/`STATUS2`/`STATUS3`/
`FSMSTATUS`/`TIMDETSTATUS` read `0`/`0`/`0x3000`/`0`/`0x4000000` --
decoding their fields (`DEMODSTATE`, `FRAMEDETID`, `VITERBIDEMOD*`,
`DSASTATE`, `CHPWRACCUMUX`, `BBPFOUTABS`, `LRDSALIVE`) shows these are
all RX-demodulator/power-measurement diagnostics, unsurprising and
unpromising for a TX stall -- not pursued further.

`SYNTH.STATUS` (`0xb8018008`), checked next, reads `0x4`:
`SYNTHREADY=1` (bit 2) but **`INLOCK=0`** (bit 0) -- the frequency
synthesizer's PLL is not locked. `IFFREQEN`(bit1)=0,
`CAPCALRUNNING`/`CAPCALBITRESULT`/`CAPCALFULLWAIT`/`CAPCALERROR`
(bits 8/9/10/14)=0, `VCOFREQACTIVE`(bit16)=0 -- no calibration is
reported running or erroring, the VCO simply isn't shown active
either. This is the first genuinely new, concrete lead since the
entire `PROTIMER`/interrupt thread closed: if the PLL never actually
locked onto the target channel frequency, the radio cannot be
transmitting real RF at a stable frequency, which is a very plausible
reason for whatever gates `FRC` from ever pulling a second byte out of
`BUFC`. **Immediately checked against existing data before investigating
further, and refuted: this is not a divergence at all.**
`tests/hardware-lockstep/` already has stock (`stage2`) `SYNTH`
captures at comparable pre-`TXEN` checkpoints
(`stage2-pre-txen-synth.bin`, `stage2-first-synth.bin`) -- both read
the identical `0x4` (`INLOCK=0`) at this offset. The open stack's own
other captures (`stage3-ch37-pre-txen-synth.bin`,
`stage3-first-synth.bin`) match too. **`INLOCK=0` is normal/expected
behavior at this specific checkpoint on both stacks, not a fidelity
gap or a clue** -- whatever the bit actually means, it evidently
doesn't need to be `1` for a real TX to succeed on stock either. Ruled
out efficiently, without a new hardware session, by checking already-
available comparison data before investing further -- exactly the
discipline this project's own rigor standard asks for.

**While checking this, found a larger, not-yet-mined opportunity:**
`tests/hardware-lockstep/`'s `stage{2,3}-ch37-pre-txen-*.bin` captures
(deliberately channel-matched, avoiding §7.19's channel/timing-mismatch
confound) exist for all seven blocks (`agc`/`bufc`/`frc`/`modem`/`rac`/
`rfcrc`/`synth`), not just the ones this investigation has checked so
far. A systematic byte-level diff finds real differences in every
block (36/36/19/25/52/12/24 bytes respectively) -- but decoding 204
differing bytes against public field names with the same rigor §7.19's
`FRC` diff got (distinguishing real divergences from RAM-pointer/
build-specific/benign-counter noise) is substantial work, not
something to rush through. **Recorded as the next well-scoped task if
this is picked up again**, rather than attempted hastily here: start
with `agc`/`rfcrc` (smallest diffs) using the same field-decode
discipline as §7.19, and watch specifically for anything that isn't
explained by a RAM address, a channel-dependent config value, or a
live counter.

### 7.43 Started that diff with `rfcrc` -- a real production-code fix found, but not the `ACTIVETXFCD` cause

**Superseded in part by §7.60:** the "per-chip trim" and "never touched" closure of the RAC/MODEM/SYNTH diffs is withdrawn.

Decoded `rfcrc`'s 12 differing bytes first (smallest set). All four
repeats (the usual secure/non-secure alias pattern) reduce to one real
difference: `RFCRC.DATA` (`+0x1C`, the live CRC accumulator,
read-only) reads `0xAAAAAA` on stock -- exactly matching `INIT`
(`+0x18`, which matches on both stacks already) -- but `0x0` on open.
`RFCRC.CMD.INITIALIZE` (`+0x010`, write-only, bit 0) is the public
command that loads `DATA` from `INIT`; `radio_crc_configure()` in
`radio.c` configures `EN`/`CTRL`/`INIT`/`POLY` but never issues it --
confirmed directly in source, no `RFCRC_CMD` offset was even defined
in this file before this section.

Tested live on hardware, carefully this time (an earlier attempt hit
the same bare-`resume`-doesn't-continue-the-real-firmware methodology
bug §7.34 already found -- caught again, redone correctly: catch
exactly at the real `TXEN` breakpoint, poke there, `resume` to that
exact address so the real firmware continues naturally). The poke
confirmed the command works exactly as documented (`DATA` becomes
`0xAAAAAA` immediately). But after letting `TXEN` execute and settle
for the usual 450ms: `RFCRC.DATA` drifts to the same value
(`0x34caed`) whether or not the poke was applied, and
`READOFFSET`/`FRC.STATUS`/`RAC.STATUS` are completely unchanged either
way. **This divergence is real but does not explain the
`ACTIVETXFCD` stall** -- `RFCRC.DATA`'s post-settle drift looks
free-running/incidental, not meaningfully coupled to processing the
actual staged packet.

**Fixed anyway, on its own merits.** This is a genuine, independently
confirmed divergence from stock's real CRC-engine initialization
behavior, unrelated to whether it explains the central mystery --
consistent with this project's standing "every divergence you find
must be recorded and fixed" goal applying to real hardware, not just
QEMU. Added the `CMD.INITIALIZE` write to `radio_crc_configure()`,
confirmed on hardware: `RFCRC.DATA` now reads `0xAAAAAA` right at
`TXEN`, matching stock exactly. QEMU 78/17 re-confirmed passing
(unaffected -- QEMU doesn't model `RFCRC.DATA`'s live accumulator at
all, so this is purely a real-hardware fidelity fix).

**Continued to `agc` next (36 differing bytes) -- same pattern, no
second fix found.** Every differing register is one of `STATUS0`/
`STATUS1`/`RSSI`/`IF` (all read-only, live measurement/interrupt
registers) or `CTRL0`'s `RSSISHIFT` field (decoded precisely: `0x27`
on stock vs `0x4E` on open -- a real, non-noise-bit difference, not
just incidental XOR fuzz). `AGC`'s whole purpose is continuous,
hardware-driven gain adjustment against real RF/ambient-noise
conditions -- `RSSISHIFT`/`STATUS*`/`RSSI` are exactly the fields such
a live loop would naturally drift over time, not static configuration
`radio_phy_set()` writes once (that part -- the PHY table's own AGC
entries -- was already confirmed byte-for-byte matching stock when
fully decomposed earlier in this project). Not confirmed as "purely
benign drift" with the same rigor as a real fix would need (would
require multiple time-separated captures to show genuine drift rather
than a one-off difference), but the pattern across both `rfcrc` and
`agc` now consistently points to live/measured state, not digital
frame-coder logic -- the diminishing signal here is itself useful
information for whoever continues this diff (`bufc`/`modem`/`rac`
remain unchecked).

**Finished the remaining three blocks (`bufc`, `modem`, `rac`): no
second fix, and the whole exercise is now conclusively closed, not
just paused.**

`bufc`'s differences are channel0's `ADDR` (just a build-specific RAM
pointer -- `efr32_tx_fifo`'s address differs between builds, never
meaningful) and `WRITEOFFSET`/`READOFFSET` (explained by the same
already-established caveat as §7.19: stock's real advertising payload
is longer than this project's deliberately minimal test payload, and
the two captures aren't necessarily at the identical point in each
stack's own TX lifecycle even when channel-matched).

`modem`'s differences are a mix of live demodulator-adjacent counters/
flags (matching §7.42's `MODEM.FSMSTATUS`-family finding) and what
read like access-address/sync-word-shaped bytes -- plausibly because
stock's real application and this project's minimal smoke-test
harness use different BLE access addresses, an application-level
parameter difference, not a protocol bug.

`rac`'s 52 differing bytes span the widest range of any block, so
mapped every one against `RAC_S_register.py`'s register names before
deciding whether to decode further (`STATUS`/`CMD`/`CTRL`/
`FORCESTATE`/`IF`/`IEN`/`TESTCTRL`/`SEQIF`/`SEQIEN`/`STIMER`/
`SEQCTRL`/`SR0`/`SR3`/`FRCRXWORD`/`SYNTHREGCTRL`/`CHPCTRL`/
`RFSTATUS`/`STATUS2`/`IFPGACTRL`/`IFADCDBG`/`PATRIM1`/`PGACAL`/
`PGACTRL`/`RFPATHEN`/`RX`/`TX`/`SYTRIM0`/`SYEN`/`SYMMDCTRL`/
`DIGCLKRETIMECTRL`/`XORETIMECTRL`/`SCRATCH6`/`SCRATCH7`). Every one
falls into a category already closed: registers this investigation
already exhaustively analyzed this session (`STATUS`, `SR0`, `SR3` --
§7.5.2's "documented reset-value difference, not a fidelity bug";
`DIGCLKRETIMECTRL`/`XORETIMECTRL` -- §7.24, already tested live with
no effect); live/status counters (`IF`/`IEN`/`SEQIF`/`SEQIEN`/
`STIMER`/`RFSTATUS`/`STATUS2`, the same free-running-state pattern as
every other block); per-physical-chip calibration trim (`PATRIM1`,
`PGACAL`/`PGACTRL`, `SYTRIM0`, `SYNTHREGCTRL`, `CHPCTRL`,
`IFPGACTRL` -- PA/PGA/synth trim registers that legitimately differ
between physical silicon units regardless of firmware); or test/debug
registers never touched by normal operation (`TESTCTRL`, `IFADCDBG`,
`SCRATCH6`/`SCRATCH7`).

**Conclusion for the whole §7.19/§7.42-7.43 mining effort:** one real,
independently confirmed production bug found and fixed
(`RFCRC.CMD.INITIALIZE`, §7.43); every other difference across all
seven radio blocks is accounted for by categories already understood
(live/measured state, per-chip trim, application-level parameter
differences, or registers already investigated and ruled out earlier
this session). This closes the existing `tests/hardware-lockstep/`
captures as a source of new leads -- not because the mining wasn't
worthwhile (it found a real fix), but because what's left in them has
now been checked and accounted for, not because it was skipped.

### 7.45 Checked for public documentation of `FRC`'s internals -- confirmed, not just assumed, that none exists

With the register-level search space exhausted, checked the one
remaining easy avenue before reaching for the M0+ sequencer's own
dispatch logic: does Silicon Labs' official, public EFR32 reference
manual document `FRC`'s `FSMSTATE` values or `ACTIVETXFCD`'s real
trigger condition? Found and read the official EFR32xG21 Wireless
Gecko Reference Manual (Series 2's closest public sibling to this
project's xG22 target, same radio IP) directly -- not inferred from
search snippets, the actual document. **Chapter 5, "Radio
Transceiver," is deliberately a black box**: a one-page overview
diagram and the explicit statement "The Radio Transceiver interface
is accessible through software drivers provided by Silicon Labs."
Every other peripheral in this same manual (`MSC`, `CMU`, oscillators,
etc.) gets a full register map and per-bit register description
section; the radio chapter gets neither. **This confirms, with direct
evidence from the actual official document rather than inference, that
there is no public register-level documentation for `FRC`/`RAC`/
`AGC`/`SYNTH`/`MODEM`/`BUFC` at all** -- not a gap in what this
project happened to find, a deliberate choice by Silicon Labs to only
expose this interface via the proprietary RAIL library. The `*_S_field.py`
models this project has used throughout (named fields, empty
description text) are evidently a byproduct of Silicon Labs' internal
tooling leaking into the public `simplicity_sdk` repo, not intentional
public documentation -- which is exactly why they have names but never
explain what any of them actually do.

This closes off "find a shortcut via public documentation" definitively,
not just as an assumption carried from earlier sessions. The only
remaining avenues are direct hardware experimentation (now exhausted at
the register level, per §7.1-7.43) or deeper reading of the M0+
sequencer's own dispatch logic (`fn_0x894`/`fn_0xd38`, deliberately
never ported per this project's charter, but still readable purely for
understanding).

### 7.46 QEMU fidelity: modeled the one `RFCRC.CMD.INITIALIZE` side effect §7.43 confirmed on real hardware

§7.43's real production fix (`radio_crc_configure()` never issuing
`RFCRC.CMD.INITIALIZE`) was confirmed, on real hardware, to make
`RFCRC.DATA` load from `RFCRC.INIT` -- a genuine silicon semantic this
project now has direct evidence for. Checked whether `vendor/qemu`'s
model implements that semantic at all: it does not. `RFCRC.CMD`/
`INIT`/`DATA` were fully generic, behaviorless register-table latches
(`EFR32_RFCRC_BASE` had only `IPVERSION` and the `EN`/`CTRL` write
masks modeled; nothing else). Added a dedicated `RFCRC.CMD` write
handler in `hw/arm/efr32bg22.c`, mirroring the existing `FRC.CMD`
handler's own shape exactly (write-only, early `return`, no
`efr32_radio_mask_write` table entry -- that path is dead code for a
register already fully handled by a dedicated block, confirmed by
checking `FRC.CMD`'s own precedent has none either): on
`INITIALIZE` (bit 0), `DATA := INIT`. Does **not** model the actual
running per-byte CRC computation during a real TX/RX byte stream --
there is no BUFC byte-stream hookup in this model to drive that, and
building one would be a much larger, separate undertaking disconnected
from any currently observed divergence; only this one directly-measured
register side effect is modeled. Added `test_rfcrc_cmd_initialize`
(`efr32bg22-test` 78→79 tests, all passing); `efr32bg22-rf-test`
(17/17) unaffected and still passing. Commit `1b7fa04e36` in the
`vendor/qemu` submodule (`Pandapip1/qemu.git` fork), bumped in the
superproject at `b1dc525`.

### 7.47 `fn_0xd38`'s calibration-loop gate, checked live on real hardware for the first time -- clear on both builds, not conclusive either way

§7.13's re-verification of `fn_0xd38` found a whole polling loop and
calibration block (local `0xe5a..0xea8`) that was deliberately left
unported, gated on `OBS_BLOCK_AA00011C` bit 8 -- a register this
project had only ever read from the static disassembly, never measured
live. `OBS_BLOCK_AA000054` (the base this same half of the function
uses throughout) was already identified as `BUFC`'s secure alias area
(`0xAA000000`, per the top-level register table); `AA00011C` is
therefore `BUFC`'s secure alias at sub-offset `0x11C` -- but, same
pitfall as §7.8's own `FRC` dump, **reading the secure alias
(`0xaa00011c`) through this debugger with Secure debug disabled reads
back a hard `0`, not the real value.** Had to read the non-secure
alias (`0xba00011c`) instead to get anything real. (Also re-confirmed,
the hard way, that `0xa8020000`/`0xa8004000` -- RAC/FRC's own secure
bases -- have the exact same problem; only ever trust the `0xb8xxxxxx`/
`0xba000000`-family non-secure reads from this debugger.)

**Measured on real hardware, both builds, at their own established
`TXEN`+settle checkpoints** (stock: breakpoint at `0xe3a0`, same
address this project has used since §7.6; open: `0x165fe`, this
build's current equivalent of the previously-recorded `0x16606` --
address shifted slightly between builds, re-derived fresh from this
exact binary's own disassembly rather than reused from an older note):

* Stock, 450 ms after its own `TXEN`: `RAC.STATUS=0x00400000`
  (`STATE=4`, `RXPD` -- stock has already finished this TX and moved on
  long before 450 ms, consistent with §7.6's "well under 100 ms"
  finding; this is not a "stuck TX" reading, and wasn't expected to
  be). `BUFC+0x11C` (non-secure) reads `0x06000000` -- bit 8 clear.
* Open, 450 ms after its own `TXEN`: `RAC.STATUS=0x09400000` (the
  usual stuck-`TX` baseline, confirming this was a valid capture).
  `BUFC+0x11C` (non-secure) reads `0x00000004` -- bit 8 also clear.

**Bit 8 is clear on both builds at these checkpoints -- consistent
with, but not proof of, `fn_0xd38`'s calibration loop never firing in
this scenario on either build.** This is *not* a clean, timing-
eliminated result the way §7.6's `ACTIVETXFCD` comparison was: §7.30
already established that this chip's transition speed outruns any
halt-based observation, so neither reading can rule out the bit having
been set-then-cleared, or never-set, during the actual microsecond-
scale `TXEN` servicing window itself -- only that it is not latched
set by the time either build settles. Both values also differ in
which *other* bits are set (`0x06000000` vs `0x00000004`), which is
expected and uninteresting: these reads aren't timing-matched (stock
is long past TX; open is still mid-stall), so this isn't a valid A/B
diff, just two separately-recorded facts. **This closes the loop on
§7.13's open thread as far as live measurement can take it**: the
calibration block was already confirmed structurally unreachable on
the open build (never ported into `radio_seq.S` at all, regardless of
any register value), and this is the first real evidence -- weak, but
real, and in the expected direction -- that stock itself likely
doesn't exercise it in a plain single-TX scenario either. Not escalated
further without a genuinely new technique for observing sub-instruction
hardware state, which this project does not have.

### 7.48 A new, direct intervention tried on real hardware: forcing `BUFC`'s `READOFFSET` past the header does not unstick `FRC`

Every prior check of this stall read registers; none had tried actually
*writing* one to see what happens. §7.31 established `READOFFSET` sits
at `2` (the header) forever while `WRITEOFFSET` sits at `0xB` (the full
staged frame) -- `FRC` never asks for a third byte. Tried directly
forcing the question: halt the open build at its usual settled `TXEN`
checkpoint (`RAC.STATUS=0x09400000`, `FRC.STATUS=0x01100200`,
`READOFFSET=2` -- baseline, as always), then `mww 0xba000018 3` (the
real non-secure `READOFFSET` address, per §7.31) to advance the pointer
by hand, as if `FRC` had consumed one payload byte.

**First attempt, resumed for 100 ms, found the forced value had reverted
to `2`** -- initially looked like `FRC` was actively fighting the write.
**Re-tested at 1 ms/5 ms/20 ms instead and the forced `3` held exactly
as written at every one of those shorter windows.** The 100 ms reversion
is therefore not `FRC` self-correcting; it's this project's own
already-documented fact resurfacing (§7.6's "methodology bug" note):
the open LLL re-arms/retries `TXEN` periodically on roughly this
timescale, and each fresh attempt re-stages the `BUFC` channel from
scratch, incidentally overwriting whatever a debugger had poked in
during the previous attempt. Not a new finding by itself, but it's why
this test has to use a short resume window to mean anything.

**With the forced value genuinely held (confirmed stable at 20 ms,
not yet reverted), `FRC.STATUS` and `RAC.STATUS` were read again:
byte-for-byte unchanged (`0x01100200`/`0x09400000` -- same
`ACTIVETXFCD=0`, same `FSMSTATE=17`, same stuck `TX`).** Advancing the
externally-visible read pointer by hand does not nudge the frame
coder's internal state machine at all. **This is a real, clean negative
result with a concrete implication**: `READOFFSET` advancing is purely
an *output* of `FRC`'s internal FSM making real progress, not an input
that can drive it -- the FSM must track its own notion of how many
bytes it has actually processed internally (separately from, or in
addition to, this externally-writable register), so spoofing the
externally-visible counter alone cannot fool it into proceeding. The
real block is entirely inside `FRC`'s own internal, unexported state,
consistent with every other negative result in this investigation, but
now demonstrated by direct intervention rather than inferred from
reading status bits alone. One further lever was worth trying along
these lines: `FRC.CMD.STATEINIT` -- see §7.49 immediately below.

### 7.49 `FRC.CMD.STATEINIT` tried live: moves `READOFFSET`, never touches `ACTIVETXFCD`/`FSMSTATE`, and repeated use crashes the chip

`FRC_S_field.py` names one `FRC.CMD` bit this project had not yet
tried live: `STATEINIT` (bit 11, write-only) -- a name that reads like
exactly the kind of "reset the frame coder's internal state machine"
primitive this stall seems to need, and a more plausible-sounding
candidate than any bit tried before it. Tried it the same way as
§7.48, on the open build at its usual settled, stuck baseline
(`READOFFSET=2`, `FRC.STATUS=0x01100200`, `RAC.STATUS=0x09400000`):

* **First write** (`mww` bit 11 of `FRC.CMD`, `0x800`, held `10 ms`):
  `READOFFSET` advanced from `2` to `4` -- a real, repeatable effect,
  not a no-op. `WRITEOFFSET` stayed `0xB` (the full 11-byte frame,
  unchanged). **But `FRC.STATUS` and `RAC.STATUS` were byte-for-byte
  identical to baseline** -- `ACTIVETXFCD` still `0`, `FSMSTATE` still
  `17`, `RAC` still stuck in `TX`.
* **Second write** (same bit, continuing from the first): `READOFFSET`
  advanced again, `4` to `6` -- same `+2` step as the first write --
  but this time the core came back **halted in a real `HardFault`**
  (`Handler HardFault`, `pc=0x1d8d0`), not a clean debug-halt.
* **Third write, same session, same fault state**: `READOFFSET`
  advanced once more, `6` to `8` -- the write still "works" mechanically
  even with the CPU already faulted, consistent with this being a pure
  hardware-side effect independent of what the CPU is doing -- but the
  chip was by this point in a confirmed bad state. Recovered with a
  full reflash (confirmed clean: `READOFFSET` back to `2`, same
  baseline as always, no lasting damage).

**Interpretation, not overclaimed: `STATEINIT` does not reset the
frame coder back to a *working* state -- it appears to re-run the same
"treat the next two bytes as a length-style header and advance past
them" step `FRC` already performed once at `TXEN`, but now starting
from wherever `READOFFSET` currently sits, rather than from byte 0.**
Since bytes 2-3 (and later 4-5) of a real BLE advertising PDU are not
another length header, this just walks the read pointer further into
the payload without ever resolving whatever condition keeps
`FSMSTATE`/`ACTIVETXFCD` latched, and walking it far enough
destabilizes something else into a fault. **Treat this the same as
`BLOCKINIT` (§7.31): do not live-poke `FRC.CMD.STATEINIT` again
outside of a full compiled, reviewed, revertible change** -- it is not
a safe exploratory primitive, even though, unlike `BLOCKINIT`, its
*first* use alone does not immediately fault. No further `FRC.CMD`
bits remain untried that look more promising than this; the
intervention-based technique introduced in §7.48 has now been pushed
about as far as this project's current register map supports.

### 7.50 `fn_0x388` decompiled and verified: RX-side bookkeeping, same unreachable status as its siblings

Continuing §7.7's M0+ sequencer sweep: `fn_0x388` (one of the
"remaining as-yet-unexamined callees" listed there) is now fully
lifted and verified in `sequencer-decomp/src/fn_0x388.c`. Called from
exactly one place -- `fn_0x23a0` (IRQ1), conditionally -- confirmed via
grep across every decompiled file in this directory; IRQ1 is not
enabled by the open stack's own `radio_seq.S`, so this function (and
everything it calls) is unreachable on the open build, the same status
as `fn_0x13b0`/`fn_0xa76`/`fn_0xa80`.

Takes two arguments, not one -- the first draft modeled it as a
single-parameter `void` function and only caught the error by
cross-checking `fn_0x23a0.c`'s own already-verified call site
(`fn_0x388(local0, gate_base_val)`, using the return value). The real
second parameter is `RAC_GATE_BASE`'s raw value (RAC+0x40, already
named elsewhere in this project); the first is genuinely unused by
this function's body at all (confirmed: no instruction anywhere in its
disassembly reads the incoming `r0`), present only because the
caller's own dispatch convention threads a value through regardless.
Also hit, and fixed the same way as `fn_0x21d8`'s delay counter
(`volatile`), a second instance of the "the optimizer proved a
write-only value has no effect and deleted the instructions that
compute it" pitfall.

Semantically, this is RX-side bookkeeping, not TX: it acks
`AGC.SEQIF.RSSIVALID` on an early-return path and
`MODEM.SEQIF.RXTIMDET` on exit (both RX-measurement/timing-detect
flags, newly identified this pass), and walks a small, previously-
unidentified table of 24-byte records (base pointer at SEQRAM `0x3ec8`)
looking for the first one whose own state byte reads below `8` --
consistent with a per-channel or per-slot RX scheduling structure.
This reinforces, independently of the reachability argument alone,
that this function was never a plausible `ACTIVETXFCD` lead even
before confirming it structurally unreachable. `fn_0x7d8`/`fn_0x690`/
`fn_0x564`/`fn_0x7ea` are left as opaque calls in this lift. Remaining
unexamined: `fn_0xace`, `fn_0xb74`.

### 7.51 A real, new discovery while chasing `fn_0xace`/`fn_0xb74`'s callers: a sixth vector, IRQ11, correcting §7.7's "all five" claim -- and confirmed, not assumed, to not change anything

Before decompiling `fn_0xace`/`fn_0xb74` themselves, checked every call
site (`bl 0xace`/`bl 0xb74`) across the whole disassembly rather than
trusting the one already-known caller (`fn_0x23a0`/IRQ1). **Found a
second pair of call sites, at local `0x2938`/`0x2942`, inside a
function starting at local `0x2898` this project had never
encountered before -- not referenced by name in any existing
decompiled file or in this document.** That function itself opens by
calling `fn_0x894` directly (the exact shape every other IRQ8/9/10/13
vector handler shares), and its own vector-table word is **`0x2899`
(word 27, i.e. `IRQ 11`)** -- confirmed by dumping all 64 vector words
directly from the raw sequencer binary, not inferred. **This means
§7.7's own "Status" paragraph ("all five of this object's vectors
beyond IRQ8/IRQ9 that this pass turned up") undercounted: there are
at least six** (`fn_0x19d0`, `fn_0x2362`/`fn_0x2390`, `fn_0x23a0`,
`fn_0x2644`, and now `fn_0x2898`/IRQ11).

**Checked immediately, with direct evidence, whether this changes
reachability on the open build -- it does not.** `radio_seq.S`'s own
`NVIC.ISER0` write is `EFR32_RAC_IRQ_TXWARM | EFR32_RAC_IRQ_TX`, which
are literally `(1 << 8)` and `(1 << 9)` (`radio_seq.S:67-68,289`) --
bit 11 is not part of that mask, so IRQ11 is exactly as unreachable on
the open build as every other non-IRQ8/9 vector already catalogued.
`fn_0x2898` itself is not decompiled in this pass (a sizable function
in its own right, confirmed unreachable before investing further, the
same scoping judgment already applied to `fn_0x388`/`fn_0xace`/
`fn_0xb74`'s own siblings) -- recorded here specifically because
finding a previously-uncatalogued *vector* (not just an uncatalogued
helper function) and then directly checking its reachability, rather
than assuming the existing "five" count was exhaustive, is exactly the
kind of check this project's own rigor standard calls for. `fn_0xace`
and `fn_0xb74` themselves remain undecompiled; both their known callers
(`fn_0x23a0`/IRQ1 and now `fn_0x2898`/IRQ11) are confirmed unreachable.

### 7.52 Retried §7.5.4's splice-into-stock test (`build-stage2.3`) with the now much fuller decompiled set -- same hang, as §7.5.4 already predicted

§7.5.4 (above) only relocated five functions and hit a hang in stock's
M33-side RAIL calibration helper, then went on to conclusively show
(still in §7.5.4) that the hang is **not** about those functions at
all: a "null" variant that relocated nothing and redirected no call
sites, and a variant that changed a single unrelated byte deep inside
the object with the length completely untouched, both hang identically
at the same `0x13056` busy-wait -- an unlocated M0+-side integrity gate
that fails on *any* byte-level change to the sequencer object,
confirmed via register-level lockstep against stock down to the exact
diverging status bit (`RFMAILBOX` bit 0, `0xB802C040`), and explicitly
confirmed **not** on the open stack's own TX path (`radio.c`/
`radio_seq.S` reference neither mailbox address, and the stock
calibration helper this hangs inside is only ever called by stock's
own RAIL library at boot, before any TX attempt).

Extended `sequencer-decomp/build_patched_stage.sh`/`link_patch.ld` to
relocate every function this project has since verified that's
actually reachable from `fn_0x894`'s real IRQ8/IRQ9 dispatch tree --
`fn_0xd38`/`fn_0x80c`/`fn_0x1af4`/`fn_0x1a18`/`fn_0x21d8`, ten total,
up from the original five -- and reran the full pipeline (compiles
clean, `west build` succeeds, flashes and boots). **Result: the
identical hang, at the identical instruction** (`pc=0x13056`, same
busy-wait on the same `RFMAILBOX` bit). Exactly what §7.5.4's own
"integrity gate" conclusion already predicted, not a new finding --
recorded because the harness itself is now a meaningfully more
complete, reusable tool (every function with a known-correct lift is
now exercised end-to-end by this test, not just the first five), and
because re-deriving an already-closed conclusion from scratch (rather
than finding and reading §7.5.4 first) cost real effort that a closer
read of this document would have saved. **This splice vehicle remains
blocked for validating any further sequencer modification until the
integrity gate itself is located** -- a bounded, well-defined reverse-
engineering target in its own right (likely a checksum/signature the
M0+'s own boot or mailbox-service path checks against the object's
real bytes), but a different task from decompiling `fn_0x894`'s call
tree, and not yet attempted.

### 7.53 The M0+ AP3 single-stepping harness attempted -- found and fixed two real bugs, then found something much bigger: the "integrity gate" was partly a test-methodology artifact

Picked up §7.52's open item: single-step the M0+ core directly (AP3,
`tests/sequencer-probe/xg22-dual-core.cfg`) to see where it actually
diverges, rather than inferring purely from the M33-side symptom.

**AP3 access confirmed still working, and a reliable catch recipe
found.** `tests/sequencer-probe/README.md`'s existing dual-core config
(M33 GDB on 3333, M0+ on 3334) connects and examines cleanly on this
OpenOCD build. The same "arm a breakpoint before the reset" principle
`bg22_reset_to_breakpoint` already established for the M33 works for
M0+ too, and more simply -- M0+ is plain Armv6-M with no Secure state
to refuse the halt in the first place: arm `bp <addr> 2 hw` on
`efm32.seq`, `resume` it, then pulse the M33's own
`AIRCR.SYSRESETREQ` (`mww 0xe000ed0c 0x05fa0004` on `efm32.cpu`) --
the M33's own reset cascades into redoing the whole RAC-driven M0+
hold/release dance, and the pre-armed M0+ breakpoint catches the
*exact* instant M0+ reaches its own reset vector (confirmed:
`pc=0x00002a2c`, matching the already-known Reset_Handler address).
Reused `tests/hardware-lockstep/trace_stores.py` unmodified for the
actual single-stepping (it already defaults to port 3334 -- this tool
was clearly built with exactly this use in mind).

**Bug #1, found immediately: `build_patched_stage.sh`'s own splice
script was corrupting the sequencer's vector table in every single
test this whole `build-stage2.3` thread has ever run, stock or
modified.** The very first AP3 catch attempt on the (apparently
working, per §7.52) 10-function splice hit a genuine M0+ `HardFault`/
lockup almost immediately (`pc=0xfffffffe`, garbage `msp`), not the
expected reset-vector breakpoint. Tracing it down: `patched_seqram.bin`
itself had a scrambled vector table. Root cause: GNU `dd`'s
`seek=$appended_base` passed the *literal string* `"0x3000"` as a
command-line argument -- and GNU `dd` parses `0x` as **a zero
multiplier** (`dd: warning: '0x' is a zero multiplier`), not hex
notation, silently treating `seek=0x3000` as `seek=0`. The ten
relocated functions were being written starting at byte offset `0`
-- directly over the vector table -- not at local `0x3000` as
intended. The five small per-call-site patches were unaffected (they
use `seek=$((off))`, arithmetic-evaluated by *bash* first, which does
understand hex -- only the one appended-block `dd` was missing that
wrapper). Fixed by adding the same `$(())` wrapper. Rebuilt: the
vector table, the five call-site patches, and the appended block at
`0x3000` are now all exactly where they should be (verified by
direct byte inspection, not just "it built"). Re-tested on AP3: no
more `HardFault` -- M0+ now boots cleanly to the identical resting
point stock reaches (`pc=0x2bba`, `msp=0x3e30`).

**Bug #2, found next: my own manual OpenOCD commands this session were
using `board/efm32.cfg` (stock OpenOCD's own file, which sources
`target/efm32.cfg` → `target/silabs/series0.cfg` -- wrong, Series 0
Gecko) instead of `target/silabs/xg21.cfg` (what
`scripts/flash-bg22-openocd.sh` actually uses, correct Series 2).**
This was invisible for ordinary halt/read/breakpoint work (the generic
`cortex_m` target behaves the same either way), but broke flash
*writes* outright (`Error: Failed to write memory at 0x10000000`) the
moment a test needed flash+run+check in one continuous session rather
than delegating to the wrapper script.

**With both bugs fixed, re-testing surfaced something far more
significant than either bug: running flash+run+check in one
*continuous* OpenOCD session (no disconnect/reconnect at any point)
changes the outcome.** Checking **stock** this way --
`bg22_fast_flash` then `sleep 500` then `halt`, all in one session --
reaches `RAC.STATUS=0x00400000` (**`STATE=4`, `RXPD`** -- a real,
successful TX-then-RX transition, same as every other confirmed-good
stock measurement in this document). Checking stock the *other* way
this session had been using for quick comparisons -- flash via the
wrapper script (which runs its own session and exits), then open a
*separate* fresh session and just `init`+`sleep`+`halt` with no reset
of its own -- reliably catches stock stuck at the identical
`pc=0x00013056` mailbox-wait this whole §7.5.4 thread has been
chasing as a sign of sequencer-blob corruption. **Same stock binary,
same hardware, two different outcomes, purely as a function of
whether a fresh OpenOCD session's own connect/examine happens to land
inside some real-time-sensitive window during early boot.** This is a
test-methodology confound this project had not previously isolated:
reconnecting a debug session (even just to `halt` and read, no
breakpoints, no single-stepping) during this specific early window
can itself cause the same hang §7.5.4 attributed entirely to blob
modification.

**This does not fully exonerate the splice, though -- re-tested both
builds with the *identical*, now-confound-free continuous-session
method, and they still disagree.** Stock: `RXPD`, success, every
time tried this way. The (now vector-table-correct) 10-function
`build-stage2.3`: still stuck at `pc=0x00013056`, `RAC+0x4C` bit 17
still clear. So there is a real difference tied to the splice after
all -- just not the all-or-nothing "any single byte anywhere trips an
unlocated integrity gate" framing §7.5.4 built on data that, with
hindsight, wasn't free of this exact confound. The more defensible
framing now: `RAC+0x4C` bit 17 (already flagged by this project,
`fn_0xd38.c`, as "new, unnamed, read-only" -- hardware-set, not
software-set) gates a pre-existing, unmodified stock busy-wait loop at
local `0x2b6a` (confirmed via AP3: M0+ parks there, repeatedly cycling
through a real `wfi`, for as long as observed -- re-confirmed after
letting it run completely freely for whole seconds, not a single-step
artifact). Stock's real boot reaches this exact loop too, and plainly
gets past it quickly enough in normal operation; the splice's extra
~1.9 KB of copy length and its five relocated, far-jump call sites are
the most likely source of whatever real-time shift keeps this specific
run from reaching bit 17's trigger condition in time -- a genuine, if
mundane, timing sensitivity in stock's own calibration/mailbox
handshake, not a deliberate anti-tamper mechanism. What exactly sets
`RAC+0x4C` bit 17, and how tight the real timing margin actually is,
remains unestablished.

**Net result:** two real, fixed bugs in this project's own tooling
(the `dd` hex bug, the wrong target file); one major, now-documented
methodological hazard for all future AP3/OpenOCD work on this chip
(§7.54 below); and the splice-into-stock vehicle's own verdict
narrowed from "mysterious integrity gate, blocks everything" to "a
real timing sensitivity in a stock-only calibration path, triggered by
splicing's own overhead" -- still blocking this specific validation
technique, but no longer mysterious, and still independently confirmed
irrelevant to `ACTIVETXFCD` (the open stack's own code never
references the mailbox addresses at all, re-confirmed by direct grep
this session).

### 7.54 New standing methodology hazard: a fresh OpenOCD session's own connect/examine can itself hang this chip's early boot, indistinguishable from a real firmware bug unless ruled out

Any check of this chip's state that connects a *new* OpenOCD session
to hardware that is already running (rather than including its own
fresh, in-session reset) risks observing a spurious hang that has
nothing to do with the firmware under test -- confirmed directly in
§7.53 on unmodified, known-good stock firmware. The project's own
established `bg22_reset_to_breakpoint` pattern (arm a breakpoint,
*then* pulse `AIRCR.SYSRESETREQ` from within that same session, then
proceed) is not just a workaround for Secure-debug-disabled halt
refusal -- it also happens to avoid this hazard, because it always
performs its own fresh reset immediately after connecting, before any
measurement. **Any test that flashes or connects in one OpenOCD
session and then checks state in a separate, later session without
first doing a fresh in-session reset should be treated as unverified
until redone the safe way.** This was not an issue for the bulk of
this project's own `ACTIVETXFCD` measurements, which have consistently
used the `bg22_reset_to_breakpoint` pattern throughout -- but it did
affect several of this session's own quick stock-vs-modified
comparisons in §7.53 before being caught, and should be checked for in
any future ad hoc hardware check that skips the full reset dance for
convenience.

### 7.55 A major, previously-untested QEMU fidelity gap: the open stack's own TX attempt never reaches `RAC.STATUS=TX` in simulation at all

**Superseded by §7.56:** retracted in full.

Every existing QEMU regression for this radio (`efr32bg22-test`,
`efr32bg22-rf-test`) exercises register behavior through direct qtest
MMIO pokes, or (`radio-sequencer-executes-open-code`) a synthetic
probe that deliberately never acknowledges the TXWARM interrupt. None
of them boot the *real* open stack -- both emulated cores, real
`radio.c`/`radio_seq.S`, a real advertising attempt -- end to end and
watch what `RAC.STATUS` actually settles to. Did that directly, using
`tests/radio-port/run-qemu-smoke.sh`'s own working invocation pattern
(`-icount shift=5 -trace enable=efr32bg22_rac_command`, which is what
makes this board model's two vCPUs deterministic and schedulable at
all) against the current `build-stage3/zephyr/zephyr.bin`.

**Result: the simulated `RAC.STATUS` never leaves `TXWARM` (state 8)
at all.** The guest's own LLL retry logic keeps re-issuing `TXDIS`/
`RXDIS` then `RAC.CMD.TXEN` roughly every 100ms of virtual time --
confirmed directly from the decoded `efr32bg22_rac_command` trace,
`RAC.CMD=0x00000001 state=8` repeating 58 times over a 5-second run,
never once advancing to `state=9` (`TX`). This is a genuinely
different failure from anything on real hardware: real silicon
reaches `TX` immediately and holds there with `ACTIVETXFCD` never
set (the entire subject of this whole document); **QEMU's own
simulation cannot even reach `TX` in the first place.** `BUFC`
`WRITEOFFSET` does reach `0xb` (the real 11-byte packet gets staged
correctly), so this is specifically a `RAC` state-machine/interrupt
problem, not a BUFC/FRC one.

**Traced the mechanism as far as static reading supports.**
`efr32_rac_set_state()` (`hw/arm/efr32bg22.c`) early-returns if the
new state equals the current one (`if (s->rac_state == state) return;`,
before line ~1012) -- so entering `TXWARM` only sets `RAC.SEQIF` bit
24 and raises the real, separately-modeled M0+ sequencer vCPU's own
`IRQ8` line (`qemu_set_irq(s->seq_irq_txwarm, ...)`, driven live off
`SEQIF & (1<<24)`, a genuine *level*, not an edge) when the state
actually changes -- which each retry's own `TXDIS`/`RXDIS` (forcing
`OFF`) then `TXEN` (`OFF`→`TXWARM`) should satisfy fresh every single
time. The TXWARM→TX transition itself (`hw/arm/efr32bg22.c`, the
`RAC.IF`/`IEN`/`SEQIF`/`SEQIEN`-write-triggered check) fires only as
a side effect of a write to one of those four registers landing while
`rac_state==TXWARM` and bit 24 already reads clear -- and the real
sequencer's own handler (`radio_seq.S`'s
`EFR32_RAC_SEQIF_TXWARM_ACK_MASK = (1<<24)`, confirmed to be the
*correct* bit QEMU also checks) does perform exactly that
clear-and-check write on every `IRQ8` entry. The trace does show the
sequencer vCPU performing SEQIF-clear writes repeatedly (~553 cycles
over the run, 2213 raw writes to the `SEQIF` CLR alias) -- far more
cycles than the 58 `TXEN` retries, meaning the sequencer vCPU *is*
genuinely executing and *is* reacting to something, just evidently
not landing its ack at the one instant each `TXWARM` entry's check
window is open.

**The one variable tried that changes this picture: QEMU's own vCPU
scheduling mode.** `-icount` forces single-threaded TCG (deterministic
execution requires it), meaning the M33 and M0+ vCPUs are two
round-robin time-sliced guests on *one* host thread -- a real
difference from silicon, where M0+ is a genuinely independent,
always-live physical core. Retried the identical test with
`-accel tcg,thread=multi` (true concurrent vCPU threads, no `-icount`)
expecting this to let the sequencer's ack land in time -- **it did
not confirm that cleanly**: under `thread=multi` the run stalled even
earlier (`BUFC.WRITEOFFSET` never even reached `0xb` in over 25 real
seconds, versus ~5 seconds under `-icount`), which could mean this
device model is not actually safe under true concurrent vCPU access
(a different, also-real problem) rather than disproving the
scheduling-fairness theory outright. **Not conclusively root-caused.**
The honest state of this finding: a solid, reproducible divergence
(QEMU cannot currently simulate the open stack's radio TX path
reaching `RAC.STATUS=TX` at all, under any vCPU scheduling mode tried
so far), with several concrete, plausible, but not yet dispositive
contributing mechanisms identified in the actual device-model code
rather than guessed at.

**Why this matters, and why it was never caught before:** this is the
first time in the whole `ACTIVETXFCD` investigation that the open
stack's real TX path has been run under QEMU at all with both cores
live and watched end to end, rather than tested via isolated qtest
register pokes. It means QEMU currently cannot be used to reproduce
or investigate *any* real TX-path behavior for the open stack --
including `ACTIVETXFCD` itself -- since the simulation diverges from
real hardware even earlier than the bug this whole document is about.
Not yet fixed; recorded per this project's own "every divergence must
be recorded" rule. The next concrete step, if picked up again: add a
dedicated qtest that boots the real open image (not a synthetic probe)
through to `TXEN` and asserts `RAC.STATUS` reaches `TX`, so this
divergence has a standing regression the moment it's understood well
enough to fix, and so any fix attempt has an immediate, automated
pass/fail signal instead of needing a manual trace read each time.

### 7.56 §7.55 retracted in full: there was no divergence -- a tracepoint-semantics misreading, not a real bug

Asked to fix §7.55's "finding" and, in the course of trying,
discovered the finding itself was wrong. `efr32bg22_rac_command`'s own
trace line prints `s->rac_state` *at the moment `RAC.CMD` is written*
-- for a `TXEN` write specifically, that is **always** `TXWARM`
(state 8), by construction: `efr32_rac_set_state(s, TXWARM)` runs
synchronously inside the same command-write handler this tracepoint
instruments, before any later, *separate* code path (the one
triggered by a subsequent write to `RAC.SEQIF`/`IF`/`IEN`/`SEQIEN`)
ever gets a chance to advance the state further. Seeing
`RAC.CMD=0x00000001 state=8` on every single `TXEN` in §7.55's trace
was not evidence of being stuck -- it is *exactly* what a healthy,
fully-working transition looks like through that specific tracepoint,
which was never designed to report the eventually-settled state.

**Directly disproved by re-running the identical scenario with the
correct instrument for the question: `-trace enable=efr32bg22_rac_state`**
(the tracepoint inside `efr32_rac_set_state()` itself, firing on every
real transition, `previous_state -> state`), against the completely
unmodified, un-instrumented code. Result, every cycle, with no
exceptions across the whole run: `RAC state 0 -> 13`, `13 -> 8`
(TXWARM), `8 -> 9` (**TX, reached cleanly, every single time**), then
`9 -> 0` roughly 155 microseconds later (matching the already-
documented, pre-existing `150-us TX timer` simulator abstraction from
earlier work in this document -- not a new mechanism), then the whole
cycle repeats roughly 100ms later, matching a normal BLE advertising
interval. **QEMU's simulation of the open stack's own TX attempt
reaching `RAC.STATUS=TX` works correctly and always has.** There is no
fidelity gap here to fix.

**What actually happened, for the record (this project's own standing
rule is to document the mistake, not just quietly delete it):** every
single piece of evidence in §7.55 (the repeated `state=8` readings,
the 58:0 ratio of attempts to successes, the "sequencer vCPU runs but
its ack never lands" narrative, the `-icount`-vs-`thread=multi`
comparison) was real, reproducible, and correctly observed -- the
*interpretation* of what "state=8 on every `RAC.CMD` trace line" meant
was wrong. Two follow-up attempts at a "fix" (forcing the writing vCPU
to yield via `cpu_exit()`, and explicitly `qemu_cpu_kick()`-ing the
sequencer vCPU) were tried and, consistent with there being no real
bug, made no observable difference to the (correctly unchanged)
`rac_command` trace output -- which in hindsight is exactly what
"the thing you're trying to fix was never broken" looks like, not
evidence the fix failed. Those two change attempts were not kept; the
device model is unmodified by this whole thread. **Lesson for next
time, recorded in [[feedback-rigor-and-methodology]]: when a
tracepoint's value doesn't match an independent, direct-register
check, suspect the tracepoint's own placement/timing semantics before
concluding the device model itself is wrong** -- especially for any
tracepoint that fires *inside* a command handler rather than at the
point a value actually settles.

### 7.57 A genuinely new, direct, real-hardware confirmation: the open sequencer's own IRQ8 handler, single-stepped live via AP3 for the first time

The QEMU investigation above (§7.55-7.56) ended up building, as a
side effect, a detailed model of exactly how `RAC.SEQIF` bits 24/25
drive the `TXWARM`→`TX` handshake. §7.15 already proved (via
self-instrumented trace words) that the open sequencer's own real
`IRQ8` handler completes within microseconds on real hardware --
but that was always an indirect, after-the-fact proof (bounded
counters read post-hoc), never a direct, instruction-by-instruction
observation of the handler actually running. AP3 single-stepping was
used earlier this session only against the *stock* sequencer
(§7.53/7.54); it had not yet been pointed at the open stack's own
`radio_seq.S`.

Did that for the first time: flashed `build-stage3`, started the
dual-core OpenOCD session (`xg22-dual-core.cfg`), armed a breakpoint
on `efm32.seq` at `0x11e` (`efr32_radio_seq_exception`'s real address,
read from `radio-sequencer.elf`'s own symbol table), and resumed.
Caught reliably, repeatedly, confirmed via `current mode: Handler
External Interrupt(8)` -- a real, live `IRQ8` entry, not an inferred
one. **Found along the way that the M33 re-issues a fresh `SEQRESET`
on every retry cycle** (visible as repeated `external reset detected`
messages on `efm32.seq`), which disrupted the first single-step
attempt mid-trace; halting `efm32.cpu` (the M33) immediately after
catching the breakpoint, before single-stepping, fixed this cleanly --
worth remembering for any future attempt at this same technique.

`tests/hardware-lockstep/trace_stores.py` (unmodified, already built
for exactly this) single-stepped the real handler to completion in 65
steps (`"complete": true, "boundary": "exception-return"`), capturing
every real MMIO write with real addresses and values: the gate-pulse
prologue (`SR0`/`SR1`/RXENSRCEN clears, matching `radio_seq.S`'s own
named constants exactly), then **`SEQIF_CLEAR=0x01000000` (bit 24,
the `TXWARM` ack) immediately followed sixteen steps later by
`SEQIF_CLEAR=0x02000000` (bit 25, the `TX` ack) -- both inside this
single `IRQ8` invocation.** This means real silicon's own `RAC` state
machine transitions `TXWARM`→`TX` essentially instantaneously once
bit 24 clears (fast enough that the same handler's own universal
exit-ack, reading whatever's currently in `SEQIF`, catches bit 25
too) -- the exact same bit-24/bit-25 handshake shape this session
independently reverse-engineered from `vendor/qemu`'s own model
while investigating §7.55, now confirmed to be real silicon's actual
behavior, not just this project's model of it.

**Net result: another real, direct, hardware-level confirmation that
the M0+/RAC dispatch handshake itself works correctly, reinforcing
(not changing) the conclusion already reached by every other avenue
in this document.** `ACTIVETXFCD`'s real blocker remains conclusively
outside anything software-visible on either core -- it is inside
`FRC`'s own internal, undocumented hardware state machine, which no
further instruction-level tracing of the M33 or M0+ side can
illuminate, since that machine's internal transitions are not
observable except through its own status register (already read
exhaustively throughout this document). This specific technique
(AP3 single-step of the *open* sequencer's real `IRQ8`/`IRQ9` entry)
is now proven and reusable, should a genuinely new angle on `FRC`
itself ever surface.

### 7.58 Stage 2.5 revisited with AP3: the real proprietary coprocessor
double-faults into CPU lockup under the open M33 driver, every launch
attempt -- a sharper, more specific finding than §7.57's blanket "FRC
is unknowable" conclusion, confined to this one configuration

`docs/hardware-bringup.md`'s existing stage-2.5 write-up (open M33 +
proprietary M0+, 2026-10-27) never attached a debugger to AP3 during a
live run -- it only read M33-side peripheral registers afterward, and
concluded the real coprocessor "did not consume the open driver's
packet" (`BUFC.WRITE=11`, `READ=2`) without ever observing the M0+
core's own state directly. §7.57's AP3 technique, proven minutes
earlier against the *open* sequencer, had not yet been pointed at the
*real* one running under the open M33. Did that now, for the first
time.

Reused the still-current `build-stage2.5` (confirmed via symbol table
and unchanged `radio.c`/`CMakeLists.txt` history since the original
build -- no rebuild needed), reflashed it fresh, and connected
`xg22-dual-core.cfg`. **Before any command was issued** -- a fully
passive connection, nothing armed, nothing resumed -- OpenOCD was
already repeatedly logging `clearing lockup after double fault` /
`external reset detected` on `efm32.seq` in a tight, continuous loop.
Reproduced identically from a second clean reflash (byte-verified
erase/write/verify) to rule out the first observation being an
artifact of this session's own earlier (failed) breakpoint attempt.
**The real proprietary M0+ coprocessor enters ARMv6-M CPU lockup --
an unrecoverable double-fault state that halts the core until an
external reset -- on every single launch attempt the open M33 driver
makes, cycling continuously.** This is a qualitatively different and
more severe failure than the "silently didn't finish" symptom
documented in the original write-up; nobody had previously looked at
the M0+ core itself to see that it was crashing, not just under- or
non-responding.

Freezing `efm32.cpu` (the M33) stops the open driver from re-issuing
`SEQRESET`, which stops the loop and lets the M0+'s frozen lockup
state be read at leisure. Armv6-M has no `CFSR`/`HFSR` (those are
Armv7-M+; this core doesn't implement that fault-detail path at all,
confirmed by both reading as `0`), so the only available clue is the
stacked `LR`, which pointed into a small helper called from a function
that stores a caller-supplied register argument into a memory location
loaded from a nearby literal pool, then polls bit 0 of an offset from
that same pointer -- consistent in shape with a configuration/handshake
routine whose correct behavior depends on something the M33 side is
expected to have already set up. That is the same general "M33-to-
coprocessor ABI/launch contract" gap the original stage-2.5 write-up
already named as the open suspect; this is now a direct, reproducible,
instruction-level handle on it rather than an inference from absence.

Followed the literal-pool pointers the crashing call site and its
nearest neighbors actually use (read directly from the capture's raw
bytes, not misdisassembled as code): all three resolve into the
`0xA802xxxx` `RAC` base/SET/CLR-alias triple this project already
established in `tests/sequencer-probe/README.md` (base, `+0x1000`,
`+0x2000`), targeting **`RAC` base offset `+0x05C`** -- read once
(result unused by this snippet), then bits 5 and 4 unconditionally
cleared and set via the alias pair. **`+0x05C` is not one of `RAC.SR0`
-`SR3` (`+0x040..+0x04C`, this project's own established range, §7.5)
and does not appear anywhere in `modules/efr32-radio/hal/radio.c`'s
`EFR32_RAC_*_OFFSET` table at all** -- the open M33 driver has never
named, read, or written this sub-register. Given the original valid
capture was deliberately taken immediately before the proven stock
firmware's own `RAC.CTRL.CPUWAITDIS` release write, the obvious
hypothesis was that stock's own pre-release path sets `+0x05C` into
some expected state the real M0+'s handshake depends on, and the open
driver's launch path -- never having heard of this offset -- simply
never does.

**Checked directly, and that specific hypothesis is ruled out, not
confirmed.** Flashed the real stock `build-stage2` (both cores stock),
caught the M33 with a hardware breakpoint at the exact same instruction
(`0x12da0`, independently re-verified in *this* build's own
disassembly to still be the `RAC.CTRL.CPUWAITDIS` SET-alias write, not
just trusted from the earlier capture's notes), and read `RAC+0x05C`
at that boundary: **`0x00000000`** -- both bits already clear, via
both the non-secure alias and the direct address. Since the crashing
M0+ helper's own write to this offset is unconditional (it reads the
old value but never branches on it, then force-clears bit 5 and
force-sets bit 4 regardless), the pre-release value plays no role in
what that helper does -- and the open driver's launch path, which also
never touches this offset, leaves the same architectural-default `0`
in place before release. **The two configurations hand off an
identical value here; `RAC+0x05C` is not the gap.** Recorded per
§7.38's own lesson: a plausible-looking lead is not evidence until
actually measured, and this one's measurement came back negative. The
real cause is still open -- the next candidates are the same kind of
shared-memory fields `docs/hardware-bringup.md` already named and
flagged as unclassified (the FRCRAM indirect-call hook slots at
`0x3844..0x3858`, and the "later-populated words" that differ between
pre-release and working-TX snapshots), not this register.

**This narrows, not widens, §7.57's conclusion: the "blocker is purely
inside FRC's unobservable internal state machine" verdict holds for
the all-open stage-3 configuration (confirmed repeatedly, most recently
in §7.57 itself), but does not hold for stage 2.5 -- there, the real
coprocessor crashes outright, which is observable, software-visible,
and specific to whatever the open M33 driver fails to supply before
release.** The natural next step is decoding exactly what that helper
and its caller read from the pointer they're given -- the same kind of
provenance-first, no-magic-numbers decomposition already applied to
the FRCRAM indirect-call hook slots in `docs/hardware-bringup.md` --
not yet done this session; this section records the crash and its
location, not yet its root cause. Board restored to the ordinary
`build-stage3` open image afterward; stage 2.5's diagnostic build and
proprietary capture remain exactly where they already were, under
`Projects/owo-bg22-proprietary-baselines/`, untouched and uncommitted
to this repository.

### 7.59 Stage 2.5's M0+ lockup root-caused: stock enables five radio-subsystem bus clocks the open M33 driver never does -- fixes the crash, but is not stage 3's `ACTIVETXFCD` blocker

Picked up §7.58's open thread with the comparison it had queued but not
run: stock `build-stage2` versus stage 2.5 at the same pre-release
boundary (stock `0x12da0`, stage 2.5 `0x15f0e` -- each the
`RAC.CTRL.CPUWAITDIS` SET-alias write in its own image; `RAC.CTRL=0x381`
in both). Everything below was measured with `bg22_reset_to_breakpoint`
inside one continuous OpenOCD session per measurement (§7.54), with the
breakpoint removed before every `resume` (§7.11).

**1. `RAC.SR3` is written by stock's M33 after all -- correcting §7.5.2.**
At stock's boundary `SR3` already reads `0x00F00A01` (stage 2.5: `0`).
DWT write-watchpoints on `SR3`'s base, SET and CLR aliases from
`z_prep_c` onward caught every M33 write [F]: a direct store of
`0x00F04800` at `0x12e50` (in `sli_rail_83b3e2e9...`, reached from
`0xe2e4`), then SET bit 0, CLR bit 14, SET bit 9 -- giving exactly
`0x00F00A01` at release -- and later SET bit 15, CLR bit 30, bits 0/6/4/5,
reaching the familiar `0x08F08A31`. §7.5.2 missed this because the store
uses the **RAC base literal plus an immediate `#0x4c` offset**, not an
`0x...4C` literal; its "zero write sites anywhere in M33 RAIL" and the
§7.9 "hardware-latched flag" inference built on it are withdrawn. (Not
re-checked here: whether the M0+ object's `SR3` reads were likewise
undercounted by the same literal-only search.) **But `SR3` is not the
stage-2.5 gap:** poking `0x00F00A01` at stage 2.5's boundary left the M0+
lockup rate unchanged (36 vs. 31 in 6 s).

**2. The M0+'s memory image is not the gap either.** The full 20 KiB at
`0xB0000000` at stage 2.5's boundary is **byte-identical** to
`stage2-pre-release-frcram.bin`, the stock capture the stage-2.5 build
embeds: in both configurations the M0+ starts from the same RAM.

**3. Register-block diff at the boundary** (RAC, FRC, BUFC, AGC, MODEM,
PROTIMER, RFCRC, SYNTH, CMU, EMU, secure SMU; aliased mirrors collapsed).
AGC, PROTIMER, RFCRC and SMU match. Real differences: `RAC.SCRATCH6/7`
(public `RAC_S.SCRATCH6/7`, `+0x3F8/+0x3FC`; stock `0x3FCC`/`8`, open `0`),
`RAC.SR3` (above), `FRC.CTRL/RXCTRL/IEN`, `MODEM.DCCOMP/IRCALCOEFWR1`,
`SYNTH.VCORANGE`, `BUFC` buffer-0 configuration, and **`CMU.CLKEN0/CLKEN1`**.
Decoded against `efr32bg22_cmu.h`, stock has these bus clocks on and the
open driver does not: `CLKEN1.RDSCRATCHPAD`, `RDMAILBOX0`, `RDMAILBOX1`,
`PRORTC`, `IFADCDEBUG` and `CLKEN0.SYSCFG` (TIMER0/BURTC/RTCC also differ,
but those are Zephyr system-timer selection). Independent symptom: SYSCFG
reads fault from the debugger under stage 2.5 but not under stock.

**4. Those clocks are the crash [F].** Setting exactly those six enables
(CMU SET aliases) at stage 2.5's boundary takes the passive 6 s lockup
count from 28-36 to **0** on every run; `SCRATCH6/7` alone (28) or with
`SR3` (36) do not. Baseline: stock shows 0 lockups under the identical
passive observation, so the observation method is not itself causing
them. The real M0+ blob evidently touches the radio mailbox/scratchpad/
PRORTC/SYSCFG blocks early and bus-faults when their clocks are gated,
then double-faults in its own fault path.

**5. Fixing the crash is not sufficient for on-air TX, and does not touch
stage 3.** Stage 2.5 with the clocks set still re-issues the M0+ reset
at the advertising-event rate (~9/s, unchanged with `SCRATCH6/7`/`SR3`
also poked) and a 22 s `scan le` does not see it -- expected, since the
open M33 does not speak the blob's mailbox protocol. The stock image
was seen in the same scan setup immediately (positive control). Stage 3,
rebuilt from current HEAD, is **unchanged** with the same six enables set
from `main`: `RAC.STATUS=0x09400000` (TX), `FRC.STATUS=0x01100200`
(`ACTIVETXFCD` clear) in both control and test, no advertiser in a 22 s
scan. The open sequencer does not use those blocks, so this is consistent.

Net: §7.58's stage-2.5 crash is closed (missing bus clocks, not a
shared-memory/ABI field); §7.57's stage-3 verdict stands. Not yet ported:
whether the open driver should enable `RDMAILBOX0/1`/`RDSCRATCHPAD`
anyway is moot until it uses them. The remaining stage-2.5-only
differences (`SCRATCH6/7`, FRC/MODEM/SYNTH/BUFC fields above) are a
ready-made checklist if stage 2.5 is pushed further toward a TX.
Captures are in `Projects/owo-bg22-proprietary-baselines/release-boundary/`
and `stage2.5-sdk-sequencer/stage2.5-open-pre-release-seqram.bin`,
uncommitted to this repository.

### 7.60 Root cause of the `ACTIVETXFCD` stall: stage 3 transmits with the frequency synthesizer powered off -- starting it by hand unsticks `FRC` and completes the frame

**Correction to §7.42 and §7.43 first.** §7.42 dismissed
`SYNTH.STATUS.INLOCK=0` as "normal on both stacks" by comparing stage 3
*in TX* (`RAC.STATUS` state 9) against stock's ch37 "pre-TXEN" capture,
whose `RAC.STATUS=0x00400000` is state 0 (OFF) -- a mismatched-state
comparison. §7.43 then closed the RAC/MODEM/SYNTH diffs partly as
"per-physical-chip trim", but both captures are from the same chip, so
any difference in those registers is firmware-written; and as "never
touched" test registers (`SCRATCH6/7`), which §7.59 shows stock writes.
Both conclusions are withdrawn.

**Method:** the radio keeps running while the M33 is halted, and the
AHB-AP reads radio registers without halting anything, so each stack
was sampled non-intrusively a few thousand times while advertising,
bucketed by `RAC.STATUS` state [F]:

| in TX (state 9)        | `RAC.SYEN` | `RAC.SYNTHENCTRL` | `SYNTH.STATUS` |
|---|---|---|---|
| stock                  | `0x1FBF`   | `0x100200`        | `0x4010005` (`INLOCK`, `VCOFREQACTIVE`) |
| stage 3 (HEAD build)   | `0`        | `0`               | `0x4` (only `SYNTHREADY`), in 100% of samples |

`RAC.SYEN` (`+0x15C`, public `RAC_S.SYEN`: `SYCHPEN`, `SYENCHPREG`,
`SYENMMDREG`, `SYENVCOBIAS`, `SYENVCOREG`, `SYLODIVEN`, ... ; the
`fn_0x13b0` decomp's "unnamed RAC offset 0x15C") and
`RAC.SYNTHENCTRL` (`+0x094`, `SYNTHSTARTREQ`/`LPFBWSEL`) are written
on stock only by the M0+ -- DWT watchpoints on every M33 alias of
`SYEN`, `SYNTHENCTRL` and `SYNTH.CMD` saw zero M33 writes. Neither the
open driver nor `radio_seq.S` writes any of them, so stage 3 enters
`TX` with every synthesizer analog block powered down and the VCO never
started. Supporting same-chip ch37 diffs that are now real rather than
"trim": `SYNTH.VCOTUNING` `0x81E6`/`0x8400`, `SYNTH.VCORANGE` `0xC8`/`0`,
`SYNTH.LOCNTSTATUS/LOCNTTARGET` `0x1026`/`0` (stock ran the LO-counter
VCO calibration; stage 3 never did), `RAC.TX`, `CHPCTRL`, `SYMMDCTRL`.

**Live intervention on stage 3, stuck in `TX` (M33 halted) [F]:**

| applied (non-secure aliases)                       | after 50 ms |
|---|---|
| nothing (control)                                  | unchanged: `RAC` 9, `FRC.STATUS=0x01100200`, `BUFC0.READ=2` |
| `SYNTH.CMD.SYNTHSTART` only                        | unchanged |
| `SYEN` + `SYNTHENCTRL` only                        | unchanged |
| `SYNTHENCTRL` then `SYNTHSTART`                    | unchanged |
| `SYEN=0x7FBF` (+`SYNTHENCTRL`) then `SYNTHSTART`   | **`FRC.STATUS=0x20` (`ACTIVETXFCD`), `BUFC0.READ=11` (whole packet), `RAC` -> 10 (`TXPD`)**, `SYNTH.STATUS=0x10004` (`VCOFREQACTIVE`) |
| `SYEN=0x7FBF` then `SYNTHSTART`                    | same |
| `SYEN=0x1FBF` (stock's TX value) then `SYNTHSTART` | same |

Reproduced on every attempt. **The minimal trigger is powering the
synthesizer's analog blocks (`RAC.SYEN`) and then issuing
`SYNTH.CMD.SYNTHSTART`: once the VCO runs, the frame coder immediately
drains the staged frame and the RAC leaves `TX`.** [I] The TX
datapath's symbol timing evidently derives from the running LO/VCO, so
with the synthesizer off `FRC` parses the header (§7.20's
`WCNTCMP0` observation) and then never gets a clock to emit it.

Not yet a valid on-air transmission: `INLOCK` stays 0 in these pokes
(`VCORANGE` never calibrated, `CAPCALSTART` alone without stock's
calibration setup did not produce a result), so the frequency is
uncontrolled. Next: port stock's M0+ synthesizer power-up and VCO
calibration into `radio_seq.S`'s TX warm-up, with every value derived
from public field names, then verify `INLOCK=1` and an over-the-air
scan.

### 7.61 Ported stock's TXWARM synthesizer power-up and VCO calibration into `radio_seq.S`: the PLL now locks and `FRC` sends the whole frame on real hardware

**How stock does it [F].** AP3 watchpoints on `RAC.SYEN`'s SET/CLR
aliases (with the M0+ target examined only after `EMU.CTRL.EM2DBGEN` is
set from the M33, so the debug link survives EM2) land in stock's IRQ8
handler. `trace_stores.py` then single-stepped one complete stock IRQ8
invocation (1656 steps, ending in the `SEQIF.STATETXWARM` ack) and
recorded 149 MMIO stores. Every one decodes to public `RAC_S`/`SYNTH_S`/
`FRC_S` field names except the RF mailbox and two `PROTIMER` alias
writes. The handler is the whole analog TX bring-up, in order:

1. retime/MMD resets, `SYCAL` clear, `VCORANGE` thresholds,
   `SYNTH.CMD` = `SYNTHSTOP|DISABLEIF`, `SYNTHENCTRL.LPFBWSEL`,
   `SYMMDCTRL.SYMMDMODE=2`, `RX.SYCHPBIASTRIMBUF`, `CHPCTRL` tristate
   mode, `SYTRIM0`/`SYTRIM1`/`VCOCTRL` trims (each a named field set);
2. `SYNTHSTARTREQ`, staged `SYEN` power-up (charge pump/MMD/VCO bias,
   then VCO PFET, then VCO regulator and LO-divider LDO, then LO divider)
   with ~1/2/2 us waits, `RADIOEN.PRESTB100UDIS`, `CHPLDOVREFTRIM=2`,
   `VCOSTARTUP` pulse, `VCDACCTRL` enables, ~4 us;
3. `VCORANGE.LOAD|THROUGH`, `SYNTH.CMD.CAPCALSTART`, `LOCNTCTRL.ENABLE`,
   then an 11-bit successive-approximation search of
   `VCOTUNING.VCOTUNING` (`VCAPSEL` fixed at 16): per trial bit it ORs in
   a few low bits, programs `LOCNTCTRL.NUMCYCLE` (2..5, finer bits count
   longer), runs the LO counter, and keeps the bit when
   `LOCNTSTATUS.LOCOUNT` exceeds `LOCNTTARGET.TARGET`. `LOCNTTARGET` is
   **read-only** in the public model -- hardware derives it from the
   programmed channel, which is why it read `0` on stage 3 (never
   enabled) and differs per channel on stock. Each measurement is bounded
   to 2047 `BUSY` polls, and `RAC.CTRL.FORCEDISABLE` aborts the search;
   an aborted search skips the start;
4. `SYNTH.CMD.SYNTHSTART`, counter off, `VCDACEN`/`LPFQSEN` off,
   `VCORANGE` override off, `SYTRIM1.SYLODIVLDOTRIMNDIO` 3 -> 1;
5. then the PA bring-up (`RAC.CMD.PAENSET`, `RAC.TX` enables,
   `PAENCTRL`, `PACTRL` slice-reset pulse, `PARAMP`, timed ramp) -- **not
   yet ported**;
6. the `STATETXWARM` ack already in `radio_seq.S`.

The waits use stock's busy-wait helper (`fn_0x21d8`, already
decompiled): `n * 195 - 111`, minus 30 per pass. Stock skips the
calibration only when `RAC.SR3` bit 6 is set or a per-channel cached
result exists; neither is reproduced (bit 6 is clear on both stacks, and
the cache is an optimisation).

**Ported (steps 1-4)** as `efr32_seq_synth_start` /
`efr32_seq_vco_calibrate`, called from the TXWARM path just before the
ack, with every value written as named public fields and the two per-bit
schedule columns read off the hardware trace (not the blob). The
calibration result goes to diagnostic word `0x4FBC`.

**Result on real hardware (fresh HEAD stage-3 build) [F]:** sampled
non-intrusively while advertising, `SYNTH.STATUS` now reads `0x4000005`/
`0x4010005` (**`INLOCK=1`**, matching stock in TX), the calibration
converges to `VCOTUNING=0x8338` (stock: `0x8335` on its channel) against
a hardware `LOCNTTARGET=0xFA3`, `FRC.STATUS=0x20` (`ACTIVETXFCD`), and
`BUFC` TX read = write = 11 (the whole frame). The RAC now passes
TXWARM -> TX -> **TXPD** and then sits in TXPD (state 10) for 99% of
samples: the stall that held every previous section in TX is gone. QEMU:
`efr32bg22-test` 79/79 and `efr32bg22-rf-test` 17/17 pass.

**Not yet on air**, for two now-specific reasons: the PA bring-up (step 5)
is not ported, so nothing drives the antenna; and nothing on the open
M0+ services the TX power-down transition, so the RAC parks in TXPD
instead of returning to OFF for the next event. Both are next.

**QEMU follow-up.** The model used to complete a released sequencer's TX
whether or not the synthesizer ran, which is how simulation hid this
stall. `vendor/qemu` now defers that completion until `SYNTH.CMD.SYNTHSTART`
has run with `RAC.SYEN`'s VCO supplies on (and drains a parked frame as
soon as it does), reports `SYNTH.STATUS.INLOCK` only while running, and
keeps `SYNTH.STATUS`/`LOCNTSTATUS`/`LOCNTTARGET` read-only. New qtest
`seq-tx-waits-for-synthesizer` fails on the old model; suites 80/80 and
17/17. Still unmodeled: TX ending in TXPD rather than OFF, the LO
counter's measured values, and §7.59's extra CMU gates.

### 7.62 TXPD servicing and the 6 dBm PA path ported: **stage 3 transmits on air**

**Stock's TXPD handler [F].** `SEQIF` state events reach the M0+ as
`IRQn <-> bit n+16` (IRQ8 TXWARM, IRQ9 TX), so `STATETXPD` (bit 26) is
IRQ10. One stock IRQ10 invocation, traced store-by-store the same way as
§7.61's IRQ8, mirrors the bring-up: PA ramp-down (`PATRIM3` bleed,
`RAC.TX` bleed/ramp-clock bits, `PAENCTRL.PARAMP` off, then a wait on
`MODEM.SEQIF.TXRAMPDONE` -- skipped when `MODEM.RAMPCTRL.RAMPDIS` -- ~1 us,
`TX6DBMENPAOUT`/`RAMPCLK` off, ~4 us, core/predriver off, ~1 us, regulators
and `ENPAPOWER`/`ENPASELSLICE` off, `PAENCTRL`/`RFPATHEN`/`PATRIM3`/
`SYLOEN`/`LNAMIXEN`/`RX`/`AGCOVERWRITE` restored, `RAC.CMD.PAENCLEAR`),
then synthesizer power-down (`SYNTH.CMD.SYNTHSTOP`, staged `SYEN` clears
with ~1 us, `RADIOEN` off, `VCDACCTRL` off, `SYNTHENCTRL` clears), then the
`STATETXPD` ack. The open image never enabled IRQ10, which is why §7.61's
build parked in TXPD. Stock's IRQ8 also opens with
`RADIOEN.SET = PREEN|RFBIASEN`, which §7.61 had missed.

**Ported** (`efr32_seq_synth_stop`, `efr32_seq_pa_start`,
`efr32_seq_pa_stop`; IRQ10 enabled at priority 1 like IRQ8/IRQ9), every
value named from the public `RAC_S`/`MODEM_S` fields: the 6 dBm PA
bring-up from §7.61's step 5 (`APC` limit, `PAENSET`, `ENPAPOWER|
ENPASELSLICE`, `INVRAMPCLK`, `SYLODIVTLO6DBM2G4EN`, LNA/mixer held at
manual maximum attenuation, regulator -> slice-reset pulse -> predriver ->
core -> output -> ramp clock with stock's 6/2/8/3/1 us waits, `PARAMP`,
bleeds off), and the TXPD sequence above with the ramp-done wait bounded.
Not ported: stock's RAIL bookkeeping in the same handlers (`SR0`/`SR1`
words, RF mailbox, `PROTIMER` alias writes, `RAC.IF.SEQ`, the
`FRC.FCD` buffer reselect) -- none is needed for the result below.

**Result [F].** The RAC now cycles TXWARM -> TX -> TXPD -> OFF every
event; VCO calibration lands on `0x8335`, stock's own value. On air, with
a control in the same boot: the board's identity (read from `bt_dev`
without halting) was `EE:55:E1:6A:AF:47`; a 15 s `bluetoothctl scan le`
saw it at **-36 dBm** while running and **zero times** with the M33
halted. An earlier boot's identity, `FC:F5:7F:1E:F0:B4`, was likewise
seen at -38 dBm. **The all-open stage-3 configuration -- open M33 driver,
open M0+ sequencer -- transmits real BLE advertisements.** Not yet
checked: receive (scan requests/connections), the remaining advertising
channels' calibration values, and whether the synth-only build of §7.61
radiated anything without the PA.

**QEMU follow-up to §7.62.** `vendor/qemu` now ends a released
sequencer's TX in TXPD, raises `STATETXPD` on the sequencer's IRQ10, and
reaches OFF only on its acknowledgement (qtest
`seq-tx-waits-for-synthesizer` extended; suites 80/80 and 17/17). Running
the current stage-3 image under QEMU (`-M efr32bg22,flash=...`) gives 187
complete OFF -> TXWARM -> TX -> TXPD -> OFF cycles in 25 s, the same
sequence measured on silicon. The model passes through SHUTDOWN (13)
before TXWARM on each launch; that step was never seen in the sparse
hardware sampling and is unconfirmed either way.

### 7.63 Receive: what stage 3 is missing, mapped on stock hardware (in progress)

**Superseded by §7.64:** stage 3 receives; the RX `SYTRIM0` port here was wrong.

Stage 3 transmits (§7.62) but never listens: on silicon and in QEMU its RAC
only ever shows OFF/TXWARM/TX/TXPD. Two separate gaps [F]:

1. **No TX->RX turnaround request.** Sampled on running stock, `RAC.STATUS`
   during TX reads `0x89400002`: `RXENS=1` with `RXMASK` bit 1, and
   `RAC.RXENSRCEN` mostly `0x2` (`SWRXEN`) -- an RX request is already
   pending while the frame goes out, and the RAC then passes TX ->
   TX2RX (11) -> RXSEARCH (2) by itself. Stage 3's TX reads `0x09400000`
   (nothing requested). `radio.c`'s `radio_switch_complete_and_rx/tx` are
   `EFR32_RADIO_TODO` on hardware (only a QEMU software-tIFS path exists),
   although `radio_rx_enable()` already writes exactly `RXENSRCEN.SWRXEN`.
2. **No M0+ RX analog bring-up.** Stock's M0+ enables IRQ0-15 and more
   (`ISER0=0x089BFFFF`; the open image enables 8/9/10). An AP3 watchpoint
   on `RAC.SYCAL`'s CLR alias, resumed until it fired outside IRQ8, lands
   in IRQ11 (`STATETX2RX`). From there to its ack the handler stores:
   AGC `ADCATTENCODE`, manual LNA/mixer/PGA overrides, `RFPATHEN.LNAMIXTRSW`,
   an **IF PGA DC-offset calibration** (`IFPGACTRL.DCCALON`/`DCRSTEN`,
   successive-approximation writes to `RAC.PGACAL` bracketed by
   `MODEM.DCCOMP.DCCOMPFREEZE`), `MODEM.DCCOMP` enabled, overrides
   released, `FRC.SEQIF` clear, `STATETX2RX` ack. The part *before* the
   watchpoint (the synthesizer retune for RX: an earlier watchpoint saw
   IRQ11 code write `SYEN=0x1FBF`) is not yet captured. IRQ2's
   (`STATERXSEARCH`) handler is RX bookkeeping only (`WCNTCMP0`=0xFF,
   `MODEM.IF` frame-detect clears, `FRC.SEQIF` RX clears, ack).

**Methodology notes.** (a) With the dual-core server's current target set
to the M0+, `read_memory` of a `0xB8...` non-secure alias reads 0 -- the
M0+ bus does not decode it; use `0xA8...`. A "stock radio stopped" reading
in this session was exactly that misread, not a real stop. (b) Hardware
breakpoints on the IRQ11/IRQ1 entries never reported a usable halt even
with `VTOR=0` and the vector table confirmed live; a watchpoint on a store
inside the handler does work. Halting the M0+ inside RX-side handlers can
also leave its registers unreadable afterwards.

Next: capture IRQ11's head (synth retune) and the RXWARM/RXFRAME/RXPD
handlers, port them as for TX, and arm `SWRXEN` from the hardware
`radio_switch_complete_and_rx()` while TX is in progress.

**IRQ11 captured in full [F].** A watchpoint on `RAC.SR1`'s SET alias (the
first store of `fn_0x894`, which every stock handler calls first), filtered
to exception 27, halts IRQ11 within a few instructions of entry; tracing
from there to the `STATETX2RX` ack (149 stores) gives the whole TX->RX
turnaround, in order: (1) the same PA ramp-down as TXPD (§7.62); (2) the
**M0+ itself sets `RAC.RXENSRCEN.SWRXEN`**; (3) the synthesizer retune for
RX through the same start routine as TX with RX parameters --
`VCORANGE` HIGHTHRESH=1/LOWTHRESH=3, `SYNTH.CMD` = `SYNTHSTOP|ENABLEIF`
(IF on for RX), `SYNTHENCTRL.LPFBWSEL` cleared, `SYMMDMODE=4`,
`RAC.RX.SYPFDCHPLPEN|SYPFDFPWEN`, `SYEN.SYCHPLPEN` set, `SYTRIM0` with
`SYCHPBIAS=1` -- while the `SYEN` supplies stay on (no staged power-up),
then a fresh LO-counter VCO calibration and `SYNTHSTART`; (4) RX
front-end power-up: `SYLOEN` RX LO dividers, `SYTRIM1.SYLODIVRLOADCCLKSEL`,
`RAC.CMD.LNAENSET`, `LNAMIXEN.LNAMIXENLDO`, `IFADCDBG` ADC/refbuf/clock
enables, RX LDOs (`IFADCENLDOSERIES|IFADCENLDOSHUNT|LNAMIXLDOLOWCUR|
PGAENLDO`), `RFPATHEN.LNAMIXEN|LNAMIXRFATTDCEN`, `PGACTRL` enables,
`SYCAL` clear; (5) the AGC/PGA DC-offset calibration listed above; (6) the
ack. Saved with the other traces outside the repo.

**Hardware turnaround wired up (M33 side).** `radio.c` now turns the LLL's
`radio_switch_complete_and_rx()` into `RXENSRCEN.SWRXEN` right after `TXEN`,
arms the RX packet stage once the LLL has consumed the TX completion and
configured HCTO, and bounds the window with the same BURTC alarm the QEMU
path uses (expiry calls `radio_disable()` unless a completion owns cleanup).
`radio_disable()` now withdraws `SWRXEN` before RXDIS: with the source still
asserted, RXDIS left the RAC in RXSEARCH and the next `radio_tx_enable()`
waited for idle forever. Result on hardware [F]: the state mix matches
stock -- TXWARM, TX, TX2RX, RXSEARCH, RXPD, OFF every event -- the
open sequencer's IRQ11 port runs (PGA DC calibration lands on stock's
`0x1E23221D`), and advertising continues (134 TXDONE in one run, still seen
on air at -38 dBm).

**Receive does not decode yet [F].** With the host actively scanning the
board's current address, 134 TXDONE and **0 RXDONE**; no scan response
seen. `MODEM.IF` (sticky, never cleared by the open stack) shows only TX
bits -- `RXPREDET`/`RXFRAMEDET*`/`RXTIMDET` have never been set -- while
`RAC.STATUS` in RXSEARCH carries `RXENS|DEMODENS` like stock's. The
demodulator never detects a packet, so the gap is demodulator/IF
configuration, not the turnaround or framing. Next: diff the radio blocks
during RXSEARCH, stock vs stage 3 (stage 3 can be held in RX by halting the
M33 at `radio_tmr_hcto_configure`; stock via a watchpoint inside its IRQ2).
RX->TX (scan response) remains unimplemented on hardware.

**QEMU: model fix, and a known regression in the simulated firmware run.**
`vendor/qemu` now latches `MODEM.IF`/`SEQIF.TXRAMPDONE` when
`RAC.PAENCTRL.PARAMP` changes (hardware evidence above); without it the
open sequencer's bounded ramp-down wait ran to its limit on every TXPD in
simulation. Qtests 80/80 and 17/17.

**Open regression (QEMU only, hardware unaffected):** the QEMU-config
firmware (`tests/radio-port` with `prj.conf`, board `bg22_ek4108a`) ran 486
OFF -> TXWARM -> TX -> TXPD -> OFF -> RX cycles with the sequencer at
`9713ed9`, but with the PA port (`9df1c9c`) it completes one cycle and stops,
and with the RX turnaround (`9ce39d3`) it dies before the first TXWARM. The
current `radio.c` is not the cause (`9713ed9`'s sequencer plus current
`radio.c` runs 501 cycles), nor are the added RX IRQ enables (reverting
only those still fails). The failure is an `LL_ASSERT` in
`ull_periph_setup()` (`ull_peripheral.c:467`, caught at `z_fatal_error` via
the QEMU gdbstub): with no peer attached, the simulated advertiser accepts
something as a CONNECT_IND. With the default 448-byte launch stack the
assert path itself overflows that thread's stack, which first presented as
a "stack overflow" with PC 0. [I] Suspected: the QEMU RF medium offering a
still-active transmission to a receiver entering RXSEARCH, made reachable
by the longer sequencer handlers -- unconfirmed. Next step for whoever
continues: trace `efr32bg22_rf_*` medium events and the delivered RX bytes
around the first RX window.

**Resolved (SS7.69):** the `ull_periph_setup()` attribution and the RF-medium
suspicion above were both wrong. The panic was the radio deadline check: the
first launch ran late behind a per-event sequencer reinstall, and every
TXPD then stalled ~15.7 ms in the model's TXRAMPDONE handling.

### 7.64 Stage 3 receives: the demodulator was correlating against an inverted access address

**Diff in RXSEARCH [F].** Stage 3 held in RXSEARCH by halting the M33 at
`radio_tmr_hcto_configure`; stock caught in RXSEARCH by an AP3 watchpoint on
`MODEM.IF`'s CLR alias filtered to IRQ2, blocks dumped over the M0+ bus. Both
read `SYNTH.STATUS=0x6` (`INLOCK=0` is normal in RX here -- not a lead).
The decisive difference: `MODEM.VTCORRCFG0` stock `0xB5BEC8B8`, stage 3
`0x4A413747` -- exact complements. Stock RAIL rewrites it at runtime
(M33 watchpoints: CLR-alias all-ones then SET-alias the pattern, at setup
and around every event), and the live value is the advertising access
address in on-air bit order rotated right by one:
`ror(bitrev(0x8E89BED6), 1) = 0xB5BEC8B8`. The PHY table's `0x4A413747`
(and §7.43's "access-address-shaped bytes") is its complement. RAIL also
lowers `VTCORRCFG1.EXPECTHT` from the table's 5 to 2 (observed; derivation
unknown). The same diff showed §7.63's RX `SYTRIM0` port wrong: stock's RX
value is `0x64E29` (`SYCHPBIAS=1`, *no* `SYCHPSRCEN`).

**Fixed:** `radio_aa_set()` now writes `VTCORRCFG0 = ror(bitrev(aa), 1)` and
`EXPECTHT = 2`; `radio_seq.S`'s RX `SYTRIM0` matches stock.

**Result [F]:** the first RXDONEs on stage 3; `MODEM.IF` gains RX frame
detection; received buffers hold well-formed ADV_INDs from neighbouring
devices (e.g. header `60 1F`, AdvA, then a `02 01 xx` Flags AD structure).

**A memory-corruption bug this exposed, fixed:** at the first RXDONE, BUFC
RX channel 1 was still at reset (`CTRL=0`, `ADDR=0x20000000`) -- the
hardware turnaround starts receiving before the LLL calls
`radio_pkt_rx_set()` -- so FRC wrote the frame over the start of RAM
(`bt_dev`; a later read showed its address type as `0xC3`). The channel is
now programmed in `radio_setup()` and before `TXEN` whenever a turnaround
will follow, and `radio_pkt_rx_set()` does not re-CLEAR it then (that
could discard a frame already arriving). Verified: `ADDR` is
`efr32_rx_fifo` at every RXDONE and `bt_dev` stays intact.

**Still missing: RX->TX.** The LLL arms RX->TX for every connectable
advertising event; with no hardware path the first accepted SCAN_REQ left
it waiting for a TX completion forever and advertising stopped. Until
RX->TX exists, an RX completion with RX->TX requested ends the event after
1 ms through the same alarm/`radio_disable()` path. Free-running with the
host actively scanning: advertising continues (seen at -37 dBm over 30 s),
no console errors. (A claim originally written here, that stock RAIL writes
`RAC.CMD.TXAFTERFRAME`, was a misread of a register left over after a
watchpoint halt; the store at that site writes RXDIS. Withdrawn -- see
§7.65.)

### 7.65 RX->TX turnaround works mechanically; scan responses are blocked by bit errors on received SCAN_REQs

**Superseded by §7.66:** the bit errors were stale RX-ring data, not reception errors.

**Stock handlers [F].** IRQ4 (`STATERXPD`, captured by an AP3 watchpoint
inside it) is the RF front-end power-down -- stock `fn_0xc02`, now named:
`RAC.RX` CLR of the IF-ADC LDOs/RF peak detector/LNA-mixer current/PGA
LDO/PFD bits, `PGACTRL` CLR of the PGA enables, `SYLOEN` CLR of the RX LO
dividers, `RFPATHEN=0`, `IFADCDBG` CLR, `LNAMIXEN` CLR,
`RAC.CMD.LNAENCLEAR` -- then the synthesizer power-down shared with TXPD,
then the ack. IRQ7 (`STATERX2TX`) was captured three ways: a "decline"
branch (`SWRXEN` CLR, `RAC.CMD.TXDIS`, ack) and a responding branch whose
TX retune is §7.61's TXWARM configuration block plus synth-lock with the
supplies left on -- but every responding trace aborts after the first
calibration trial on `RAC.CTRL.FORCEDISABLE`: under single-stepping RAIL's
~150 us response deadline always expires and RAIL cancels, so a complete
responding IRQ7 cannot be traced. Stock's `RAC.STATUS` during RXFRAME is
`0x63400000`: `TXENS` set, `RXENS` clear.

**Ported.** `radio_seq.S`: RXPD = front-end power-down + synthesizer
power-down; RXFRAME (IRQ3), when the M33 marked the window "pending" in a
new shared word (`0x4FA4`, 0 = idle, 1 respond, 2 cancel, 3 pending),
withdraws `SWRXEN` and issues `TXEN`; RX2TX (IRQ7) waits up to ~120 us for
the M33's decision -- respond: front-end down, TX configuration,
synth-lock, PA; cancel or timeout: `TXDIS`. `radio.c`: the RX window is
marked pending when the LLL armed RX->TX; `radio_pkt_tx_set()` for an
armed window publishes the response to BUFC, labels the TX completion,
retires the 1 ms guard and writes "respond"; `radio_disable()` writes
"cancel". The hardware HCTO now only ends a window still in RXSEARCH: it is
a header timeout, and aborting frames in progress had cancelled every
SCAN_REQ. Diagnostic per-IRQ entry counters live at `0x4F80` (bytes,
IRQ0-23; 24 RXFRAME-TXEN; 25-27 respond/cancel/timeout).

Measured along the way [F]: with `SWRXEN` still asserted, the RXFRAME TXEN
latched (`TXENS`) but the RAC returned to RX at frame end; withdrawing it
first gives stock's `0x63400000` and RX2TX on every received frame.

**Result [F]:** per 5 s, TXWARM = TX (no stray transmissions), RX2TX = RXFRAME;
outcomes ~half "cancel" (the LLL declined other devices' frames) and half
"timeout", 0 "respond" -- unchanged with a 600 us wait, so not latency.
The RX FIFO shows why: the host's SCAN_REQs do arrive (header `0xC3`,
length 12), but their AdvA reads `FA:9E:57:19:BD:33` against the board's
`FA:99:D7:19:BD:3F` -- the same address with several flipped bits -- so
they fail CRC and are correctly ignored. Neighbours' advertisements
decoded cleanly, while this scanner is close (about -35 dBm): errors on a
strong signal point at gain control/saturation. Open RX-configuration
differences from stock's RXSEARCH capture (§7.64): `AGC.CTRL0`
`0x000A72F5` vs `0x00093AF5`, `MODEM.IRCALCOEFWR1` 0 vs `0x80C080D1`,
`MODEM.BLEIQDSA` 0 vs `0x13C4`, `FRC.RXCTRL`, the retime controls. Next.

**Checked and negative:** poking stock's `AGC.CTRL0` (`0x00093AF5`) and
`MODEM.IRCALCOEFWR1` (`0x80C080D1`) into the running board (both read back
and persist across events) leaves the outcome unchanged over 6 s windows --
0 respond, roughly even decline/timeout. `MODEM.BLEIQDSA` reads back 0 after
a write in this context, so it could not be tested this way. The bit-error
cause remains open.

### 7.66 Received frames now reach the LLL with a real CRC result; scan responses are sent but late

**Correction to §7.65.** Its "bit errors on received SCAN_REQs" were a
measurement artifact: the bytes came from the head of the 512-octet RX ring,
which held stale data. Two driver gaps were the real cause [F]:

1. **No hardware RX handoff.** `efr32_packet_stage_rx_complete()` was only
   wired to the QEMU descriptor seam, so on silicon the LLL parsed whatever
   was in its own buffer. Measured FIFO layout (§7.64): header, length, then
   exactly `length` payload octets at READOFFSET, plus any trailer
   `TRAILRXDATA` selects. `isr_radio()` now hands that frame to the LLL at
   RXDONE and consumes it. A delivered SCAN_REQ's AdvA then matched the
   board's identity exactly.
2. **`FRC.TRAILRXDATA` is configuration, not status** (public model:
   read-write, reset 0; fields RSSI, CRCOK, PROTIMER/RTC stamps). The driver
   never set it and read it back as the CRC result, so every frame looked
   CRC-failed. Stock runs `TRAILRXDATA=0x1B` and `RXCTRL=0x262`
   (`ACCEPTCRCERRORS`, which ours lacked: without it FRC silently drops a
   failed frame, no RXDONE). With `TRAILRXDATA.CRCOK` FRC appends one octet;
   [I] its bit 7 is the CRC result -- set on every frame while failures
   were dropped, clear on a frame received with a deliberately corrupted
   `RFCRC.INIT` (set while the M33 was halted after its own CRC setup), low
   bits varying with signal. The hardware path now sets both, reads bit 7
   per frame, and `radio_crc_is_valid()` returns it; the QEMU descriptor
   path is unchanged.

**Result [F]:** CRC-good and CRC-failed frames are both delivered and
classified; the LLL now accepts SCAN_REQs addressed to the board and stages
the scan response -- the open sequencer's RX2TX handler records "respond"
outcomes and TX entries exceed TXWARM entries by the same count.

**Not yet received by the host: timing.** RAC.STIMER runs at ~4.8 ticks/us
(against BURTC's 32.768 kHz). Timestamped from IRQ7 entry: the M33's
decision arrives after ~132 us (the LLL runs in a thread after RXDONE),
synthesizer retune + VCO calibration take ~48 us, PA bring-up ~28 us --
~210 us before TX, against BLE's 150 +/- 2 us T_IFS, and the TX start is not
hardware-timed at all. Many "timeout" outcomes are decisions arriving just
past the M0+'s 120 us wait.

### 7.67 RX->TX: TX retune moved ahead of the decision, with cached calibration

The RAC holds RX2TX until the sequencer acknowledges (a TXDIS issued after
the decision wait prevented the transmission), so the open RX2TX handler
now powers the receiver down and retunes the synthesizer for TX *while* the
M33 decides, and brings the PA up only on "respond" -- nothing radiates for
a declined frame. The retune reuses this event's TXWARM VCO calibration
(same channel; `0x4FA0`), as stock's per-channel cache does, instead of
recalibrating; `efr32_seq_synth_lock` takes a mode (calibrate; calibrate and
cache as TX tuning; use cached).

**Measured [F]** (RAC.STIMER, ~4.8 ticks/us, from IRQ7 entry): synthesizer
locked at ~17 us (was ~48 us after the decision), decision ~154 us, PA up
~182 us. Per 5 s: 7 "respond", 14 "cancel", **0 timeouts** (was 3/10/17).
The host still does not receive the scan response: the remaining excess
is the M33's decision latency -- the LLL runs from a job thread after
RXDONE ("do not run an LLL state machine on the hardware interrupt
stack") -- and the TX start is still not hardware-timed to T_IFS. Both need
an architecture decision (running the RX callback at interrupt level, or
letting the sequencer pre-qualify a SCAN_REQ) and finding stock's
hardware T_IFS timing mechanism.

### 7.68 Scan responses received by the host; the M33 delay was a BURTC arm on the critical path

**Where the ~132 us went [F].** `RAC.STIMER` stamps (bring-up builds only,
spare SEQRAM `0x4F50..`) on one consistent SCAN_REQ: RXDONE interrupt at
~2 us; the completion job starts the LLL's RX callback at ~23 us -- the
deadline scheduler releases it promptly; then inside the LLL's `isr_rx`,
`lll_isr_status_reset()` alone took ~119 us. In it, `radio_status_reset()`
armed §7.64's interim "unserved response" guard through
`cntr_qemu_tifs_arm_after_us()`, i.e. a BURTC compare (LF-domain
synchronisation and a 64-bit division). The guard is obsolete now that
RX->TX exists (the LLL disables declined frames; the sequencer bounds its
own wait) and is removed. Result: response published ~39 us after the
frame end (was ~155), sequencer sees "respond" at ~41 us, PA up at ~69 us.

**TX start.** `RAC.STIMER` restarts on every RAC state change, so TX-entry
time was measured with the M0+'s free-running SysTick (~38.6 MHz,
calibrated against STIMER within IRQ7): the RAC enters TX ~3 us after the
sequencer acknowledges STATERX2TX, ~33 us after PA-up with a 30 us hold --
the acknowledgement controls the TX start. The sequencer now holds it until
`STIMER` reaches 140 us after the frame end. [I] The RX- and TX-chain delays
that separate this from the on-air T_IFS are not measured.

**On air [F].** `tests/radio-port` advertised with no scan-response data, so
a host could never learn its name -- every earlier "no name" result for
stage 3 was uninformative. With a complete-name scan response, an active
`bluetoothctl scan le` reports `Name: efr32-radio-port`: the SCAN_REQ was
received, accepted by the LLL and answered with a decodable SCAN_RSP. A
negative control showed the host also accepts a response held 1-2 ms late,
so this scanner cannot verify T_IFS compliance; that needs a stricter
scanner or an SDR.

### 7.69 The M0+ now runs across events; RAC holds transitions on unacknowledged state events

**The QEMU regression [F].** Two independent causes, both found with the
`-icount` firmware run's RAC state trace:

1. `radio_reset()` runs in every LLL prepare, and it re-copied, byte-verified
   and rebooted the private M0+ image each time. Its cost grows with the
   image (3446 bytes now); the PA port (`9df1c9c`) added ~450 bytes and pushed
   the first launch past its start-by deadline. Stock RAIL loads its image
   once at initialization. The open HAL now installs and starts the M0+ from
   `radio_setup()`, and later `radio_reset()` calls only quiesce the RAC.
2. The model latched `MODEM.SEQIF.TXRAMPDONE` at the `RAC.PAENCTRL.PARAMP`
   write. The TXPD ramp-down (SS7.62), like stock's, drops PARAMP, *then*
   clears TXRAMPDONE, then polls it, so the clear erased the model's instant
   completion and every TXPD ran the bounded wait to its limit (~15.7 ms of
   virtual time). The model now leaves the ramp pending at the PARAMP change
   and latches it at the first later read of `MODEM.IF`/`SEQIF`. It still has
   no measured ramp duration.

The printed lateness in `efr32_radio_deadline_check` overflowed 32 bits
("7 ns" for ~87 us); it is now computed in 64 bits.

**Silicon: RAC waits for every enabled state event [F].** Stopping the
per-event reboot broke hardware after the first advertising event. The
RAC sat in SHUTDOWN with a TXEN outstanding, `SEQIF=0x20200007` (STATESHUTDOWN
bit 29 and RX2RX bit 21 set), `SEQIEN=0x3FFF0000`, and the M0+ idle in WFI
with IRQ0/5/13 pending but not enabled (`ISER0=0xF9E`). The open reset
handler enabled all 14 state events in SEQIEN but gave only 9 of them an
NVIC enable. Clearing only bit 29 from the debugger released the held TXEN
at once: the M0+ counters show TXWARM, TX, TX2RX, RXSEARCH and RXFRAME
each running once. The RAC then parked in RX2RX (state 5) on the still-set
bit 21. So the RAC holds a transition while any SEQIEN-enabled state event
is uncleared. The per-event reboot's own SEQIF clear had been hiding this.
The open image now NVIC-enables every state event it enables in SEQIEN (stock
enables IRQ0-15, SS7.63). The common tail acknowledges those that have no
handler. Hardware with the M0+ running continuously: 150 TX per 5 s and the
host receives the scan-response name, as before.

**QEMU model updated.** Every RAC state, SHUTDOWN included, latches
`SEQIF` bit 16 + state and drives M0+ IRQ *state* (previously only IRQ8-10
were wired, and SHUTDOWN latched nothing). With the sequencer released, a
TXEN and the TXWARM/TX/TXPD handshakes are held while any SEQIEN-enabled
state event is pending. New qtests: `pa-ramp-survives-clear-after-paramp` and
`seq-unacked-state-event-holds-txen` (82/82, RF suite 17/17). The model now
reproduces the silicon failure: the firmware with the old 9-IRQ enable mask
stops after one TX. With the fix it runs 153 TX cycles in 5 s of virtual
time. RX2RX itself is still not modelled.

### 7.70 QEMU models the receive side and both turnarounds; the hardware-configuration firmware answers SCAN_REQs in simulation

Until now the QEMU model knew only OFF, RXSEARCH, TXWARM, TX, TXPD and
SHUTDOWN, so the open M0+'s TX2RX, RXFRAME, RX2TX and RXPD handlers never ran
in simulation, and the QEMU firmware build relied on two simulator-only seams
(`CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR`, `CONFIG_EFR32_RADIO_QEMU_TIFS`).
`vendor/qemu` `92d87db5fe` adds, for images that release the sequencer, the
behaviour measured in §7.63-7.69:

- TX ending with `RXENSRCEN.SWRXEN` asserted goes TX2RX (11), and RXSEARCH on
  the `STATETX2RX` acknowledgement; SWRXEN set while TXWARM/TX is a pending
  request, not an immediate RXSEARCH [F, §7.63 state mix].
- An offered frame enters RXFRAME (3). At frame end FRC writes header,
  length, payload and, with `TRAILRXDATA.CRCOK`, one trailer octet with bit 7
  set to BUFC channel 1, raises RXDONE, and the RAC goes RX2TX (7) when TXEN
  was latched during RXFRAME with SWRXEN withdrawn [F, §7.65], RX2RX (5)
  when SWRXEN is still asserted [F that it returns to RX, §7.65; RX2RX as the
  route [I] from §7.69], otherwise RXPD (4) [I].
- RX2TX is held until acknowledged and TX starts 3 us later, with no TXWARM
  [F, §7.66 TX > TXWARM counts, §7.68].
- RXDIS in a receive state goes RXPD, then OFF on acknowledgement [F, §7.63];
  with SWRXEN still asserted it is ignored [F outcome, §7.63; mechanism
  unknown].
- Frame-end and TX-end transitions, and RXFRAME entry, wait for pending
  SEQIEN-enabled events like TXEN does [I generalisation of §7.69].
- `RAC.STIMER` counts 4.8 ticks/us from the last state change, 16 bits
  [F rate §7.66, restart §7.68; width from the public register model]. The
  open RX2TX handler polls it, so without it the handler never returned.
- `RAC.STATUS`: RXENS and RXMASK bit 1 follow SWRXEN, TXENS reports the
  RXFRAME latch, SEQACTIVE reads 0 in RXFRAME [F, the four samples in
  §7.21/7.63/7.65].

Synthetic or inferred in the model, listed so they are not mistaken for
measurements: the received frame lasts 150 us, the same synthetic value as
the model's TX (no airtime model); the trailer's signal-dependent low bits
are 0 and the CRC bit is always set (the medium has no bit errors); a
receiver that enters RXSEARCH while a peer is mid-frame still receives that
frame (pre-existing medium rule); OFF + SWRXEN still enters RXSEARCH
directly, without RXWARM; RXDIS/TXDIS in RXPD/TXPD and TXDIS in a receive
state are ignored; TXDIS/RXDIS in other busy states still end in OFF at once
(pre-existing). Guests that configure the QEMU-only descriptor ring (BUFC
channel 2) keep the old instantaneous handoff.

**A seam bug this exposed [F, QEMU].** With the QEMU-configuration
advertiser, the first SCAN_RSP ended in TX2RX and the event never finished:
the QEMU_TIFS RX->TX path issued RXDIS without withdrawing SWRXEN, which on
silicon does not stop receive (§7.63) and leaves an RX request on the next
TX. `radio.c` now clears SWRXEN before that RXDIS, as `radio_disable()`
already does. The hardware path is unaffected.

**Results in QEMU [F, QEMU].** Hardware-configuration `tests/radio-port`
(both seams off, board `bg22_ek4108a`), alone at `-icount shift=5`: 165
TXWARM -> TX -> TX2RX -> RXSEARCH -> RXPD -> OFF cycles in 5.4 s of virtual
time, the silicon state mix of §7.63, no deadline panic. Against the
QEMU-configuration active scanner on `efr32bg22-dual` (`shift=3,sleep=off`,
363 s virtual): 3626 SCAN_REQs received, each through RXSEARCH -> RXFRAME ->
RX2TX -> TX -> TXPD -> OFF; 3625 SCAN_RSPs sent (the one exception is the
first event, which the M33 had not yet marked pending, so the RAC went
RX2RX); the scanner's host reported 3625 scan responses carrying the
complete-name AD. TX started 143.1 us after every frame end: the open
sequencer's 140 us STIMER hold plus the 3 us start. No panic. The
QEMU-configuration advertiser against the same scanner: 1183 SCAN_RSPs in
118 s virtual, 1180 reported. Hardware re-check of the current stage-3 build
[F]: 150 TXWARM and 156 TX per 5 s, 6 respond / 8 cancel / 0 timeout, and
the host reported `Name: efr32-radio-port`.

**Seams.** The advertiser no longer needs either seam in QEMU. A scanner
built with both seams off receives advertisements through the hardware path
(2209 reports, all well-formed) but never sends a SCAN_REQ: the hardware
driver marks an RX->TX window pending only after a TX->RX turnaround
(`radio_tmr_hcto_configure`), so a scanner's plain RX window falls through
RX2RX. That is a driver gap, not a model gap; the scanner still needs
`QEMU_TIFS`. The `run-qemu-*-e2e.sh` checks still match pre-§7.62 traces
(`RAC state 9 -> 0`, an 8-byte SCAN_RSP) and fail for that reason alone.

**CMU gates (§7.59) not modelled.** The model gates only the eight radio
blocks' EN registers on `CLKEN1`/`RADIOCLKCTRL`. `RDMAILBOX0/1`,
`RDSCRATCHPAD`, `PRORTC` and `CLKEN0.SYSCFG` are not gated: §7.59 shows
enabling the six together stopped the stock blob's lockups, but not which
access faults or how, so there is no rule to model yet.

### 7.71 T_IFS on silicon: matched to the stock stack, from hardware timestamps

**Method [F].** No SDR or sniffer is available, so the turnaround was timed
on the chip, non-intrusively, with `scripts/bg22-tifs-probe.tcl`. From the
debugger, while the firmware runs unmodified, PRS async channels 9-11 route
`MODEMH.EOF` (end of a received frame), `MODEMH.SYNCSENT` and
`MODEM.FRAMESENT` into TIMER1 input capture. EOF reloads and starts the
one-shot counter; the other two are captured against it. Probe findings:
- The MODEM events must be taken as `INSEL=PRSASYNCPULSE`.
  `PRSASYNCLEVEL` captured nothing, and `PRSSYNC` selects the synchronous
  channels instead.
- Debugger reads of `CCn.ICF` do not pop the capture FIFO. Once it is
  full, each new capture lands in `CCn.ICOF`, which the script polls.
- TIMER1 and PRS registers fault ("Failed to read memory") while their
  `CMU.CLKEN0` bit is clear.

**Calibration [F].** `FRAMESENT - SYNCSENT` equals the PDU+CRC bit count at
1 Mbps exactly. In stage 3 (TIMER1 on HFRCODPLL, 76.8 MHz): 8600 ticks =
112 us for a len-9 ADV_IND, 17816 = 232 us for the len-24 SCAN_RSP. In
stage 1 (TIMER1 on HFXO, 38.4 MHz): 12287 = 320 us for its len-35
ADV_IND, 3378 = 88 us for its empty SCAN_RSP. This identifies each packet
and confirms both tick rates.

**Result [F].** Interval from a received SCAN_REQ's EOF to our SCAN_RSP's
sync word:

| Image | EOF -> SCAN_RSP SYNCSENT | n | range |
|---|---|---|---|
| Stage 1 (Silicon Labs BLE stack, `ble_minimal`) | 180.8 us | 31 | 180.2-181.1 us (two at 182.1) |
| Stage 3, 140 us RX2TX hold | 185.2 us | 8 | 184.9-185.4 us |
| Stage 3, 135.6 us hold | 180.5 us | 9 (+10 repeat) | 180.1-180.9 us |

From EOF to the last bit at the antenna, and from SYNCSENT to the sync
word on air, the delays are fixed silicon paths, identical across the two
images on the same PHY. So equal intervals mean equal on-air T_IFS. With
the 140 us estimate the open stack answered 4.4 us later than stock. If
stock sits at the nominal 150 us, that was ~154.4 us, outside 150 +/- 2 us.
`EFR32_SEQ_RX2TX_TX_AT_100NS` is now 1356, matching stock to within ~0.3 us
(one STIMER tick is ~0.21 us). Functional re-check: 150 TX per 5 s, host
receives the scan-response name, no RX2TX timeouts.

**Absolute T_IFS [I].** The SYNCSENT->FRAMESENT airtime implies SYNCSENT
fires as the access address ends, 40 us (1 preamble + 4 AA octets) after
the packet starts. So T_IFS = EOF->SYNCSENT - 40 us + d_rx + d_tx, where d_rx
is the antenna-to-EOF delay and d_tx the SYNCSENT-to-antenna delay. If
stock's on-air T_IFS is the nominal 150 us, d_rx + d_tx is ~9.2 us. That
assumption is the remaining unverified link; checking it needs an external
receiver. Stock's jitter (~1 us) is larger than ours (~0.5 us): the open
image releases TX from a STIMER compare loop at fixed latency.

**Cross-check with a second peer [F].** The same probe, re-wired so that our
ADV_IND's FRAMESENT reloads TIMER1 and EOF is captured, timed the laptop's
Bluetooth controller answering with a SCAN_REQ: FRAMESENT -> SCAN_REQ EOF
335.7 us (334.4-336.9, n=27). The SCAN_REQ is 22 octets on air, 176 us.
With the host at the nominal 150 us, d_tx + d_rx = 9.7 us. The stock-based
estimate above gives 9.2 us, so two independent peers agree within 0.5 us.
Our absolute T_IFS is then 149.7-150.2 us. The split between d_tx and d_rx
still needs an external receiver.

**TX ready [F].** PRS `RACL.ACTIVE` (source 0x33, signal 0) rising ->
ADV_IND SYNCSENT: 131.7 us (131.65-131.76, n~330). That is TXEN to sync
word, i.e. TXEN -> first bit on air = 91.7 us + d_tx. [I] ACTIVE rising
marks the TXEN write (RAC leaving OFF). The LLL's
`radio_tx_ready_delay_get` and the three related getters still return 0.

**Radio prerequisites: which timers must run [F].** Live stage 3 has
PROTIMER enabled but stopped (`CTRL=0`, counter 0 across samples), and
RTCC, PRORTC, SYSCFG, RDMAILBOX0/1 and RDSCRATCHPAD clocks off. Knock-outs,
counting M0+ state-IRQ entries over 3 s before and after each:
- clearing `PROTIMER.EN` at runtime;
- gating `CMU.CLKEN1.PROTIMER` at runtime (PROTIMER accesses then bus-fault);
- a build that never sets `PROTIMER.EN` at all.

None changed TXWARM/TX (~90 per 3 s) or RXSEARCH entries, frames were still
received, and the last build sent 9 scan responses with the host receiving
the name. So the open image needs no PROTIMER counter, enable or clock,
during operation or at boot. §4's "PROTIMER before RAC" ordering came from
the vendor-sequencer path and does not hold for the open image. One
difference in the no-enable build: `RAC.STATUS` before the first TXEN read
`0x00400000` instead of `0x10400000` (bit 28). Its meaning is not yet
established. Also seen: IRQ1 (RXWARM) never fires in the open flow.

### 7.72 The RAC state machine, RAC.STATUS, bus-clock gating and radio timing, measured and modelled

All on the BGM220 board, stage 3 (`build-stage3-head`) unless noted. Probe
scripts and raw logs are in the session scratchpad, not in this repository.

**Methods.** (a) *Passive sampling*: ~20k reads of `RAC.STATUS` over the
AHB-AP while stage 3 advertised to an actively scanning host, bucketed by
state. (b) *Debugger as sequencer*: M33 halted; the M0+ halted, its NVIC
disabled (`ICER0`), and its PC set to a `wfi; b .-2` loop in spare SEQRAM, so
nothing but the debugger cleared state events; commands and `SEQIF` clears
were then written one at a time, reading `STATUS`/`SEQIF` after each.
(c) *Gate map*: M33 halted, every `CMU.CLKEN1` bit, `RADIOCLKCTRL.EN` and
`CLKEN0.SYSCFG` cleared one at a time while reading each 16 KiB radio
window. (d) *PRS*: `scripts/bg22-tifs-probe.tcl` generalised to any three
PRS signals (RACL.ACTIVE/RX/TX, MODEMH.SYNCSENT/EOF, MODEM.FRAMESENT,
MODEML.FRAMEDET) into TIMER1 CC0-2 at 76.8 MHz, CC0 reload-starting the
counter. (e) *M0+ SysTick probe*: a scratch build of `radio_seq.S` that
stamps SysTick (38.4 MHz assumed) at every IRQ entry, after each
acknowledgement until `STATUS` changes, after `SYNTHSTART` until `INLOCK`,
and around `PARAMP`.

**First attempts that were wrong.** An earlier version of (b) never halted
anything: `efm32.cpu halt` is not a target method in this OpenOCD, so the
firmware kept running and every command appeared to complete at once. With
the M0+ only *halted* (not in WFI), the RAC never left OFF at all, even for
TXEN: `STATUS` showed TXENS or RXENS and the state stayed 0. The parked-WFI
setup is what made the state machine observable. The first SysTick probe put
its ring at SEQRAM `0x4000`, which is in use by something else (it read
`0xe5ecc0de`, `0x01010201`, `0xaabbccdd`, ...; the owner is not identified),
and the image stopped advertising; moved to `0x2000`. That probe's
RX-side numbers are discarded: its `INLOCK` poll ran to its bound in the
TX2RX handler, where the receive synthesizer never reports lock.

**State machine [F]** (method b):
- OFF + SWRXEN -> RXWARM (1), `STATERXWARM` latched. TXEN in OFF ->
  TXWARM. **OFF leaves at once even with `STATEOFF` still set.**
- **Every other observed state is left only once its own `SEQIF` bit is
  cleared, whether or not `SEQIEN` enables it**: RXWARM was held with
  `SEQIEN` = 0. A pending event of *another* state does not hold: TXEN went
  OFF -> TXWARM, and the TXWARM clear went to TX, with `STATEOFF` set
  throughout. This refines §7.69's "any SEQIEN-enabled event" rule.
- RXWARM -> RXSEARCH on the clear, even if SWRXEN was withdrawn
  meanwhile. RXDIS in RXWARM did nothing.
- RXSEARCH -> RXPD when SWRXEN is withdrawn, no RXDIS needed. RXDIS also
  gives RXPD **with SWRXEN still set**. The RXPD clear then gives OFF ->
  RXWARM (both events latched), so the radio is soon back in RXSEARCH. This
  explains §7.63's "RXDIS left the RAC in RXSEARCH". TXDIS in RXSEARCH did
  nothing.
- RXPD, TXPD, SHUTDOWN -> OFF on their clear. TXDIS/RXDIS in RXPD did
  nothing. SWRXEN set during RXPD gave RXWARM right after the OFF entry.
- TXWARM -> TX on the clear. TXDIS in TXWARM only clears TXENS, and the
  clear then gives TXPD.
- **TXDIS in TX is immediate, even with `STATETX` uncleared**: TXPD, or
  TX2RX with SWRXEN set. RXDIS in TX does nothing. TXEN in TX sets TXENS
  (`0x49400000`); what that does at frame end was not observed.
- TX2RX -> RXSEARCH on the clear.
- FORCEDISABLE from RXSEARCH or TX -> SHUTDOWN (13) at once, clearing TXENS.
  The SHUTDOWN clear -> OFF with FORCEDISABLE still set. OFF then holds until
  FORCEDISABLE is cleared, after which a still-asserted SWRXEN gives RXWARM.
- Running stage 3, counted with the M0+ IRQ counters (method a): IRQ1 = 0,
  so the open flow never uses RXWARM (TX2RX goes straight to RXSEARCH).
  TXPD entries = responses + cancels, so **RX2TX goes to TXPD when the M0+
  withdrew the request with TXDIS**. A sample in RX2TX after that TXDIS
  read `0x17000000` (TXENS clear, still RX2TX).

**RAC.STATUS bits [F]:**

| state, condition | sample |
|---|---|
| OFF, events clear / STATEOFF pending | `0x00400000` / `0x10400000` |
| RXWARM pending (SWRXEN) | `0x91400002` |
| RXSEARCH pending / cleared | `0xb2400002` / `0xa2400002` |
| RXFRAME after TXEN (running) | `0x63400000` |
| RXPD (running, handler busy) | `0x14000000` |
| RX2TX (running) | `0x57000000`, `0x17000000` after TXDIS |
| TXWARM pending | `0x58400000` (`0xd8000002` running, SWRXEN set) |
| TX pending / cleared | `0x19400000` / `0x09400000` |
| TX2RX (running) | `0x9b000002` |
| SHUTDOWN pending (from TX / from RXSEARCH) | `0x1d400000` / `0x9d400002` |

So: SEQACTIVE (28) = any `SEQIF` state event set, regardless of `SEQIEN`
(it matches the coordinator's `0x10400000` before the first TXEN). TXENS (30)
= the latched TX request. RXENS (31) and RXMASK bit 1 = SWRXEN. DEMODENS
(29) = RXSEARCH or RXFRAME (clear in RXWARM, RXPD, RX2TX, TX2RX, TX,
TXWARM, OFF and SHUTDOWN). SEQSLEEPING (22) = the M0+ is sleeping in WFI:
clear while it is halted (`0x00000000` in OFF), and clear in every sample
taken while a handler ran. [I] RX2RX was never sampled.

**Bus clocks [F]** (method c). With `RADIOCLKCTRL.EN` set, clearing each
`CLKEN1` bit turns every read of exactly one window into a bus error:

| bit | block | bit | block |
|---|---|---|---|
| 0 | AGC `0xB800C000` | 6 | SYNTH `0xB8018000` |
| 1 | MODEM `0xB8014000` | 7 | **RDSCRATCHPAD `0xB8024000`** |
| 2 | RFCRC `0xB8010000` | 8 | RDMAILBOX0 `0xB8028000` |
| 3 | FRC `0xB8004000` | 9 | RDMAILBOX1 `0xB802C000` |
| 4 | PROTIMER `0xB801C000` | 10 | PRORTC `0xB8000000` (§1's [I] base confirmed) |
| 5 | RAC `0xB8020000` | 11 (BUFC) | **nothing**: BUFC stays readable |

Bits 12-15 changed nothing. `RADIOCLKCTRL.EN` cleared made every radio
window, BUFC (`0xBA000000`) and SEQRAM (`0xB0000000`) fault.
`CLKEN0.SYSCFG` cleared made SYSCFG fault at both the secure and the
non-secure address. A write while gated is accepted by the AP without error
and dropped: RAC `SR0` and SYSCFG `ROOTDATA1` read back their pre-gating
value once the clock returned. The windows at `0xB8008000` and from
`0xB8030000` to at least `0xB80FC000` fault whatever the enables. The
secure `0xA8...` radio aliases read 0 while the radio is attributed
non-secure. The coordinator saw the same bus-error response for TIMER1 and
PRS (§7.71). [I] That a CPU access takes a BusFault rests on the AHB error
seen by the AP; a CPU-side load was not stepped on silicon.

**Timing [F]** (methods d, e; TIMER1 at 76.8 MHz, n > 300 per row unless
noted):

| interval | value |
|---|---|
| RACL.ACTIVE rise (TXEN) -> RACL.TX rise | 91.0 us (TXWARM incl. the M0+ handler) |
| RACL.TX rise -> SYNCSENT | 40.6-40.7 us |
| FRAMESENT -> RACL.TX fall | 0.8-1.0 us |
| FRAMESENT -> RACL.RX rise | 124.3-124.6 us (TX exit to RX: 123.4-123.7 us, TX2RX incl. the M0+ handler) |
| EOF -> RACL.RX fall | 3.2-3.4 us |
| MODEML.FRAMEDET -> EOF | 131.4-133.2 us (frames assumed 14-octet SCAN_REQs) |
| `SEQIF` clear -> `STATUS` change (SysTick) | 1.09-1.22 us, same for TX2RX, RXPD, TXPD, RX2TX |
| `SYNTHSTART` -> `INLOCK`, TX (SysTick, n = 216) | 7.6-9.95 us, median ~8.9 us |
| `PARAMP` set -> `TXRAMPDONE` (SysTick, n = 110) | 0.44 us (one poll) |
| `PARAMP` clear -> `TXRAMPDONE` (SysTick, n = 216) | 0.81-3.96 us, median 3.96 us |

In receive, `SYNTH.STATUS` stayed `0x6` (IFFREQEN, SYNTHREADY) and never
showed `INLOCK`. The SysTick rate is assumed rather than cross-calibrated:
the probe's TX entry-to-exit read 150.9 us against 153.55 us from PRS, but
its two ends have different IRQ-entry latencies, so this is not a clean
check (±2%). **Note on a firmware comment:** `radio_seq.S` says the RAC
enters TX "~3 us after this handler acknowledges" (§7.68). The direct
clear-to-`STATUS` latency is 1.09 us; the ~3 us included the M0+'s
exception exit and the IRQ9 entry. The 135.6 us hold constant was tuned
end-to-end against stock (§7.71), so this changes no value.

**QEMU model** (`vendor/qemu` `2707a58585`): everything above under [F] is
modelled: the state routes and own-event holds, the 1.09 us acknowledgement
latency, the STATUS bits, the gate map (bus error on read, write dropped;
the machines no longer ignore bus errors, so a gated access is a CPU fault),
the TX timing (TX entry to exit = 0.65 + 40 + 8·(PDU + 3) + 0.9 us), the
receiver's EOF (peer FRAMESENT + 9.45 us, the d_tx + d_rx of §7.71) and its
exit 3.3 us later, `INLOCK` after 8.9 us (`IFFREQEN` instead in IF mode), and
the PA ramp times. Model check: EOF -> SCAN_RSP SYNCSENT is 180.6 us
(silicon 180.1-180.9 us).

[I]/placeholder choices in the model:
- RXFRAME begins at frame detection, 3.5 us after the access address
  arrives (from the FRAMEDET row, assuming those frames were SCAN_REQs).
- Frame-driven exits (TX end, frame end, RXFRAME entry) obey the own-event
  hold, which the debugger could not exercise.
- RXDIS in RXFRAME/RX2RX/RX2TX/TX2RX is ignored.
- A TXEN latched during TX is dropped at frame end.
- RXDONE is raised at EOF and TXDONE at TX exit.
- An aborted TX (TXDIS) raises no TXDONE.
- A receiver must be searching when the preamble arrives.
- Commands (TXEN, SWRXEN, TXDIS in TX, FORCEDISABLE) act with no latency.
- SEQRAM is not gated by `RADIOCLKCTRL`.
- Only LE 1M is modelled.

**Results in QEMU.**
- **Hardware-configuration advertiser alone** (shift=5): 159 TX in 5.2 s,
  no panic.
- **Against the QEMU-configuration scanner** (shift=3, sleep=off): 2863
  SCAN_REQs, 2863 SCAN_RSPs and 2863 name reports in 286 s virtual, no
  panic. The first SCAN_REQ is now answered too.
- **QEMU-configuration advertiser alone**: 159 TX in 5.2 s, now through
  RXWARM, as silicon does for OFF + SWRXEN.
- **QEMU-configuration advertiser against the scanner: a regression.** It
  hit its QEMU-only TIFS job deadline panic once, 61.48 us late against a
  61.04 us tolerance, after 33 s virtual. The previous model ran 118 s
  without one.

**Still divergent.**
- **M0+ code timing.** One `-icount` rate serves both cores, so the
  sequencer's handlers run at the M33's instruction rate. In the dual run
  TXWARM takes 12.6 us against silicon's 91 us, and TX2RX 39 us against
  123.5 us. The model reproduces the hardware latencies around them, but
  not the handler durations.
- **Not measured, so left as [I] or unmodelled:** command latencies,
  RX2RX's STATUS and hold, TXEN-in-TX semantics, and the d_tx/d_rx split.
- **Unidentified SEQRAM `0x4000` user.**

### 7.73 Timer and oscillator timing on silicon; the QEMU model follows it

Tags: **[F]** measured on silicon, **[I]** inferred from measured rules,
**[P]** placeholder (unmeasured; the model uses the named value).

QEMU: vendor/qemu commit `a10ac495f2`. Firmware consequence: stage 3 delivers
clock events >61 us late on silicon (152 per 10 s); see the BURTC section
below. Both QEMU firmware builds now panic on it, as silicon would with
`CLOCK_EVENT_SCHED_PANIC_ON_LATE`. The fix is open.

**Setup and method.**

- Board: BGM220 explorer kit (BG22), J-Link. Stage 3 left in flash; probes ran
  from SRAM after halting stage 3 (its clock tree configured), so no reflash.
  Board restored and checked afterwards: 150 / 152 per 5 s, "Name:
  efr32-radio-port".
- Live stage 3 clocks (debugger read): `CMU.SYSCLKCTRL = 0x00010402`
  (HFRCODPLL, HCLK /1, PCLK = HCLK/2), `EM01GRPACLKCTRL = 1` (HFRCODPLL),
  `DPLLREFCLKCTRL = 1` (HFXO), `DPLL.CFG1 = 0x0eff077f` (76.8 MHz),
  `EM23GRPACLKCTRL = 2` (LFXO), `EM4GRPACLKCTRL` = LFXO (BURTC on LFXO),
  `HFXO.STATUS = 0x40030003`, `LFXO.STATUS = 0x00010001`, `LFXO.CFG = 0x401`
  (TIMEOUT 4K), `DPLL.STATUS = 0x3`, BURTC EN=1 CFG=0 running,
  TIMER0 CFG=0 (PRESC 0) running, `PROTIMER EN=1, CTRL=0, COUNTER` static at 0
  over 100 ms.
- Timebases: DWT.CYCCNT at HCLK = 76.8 MHz (HFRCODPLL locked to HFXO), or at
  HFXO/FSRCO where stated; PRS async channels 9/10 (SWPULSE or a timer event,
  INSEL=PRSASYNCPULSE) captured by TIMER1 at 76.8 MHz. Cross-check:
  4096 BURTC ticks = 9598940-9598948 HCLK cycles (n = 4): 2343.49 cycles/tick,
  i.e. LFXO = 32771.6 Hz against the HFXO-derived clock (+110 ppm, crystal
  tolerance). P below = one LF period = 2343.5 HCLK cycles = 30.52 us.
- Probe source: a scratch probe (probe.c plus OpenOCD scripts; not committed),
  with raw logs kept outside the repo.

**BURTC (LFXO, CFG.CNTPRESC = DIV1).**

| Rule | Value | n | Method |
|---|---|---|---|
| [F] COMP write -> SYNCBUSY.COMP clear | 4743-7033 cycles = 2.02-3.00 P; completes at a fixed LF phase: the 3rd LF edge after the write (phase + busy = 7016-7028 for 63/64; the 64th landed after the edge, 9368 = 4 P) | 64, phase swept over the whole period | CYCCNT, edge-synced to CNT change |
| [F] COMP readback during sync | old value until SYNCBUSY clears, new after | 64 | same |
| [F] CNT write -> new value readable / SYNCBUSY.CNT clear | 4738-7004 / 4755-7021 cycles: the 3rd edge; reads the old count until then, then exactly the written value (no increment at that edge) | 32 | same |
| [F] CMD.STOP -> SYNCBUSY.STOP clear, STATUS.RUNNING clear, last increment | 4770-7006 cycles (3rd edge); CNT advanced exactly 3 after the write (the effective edge still counts) | 32 | same |
| [F] CMD.START -> SYNCBUSY.START clear / RUNNING set | 4786-7080 cycles (3rd edge); first increment one edge later (4th edge) | 32 | LF phase extrapolated from the last edge before STOP |
| [F] Compare match | IF.COMP sets on the edge where CNT advances from COMP to COMP+1 (CNT reads COMP+1 in the first poll that sees IF; IF 1.0 P after CNT first reads COMP) | 72 | CYCCNT poll of IF and CNT |
| [F] Compare lead | COMP = CNT + d (CNT read just before the write): d = -1, 0, 1, 2 never fire (wait for the 2^32 wrap); d = 3, 4, 5, 6, 8 always fire, at 4.0, 5.0, 6.0, 7.0, 9.0 P after the pre-write edge (phase + t_IF = 9377-9413, 11733-11773, 14069-14109, 16407-16465, 21101-21147 cycles) | 8 phases x 9 d | same |
| [F] Disarm / retarget an imminent match | Rewrite COMP while CNT reads COMP-k: old match still fires for k = 0, 1, 2; cancelled for k = 3, 4, 5 | 8 phases per k | same |
| [I] Unifying rule (modelled) | each of COMP/CNT/START/STOP takes effect at the 3rd LF edge after the write; the increment and compare on an edge use the state effective before that edge | - | fits every row above |
| [F] IF -> NVIC pending (IRQ 18) | same poll or one poll earlier (read order): within 46 cycles = 0.6 us | 40 fired | NVIC ISPR polled with IF |
| [F] EN clear (after STOP synced) -> SYNCBUSY clear | 8210-8220 cycles = 3.50 P | 8 (one phase) | CYCCNT |
| [F] EN set -> SYNCBUSY clear | 12008-13758 cycles = 5.12-5.87 P, completing at a fixed LF phase | 8, phase swept | CYCCNT |
| [I] EN completion grid | modelled at the 6th (set) / 4th (clear) falling LF edge after the write: no LF reference was available while BURTC was stopped, so the half-period offset is inferred from 3.50 P | - | - |
| [F] EN 0 -> 1 | CNT preserved; STATUS.RUNNING stays 0 until START | 8 | - |
| [F] Counting with CLKEN0.BURTC cleared | keeps counting: 100 of 100 ticks over 100 P | 3 | CNT before/after |
| [F] Bus access with CLKEN0.BURTC cleared | read: bus error; write: bus error (sticky error on the next transaction) | 1 read; see TIMER rows for writes | debugger AP |
| [I] CFG.CNTPRESC | counter divides by 2^n, synchronizers keep the undivided edges | - | header field meaning only |
| [P] PRECNT, a second write while a sync is pending (later write replaces it), writes while EN = 0 (synced the same way), counting before the source oscillator is RDY (not gated) | - | - | - |

Rate: [F] LFXO 32771.6 Hz vs HFXO-derived HCLK (n = 4); the model keeps the
32.768 kHz nominal. ULFRCO 1 kHz is the xg22.dtsi nominal [P].

**Consequence for the firmware (contradicts its assumptions).**

1. **COMP fires one LF tick after CNT reaches COMP.** The BURTC clock-event
   driver (`vendor/zephyr/drivers/timer/silabs_burtc_clock_event.c`) writes
   COMP = deadline and treats CNT >= deadline as due, so its interrupt comes
   one tick (30.5 us) after the deadline tick. Its `MIN_DELAY_CYC = 6` is
   safely above the measured minimum lead of 3 (d >= 3 fires); with the +1
   rule it could program COMP = deadline - 1 with a lead of >= 4.
2. **`sl_hal_burtc_set_compare()` spins on SYNCBUSY.COMP before and after the
   write**, i.e. 2.0-3.0 P (61-92 us) per COMP write, up to ~6 P (183 us)
   back to back, inside the clock-event spinlock. On silicon, stage 3's
   `late_count` (clock_event_sched.c, 0x20002da0) read 0x4c30 then 0x4cc8 10.0
   s later: **152 late deliveries (> 61036 ns tolerance) in 10 s on silicon**
   (stage 3 builds without PANIC_ON_LATE, so nothing is printed). TODO.md's
   "Missed deadline" item is reproduced on hardware.
3. QEMU regression (6 s, -icount shift=5): the radio-port-qemu-new and
   radio-port-qemu-hwcfg builds now panic at ~18 ms virtual:
   `clock event ... delivered 63270 ns late (tolerance 61036 ns): stage 2,
   servicing D` (hwcfg: 63180 ns), 0 TXWARM->TX (baseline 2707a58585: 153,
   no late). Isolation runs: keeping the silicon effect timing and the +1
   match but hiding SYNCBUSY from the CPU gives 153 TXWARM->TX and no late,
   so the CPU-side SYNCBUSY spin is what breaks the 61 us tolerance; using the
   old match edge with SYNCBUSY visible still fails ("radio job qemu tifs ran
   140577 ns late").

**TIMER0-4 (TIMER0 and TIMER1 measured; all five share the IP).**

| Rule | Value | n | Method |
|---|---|---|---|
| [F] Rate | PRESC 0, 1, 3, 9: CNT advanced 100004 / 50002 / 25001 / 10000 over 100000 HCLK cycles with EM01GRPACLK = HCLK | 1 each | CYCCNT |
| [F] Bus store spacing | two consecutive PRS SWPULSE stores: TIMER1 stamps 10 ticks apart (16/16); one store pulsing both channels: 0 apart (16/16) | 32 | PRS -> TIMER1 |
| [F] CMD.START -> counting | SWPULSE then START in consecutive stores, TOP = K: OF stamp - SWPULSE stamp = K + 10 for K = 4, 8, 16, 32 (all 32): START acts at the bus write within +-1 timer cycle (OF path vs SW path assumed equal) | 32 | PRS -> TIMER1 |
| [F] CMD.STOP | START...STOP pair, each preceded by SWPULSE: final CNT = stamp difference exactly (16/16): STOP latency = START latency | 16 | PRS -> TIMER1 |
| [F] STATUS.SYNCBUSY after a CMD write | 38 HCLK cycles (0.49 us) (8/8); STATUS right after START reads 0x40 | 8 | CYCCNT |
| [F] OC written equal to CNT (PRESC 1023) | fires at the end of the current tick (1026-1040 cycles after the tick edge) | 6 | CYCCNT |
| [F] OC = CNT + 1 | one tick later (2050-2064 cycles after the edge) | 6 | same |
| [F] START with CNT == OC | fires after one tick (1051 cycles) | 6 | same |
| [F] OC = CNT - 1 (passed) | does not fire (waits for the wrap) | 4 | same |
| [I] Compare rule (modelled) | CC and OF set IF on the tick where CNT advances out of OC / TOP | - | - |
| [F] OC arming at full rate | read CNT c, write OC = c + d, read c2: c2 - c = 26 ticks; fires iff d >= 19 (4/4 each d = 0..24): the write takes effect inside the read/write/read window, no extra arming lead | 100 | CYCCNT |
| [F] Match -> IF -> NVIC | NVIC pending seen in the same poll as IF (16/16); IF itself within the poll granularity (26 cycles) | 16 | CYCCNT poll |
| [F] EN rules (TIMER1, debugger) | EN = 0: writes to CTRL, CMD, TOP, TOPB, CNT, CC0_CTRL, CC0_OC, CC0_OCB, DTCTRL are bus errors; their reads return 0 (no error) and values are retained across EN 0 -> 1 (TOP/CNT/OC); EN = 1: writes to CFG, CC0_CFG, DTCFG are bus errors; IEN, IF always writable | 1 per register | debugger AP, sticky error on the next transaction |
| [F] EN rules (TIMER0, CPU) | CC0_CTRL write with EN = 0: precise BusFault, CFSR = 0x8200, BFAR = 0x50048064 (n = 2); CMD write with EN = 0: BusFault, BFAR = 0x5004800c (n = 1) | 3 | probe fault handler |
| [F] EN_SET -> START in the next store | starts (CNT 219 after 200 cycles, 8/8) | 8 | CYCCNT |
| [F] Counting with CLKEN0.TIMER0 cleared | keeps counting: 100040 counts over 100061 cycles gated | 3 | CNT before/after |
| [F] Bus access, clock gated | read error for TIMER0-4, BURTC, RTCC, PRS, PROTIMER, LETIMER0 (1 each); write error for TIMER1 CFG, TIMER1 IPVERSION, TIMER2 CFG, PRS SWLEVEL (5/5 each) | 30 | debugger AP |
| [F] CLKEN set -> accessible | CPU load in the store right after CLKEN_SET: no fault for TIMER2, TIMER3, TIMER4, RTCC, LETIMER0 (4 x 5) | 20 | probe |
| [P] SYNCBUSY after writes other than CMD; EN = 0 on a running timer (kept running in the model); CFG.CLKSEL other than PRESCHFPERCLK; MODE other than UP | - | - | - |

Note: the radio-block gating in 2707a58585 drops gated writes silently;
for TIMER1/TIMER2/PRS every gated write left a sticky bus error. A debugger
write reports the error only on the following transaction, which may be why
the radio measurement saw "dropped". Radio windows were not re-measured here.

TIMER0 clock-event minimum lead (TODO.md asks for evidence of 2000 ns): the
hardware needs none (an OC equal to CNT fires on the next tick; a write lands
within ~26 cycles = 0.34 us); any lead is for software latency only.

**Oscillators.**

| Rule | Value | n | Method |
|---|---|---|---|
| [F] FSRCO | 19.804-19.811 MHz (19818-19825 TIMER1 ticks per 38428 HFXO cycles) | 8 | CPU on HFXO, TIMER1 on FSRCO |
| [F] HFXO cold start, stage 3 config (at `CMU_DPLLLock` entry, FORCEEN) | ENS 25, RDY 5455-5480, COREBIASOPTRDY 5663-5665 FSRCO cycles = 1.26 us, 275.4-276.7 us, 285.9-286.0 us; STATUS 0x40000000 -> 0x40010003 (no HWREQ with FORCEEN only) | 3 | CYCCNT on FSRCO, calibrated |
| [F] HFXO STATUS while a hardware requester holds it | 0x40030003 (HWREQ set, ISWARM clear) | 1 | live stage 3 |
| [P] HFXO warm restart | not measurable: the radio holds HWREQ; the model reuses the cold numbers | - | - |
| [F] DPLL relock (HFXO ref running) | EN -> STATUS.RDY and IF.LOCK: 11588 HFXO cycles = 301.8 us every time | 16 | CYCCNT on HFXO |
| [F] DPLL disable | EN = 0 -> STATUS.ENS clear: 38-41 HFXO cycles (1.0-1.07 us) | 16 | same |
| [F] DPLL STATUS bit 31 | 0 (it is the config LOCK bit; the old model set it) | 1 | live stage 3 |
| [I] DPLL lock from a cold reference | counted from the reference's RDY | - | - |
| [P] DPLL ENS after EN (immediate); LFXO-referenced lock (uses the HFXO figure) | - | - | - |
| [F] LFXO cold start, stage 3 config (TIMEOUT 4K) | ENS -> RDY 124.6-125.4 ms (4096 LFXO cycles = 125.0 ms); ENS already set 0.41-0.54 ms after `CMU_LFXOInit` entry | 5 | host-timed debugger polling, ~0.45 ms resolution |
| [P] LFXO ENS latency (< 0.5 ms, modelled immediate); LFXO restart (held on by another requester) | - | - | - |
| [F] LFRCO FORCEEN | ENS 46-47, RDY 16048-16049 HCLK cycles (0.61 us, 209.0 us); FORCEEN clear -> ENS clear 51-65 cycles | 4 | CYCCNT |
| [F] CLKEN -> peripheral live | next access succeeds (see TIMER rows) | 20 | probe |

**What the model now does (commit a10ac495f2).**

- BURTC on an LF edge grid (EM4GRPACLKCTRL source), all [F]/[I] rules above,
  SYNCBUSY bits, old-value reads, IRQ 18 with IF.
- TIMER0-4 as one model (TIMER1, TIMER3 new): EM01GRPACLK / (PRESC+1) for all
  (TIMER2/4 were a fixed 38.4 MHz), immediate START/STOP/OC, CMD SYNCBUSY
  495 ns, compare/overflow on the tick leaving OC/TOP, EN-state bus errors and
  zero reads, IRQs 7-11.
- TIMER0-4 and BURTC bus-fault every access with their CMU clock off.
- HFXO, DPLL, LFXO, LFRCO STATUS timed from their request/enable.
- qtests: efr32bg22-test 91/91 (84 + 7 net new: burtc-compare-lead,
  burtc-retarget-window, burtc-en-sync, timer0-compare-equal-fires-next-tick,
  timer-cmd-syncbusy, timer-sync-registers-read-zero-when-disabled,
  timer-bus-faults, lfxo-ready-timeout; burtc-current-compare-waits-for-wrap
  removed, it encoded the opposite of silicon), efr32bg22-rf-test 24/24. The
  timing tests fail on 2707a58585.

**Not modelled / not measured.**

- PROTIMER: model still free-runs at 2 MHz; silicon stage 3 has EN=1, CTRL=0,
  COUNTER static at 0. What starts it (CTRL/CMD encodings) is not public.
- RTCC, LETIMER, SYSRTC: not modelled (RTCC/LETIMER only gate-tested).
- PRS itself, TIMER input capture, DTI, PWM outputs.
- M0+ SysTick / HCLKRADIO rate (TODO §7.72 cross-check) not measured.
- HF timer IF latency below the 26-cycle poll granularity.
- LF oscillator readiness gating the BURTC count; frequency of HFRCODPLL
  while the DPLL is locking.

### 7.74 Cross-core timing on silicon: the M33 slows the M0+ 2-3x on shared targets

Date: 2026-10-30. Board: the shared BG22 on J-Link. Probe sources, scripts and raw
logs are kept outside the repository.
Tags: [F] = measured directly here; [I] = inferred.

**Setup and method.**

- **Clock tree.** Stage 3 (`build-stage3-head`) boots normally and sets the clocks. Then the
  debugger halts the M33, loads a bare-metal M33 probe into SRAM (`0x20004000`) and resumes it
  there. The M0+ probe is loaded into SEQRAM (`0xB0000000`) and released the way
  `radio_reset()` does it: CTRL=pol|SEQRESET, copy, then CTRL=pol|FORCEDISABLE|CPUWAITDIS.
  FORCEDISABLE stays set, so the radio stays in OFF/SHUTDOWN. Clocks inherited from stage 3 [F]:
  - SYSCLK = HFRCODPLL at 76.8 MHz (DPLL referenced to HFXO, N=3839, M=1919).
  - `CMU.SYSCLKCTRL=0x10402`: HCLK/1, PCLK/2, **RHCLKPRESC=DIV2**.
  - EM01GRPA = HFRCODPLL, so TIMER1 runs at 76.8 MHz.
  - RADIOCLKCTRL.EN=1, `CLKEN1=0x2a87f`. ICACHE0's bus clock is off.
- **Workloads.** Each core runs a fixed loop of 200 iterations. Each iteration is 8 identical
  ops (ALU add, LDR, or STR to one address) plus `subs; bne`. One measurement is 1600 ops,
  repeated R=40 times; the first repetition is dropped, so n=39. Each repetition is stamped
  with the core's own cycle counter (M0+ SysTick, CLKSOURCE=core; M33 DWT.CYCCNT) and with
  TIMER1.CNT read directly by that core.
- **Cost per op.** Cost/op = (cycles - 609)/1600 on the M0+ and (cycles - 606)/1600 on the
  M33. The subtracted constant is the measured loop and stamp overhead: the ALU loop is
  exactly 1 cycle per add on both cores.
- **Interference.** The other core runs an interfering loop for the whole window, or sits
  idle. The debugger only arms an experiment and later reads the results; its footprint is
  quantified separately (§8).
- **Production-like rows.** Production M33 code runs from flash. I wrote 220 B of M33 loops
  into the then-unused flash at `0x60000` (slot1). Rows marked "flash" ran from there. That
  page was later wiped by `hwtest.sh`'s full erase.

**Calibration [F].**
- M0+ SysTick vs TIMER1: slope 1.99998 (N=500 vs 2000, n=49 each). So the M0+ core runs at
  exactly TIMER1/2 = 38.4 MHz in this configuration.
- M33 DWT vs TIMER1: 1.0000. The M33 runs at 76.8 MHz.
- BURTC (EM4GRPA = LFXO, 32768 Hz) as an independent reference: DWT measured
  76.7904 MHz, i.e. the HF tree is -125 ppm relative to the LFXO (crystal offsets, [I] which
  one).

**Repeatability [F].** With the M33 halted or spinning in flash, every M0+ baseline has zero
spread (min = max over n=39). The key contention rows were repeated in 3 back-to-back runs
and in a second session after a chip reset. They agreed to within 1.5% between sessions,
and within 0-0.2% inside a session.

**Address map seen by the M0+ [F].**

| target | M0+ address | result |
|---|---|---|
| SEQRAM | `0x0000_0000` | own RAM; flash is not visible (addr 0x100 is SEQRAM) |
| radio regs (RAC/FRC/SYNTH/MODEM) | `0xA8xx_xxxx` | ok |
| BUFC | `0xAA00_0000` | ok |
| radio/BUFC/SEQRAM NS aliases | `0xB8.., 0xBA.., 0xB0.., 0xA0..` | HardFault |
| main SRAM | `0x2000_0000` | **same physical SRAM as the M33's** (an M0+ store is read back by the debugger) |
| TIMER1, PRS, CMU | `0x5004C024`, `0x50038010`, `0x50008070` | ok; TIMER1 counts correctly |
| TIMER1 at `0x4004C024` | | no fault, but returns a constant value |

**Single-core baselines (the other core halted, or spinning in flash) [F].**

Cycles per op in the core's own clock. n=39, zero spread unless noted.

| op | M0+ @38.4 MHz | M33 @76.8, code in flash | M33 @76.8, code in SRAM |
|---|---|---|---|
| ALU add | 1.00 | 1.00 | 1.00 |
| SEQRAM ld | 2.00 | 1.88 | 3.87 |
| SEQRAM st | 2.50 | - | 3.87 |
| main SRAM ld / st | 3.25 / 3.25 | 1.00 / 1.00 | 1.50 / 1.75 |
| RAC/FRC/SYNTH/MODEM ld | 4.25 (all equal) | 3.88 | 5.87 |
| RAC SR st | 4.25 | - | 5.87 |
| BUFC ld | 3.25 | - | 3.87 |
| TIMER1 CNT ld | 8.75 | - | 13.1 |
| CMU ld | 4.75 | - | 5.12 |
| SysTick IRQ entry (reload to first ISR load) | 16 cycles | - | 17 (busy) / 18 (WFI) |

The other agent owns single-core costs. The SRAM-resident column is here only because it
changes the cross-core picture (§2).

**1. SEQRAM contention: M33 has priority [F].**

M33 loads from flash-resident code, M0+ measured. Cycles for 1600 ops; baseline in brackets.

| M0+ workload | M33 interferer | M0+ result | per M0+ op | slowdown |
|---|---|---|---|---|
| ALU (its own instruction fetch from SEQRAM) | SEQRAM ld | 6118-6150 [2209] | - | 2.8x |
| SEQRAM ld | SEQRAM ld | 10177-10248 [3809] | 5.98 vs 2.00 | 2.7x |
| SEQRAM st | SEQRAM st / ld | 11501-11527 / 11687-11705 [4609] | 6.8-6.9 vs 2.5 | 2.5x |
| RAC ld | SEQRAM ld | 9210-9212 [7409] | | +24% (fetch stalls) |
| SRAM ld | SEQRAM ld | 6723-6740 [5809] | | +16% |
| TIMER1 ld | SEQRAM ld | 17495-17509 [14610] | | +20% |

- Using the M33's measured rate (1.38 M0+ cycles per M33 SEQRAM load under contention), each
  M33 SEQRAM access costs the M0+ about **0.9 M0+ cycle** (~23 ns) [F].
- Same address vs a different SEQRAM word made no difference, so no banking was observed.
- Reverse direction (M33 measured, flash code): M33 SEQRAM ld goes from 1.88 to **2.38** M33
  cycles/op (4408 vs 3608, n=39 x 3 runs, zero spread). This happens as soon as the M0+
  executes anything: M0+ ALU, SEQRAM ld and SEQRAM st all give 4408. With the M0+ in WFI
  idle: 3608 (range 3608-3662).
- With M33 code in SRAM, the M33 SEQRAM ld was 3.87 cycles and only M0+ SEQRAM stores slowed
  it, to 4.49 (+14%).

**2. Main SRAM contention [F].**

Flash-resident M33.

| measured | interferer | result [baseline] | per op |
|---|---|---|---|
| M0+ SRAM ld | M33 SRAM ld | 7553-7644 [5809] | 4.34-4.40 vs 3.25 (+34%) |
| M0+ SRAM ld | M33 SRAM st | 7476-7481 | +29% |
| M0+ SRAM st | M33 SRAM st | 6940-7020 [5809] | +20% |
| M0+ SRAM ld | M33 ALU in flash | 5934-5941 | +2% |
| M0+ SRAM ld | M33 RAC ld | 5853-5857 | +0.8% |
| M33 SRAM ld | M0+ SRAM ld | 2429-2464 [2205] | 1.15 vs 1.00 (+10.5%) |
| M33 SRAM ld | M0+ SRAM st | 2695-2710 [2205] | 1.31 (+23%) |
| M33 SRAM st | M0+ SRAM ld / st | 2589-2599 / 2430-2462 [2205] | +18% / +10% |

- **M33 code fetched from SRAM** competes with the M0+'s SRAM path even when the M33 does no
  data accesses. An M33 spin loop in SRAM raised M0+ SRAM ld from 5809 to 5810-8409,
  depending on the loop's address and alignment (it changed between two builds). The M33 ALU
  loop in SRAM gave 6948.
- Conversely, M0+ SRAM traffic slows an SRAM-resident M33 ALU loop by 3% (2206 to 2270-2285),
  and leaves a flash-resident one at 2206-2207.
- M0+ TIMER1 and CMU loads also pay about +0.5 cycle/op while the M33 does SRAM data
  traffic (TIMER1: 15408 vs 14610). The M0+ path to the peripherals shares the SRAM/AHB
  matrix with the M33 [I].

**3. Radio register contention: one shared path for RAC/FRC/SYNTH/MODEM; BUFC separate [F].**

| measured | interferer | result [baseline] | per op |
|---|---|---|---|
| M0+ RAC ld | M33 RAC/FRC/SYNTH ld or RAC SR st (code in SRAM) | 15290-15316 [7409] | 9.2 vs 4.25 (2.07x) |
| M0+ RAC ld | M33 RAC ld (flash) | 16320-16358, 3 runs + 2nd session | 9.83 (2.3x) |
| M0+ RAC ld | M33 BUFC ld | 7409-7410 | no effect |
| M0+ BUFC ld | M33 BUFC ld | 8230-8247 [5809] | 4.77 vs 3.25 |
| M0+ BUFC ld | M33 RAC ld | 5809-5810 | no effect |
| M33 RAC ld (flash) | M0+ RAC ld / FRC ld | 8373-8383 [6807] | 4.86 vs 3.88 (+23%) |
| M33 RAC ld (SRAM code) | M0+ RAC/FRC ld or SR2 st | 10677-10685 [10003] | +6.8% |

So any of RAC, FRC, SYNTH and MODEM contends with any other, both for loads and stores, and
BUFC is an independent target. The M0+ loses about 2x; the M33 loses 7-23%. Priority
(M33 > M0+) is [I] from this asymmetry; the arbitration scheme is not identified.

**4. Peripheral bus (TIMER1/CMU/PRS, PCLK = 38.4 MHz) [F].**

All rows with M33 code in SRAM.

| measured | interferer | result [baseline] |
|---|---|---|
| M0+ TIMER1 ld | M33 TIMER1 ld | 37855-37884 [15409] (2.46x) |
| M0+ TIMER1 ld | M33 CMU ld | 21808-21828 (+42%) |
| M0+ CMU ld | M33 TIMER1 ld | 31527-31528 [9009] (3.5x) |
| M0+ CMU ld | M33 CMU ld | 15163-15176 (1.7x) |
| M33 TIMER1 ld | M0+ TIMER1 ld | 28339-28365 [21605] (+31%) |
| M33 TIMER1 ld | M0+ CMU ld | 23581-23591 (+9%) |
| M33 CMU ld | M0+ TIMER1 / CMU ld | 14781-14839 (+68%) / 10543-10553 (+20%) [8805] |

- Radio-register and SEQRAM traffic by one core does not slow the other core's peripheral
  accesses, except through the M0+ instruction-fetch stalls of §1.
- With HCLKPRESC=DIV4, an M0+ ALU chunk bracketed by two TIMER1 reads rose from 484 to 520
  TIMER1 ticks. The ALU cycle count itself was unchanged (229), so peripheral access latency
  follows HCLK [F].

**5. Flash / ICACHE [F].**

- The M0+ cannot reach flash.
- M33 flash data traffic left every M0+ workload at its exact baseline:
  - a cache-missing sweep (32 B stride over 64 KiB);
  - repeated hits on one word.
- M33 code executing from flash with no data traffic: M0+ SEQRAM, radio and BUFC rows at
  exact baseline. M0+ SRAM ld +2% and TIMER1 ld +0.5% with the flash ALU loop, 0% with the
  flash spin loop.
- ICACHE behaviour itself was not varied (its bus clock is off in stage 3). Its effect on
  cross-core timing is therefore **unmeasured** beyond this.

**6. LDMA traffic [F].**

Continuous self-linked descriptors, 64 words per block, REQMODE=ALL; the M33 measured from
flash code.

| LDMA traffic | M0+ ALU | M0+ SEQRAM ld | M0+ SRAM ld | M0+ RAC ld | M0+ TIMER1 ld | M33 SRAM ld | M33 SEQRAM ld | M33 RAC ld |
|---|---|---|---|---|---|---|---|---|
| none | 2209 | 3809 | 5809 | 7409 | 14610 | 2205 | 3608 | 6807 |
| SRAM->SRAM (2 runs) | 2209 | 3809 | 6462-6468 | 7409 | 15406-15409 | 2764-2767 | 3960-4017 | 7183-7198 |
| SRAM->SEQRAM (2 runs) | 3668-3677 | 7107-7115 | 6775 | 8319-8336 | 16137-16159 | 2507-2512 | 5058-5136 | 9005-9021 |

- LDMA from a RAC register raised LDMA IF bit 31 (error) after one block and wrote stray
  words into my SRAM mailbox. That variant is **unmeasured**.
- Clearing `LDMA.EN` does **not** stop a busy self-linked channel (CHBUSY stayed 1 and kept
  loading SEQRAM). Breaking the link and writing CHDIS did stop it [F].

**7. Clock changes made by the M33 [F].**

Static configurations. The M0+ rate is from SysTick vs TIMER1 (in-core, exact) and from the
monitor counter vs the LFXO BURTC (±0.1%):

| SYSCLKCTRL | M0+ clock | M33 clock |
|---|---|---|
| HFRCODPLL 76.8, HCLK/1, RHCLK/2 (stage 3) | 38.4 | 76.8 |
| CLKSEL=HFXO 38.4 | **19.2** | 38.4 |
| CLKSEL=FSRCO ~20 | **9.89** | 19.80 |
| HCLKPRESC=DIV2 | 38.4 | 38.4 |
| HCLKPRESC=DIV4 | 38.4 | 19.2 |
| CLKSEL=HFXO, RHCLKPRESC=DIV1 | 38.4 | 38.4 |
| EM01GRPA = HFXO or FSRCO | 38.4 (unchanged) | 76.8 |

- **M0+ clock = SYSCLK / (RHCLKPRESC + 1)**. It is independent of HCLKPRESC and of the
  EM01GRPA clock.
- **Live switching** (the M33 toggled SYSCLKCTRL every 100 µs while the M0+ timed 229-cycle
  chunks, about 2400 chunks per case):
  - PLL<->HFXO: chunk durations took the fast value (484 ticks), the slow value (963-965) or
    intermediates; maximum 972.
  - PLL<->FSRCO: slow 1868-1873, maximum 1882.
  - So a switch adds at most ~8-10 TIMER1 ticks (~0.1 µs) beyond the rate change. No stall
    was observed.
- **RADIOCLKCTRL.EN = 0** freezes the M0+: 72,695 cycles in 1.006 s, which is the time
  before the write landed. It resumes on re-enable with its state intact. Meanwhile the
  debugger's SEQRAM reads fail.
- **CLKEN1 gating** of RAC or FRC makes M0+ loads from that block HardFault (8 of 8, ~22
  cycles each). Gating BUFC (bit 11) has no effect, matching §7.72.

**8. Debugger footprint [F].**

OpenOCD on J-Link OB at 4 MHz SWD manages ~17.6k AP word reads/s. M33 halted, M0+ measured,
n=399 per row.

| debugger target | M0+ ALU | M0+ SEQRAM ld | M0+ RAC ld | M0+ TIMER1 ld |
|---|---|---|---|---|
| none | 2209 | 3809 | 7409 | 14610 |
| SEQRAM reads | 2209-2211 | 3809-3813 | 7409-7412 | 14610-14619 |
| RAC reads | 2209-2210 | 3809-3810 | 7409-7421 | 14610-14617 |
| TIMER1 reads | 2209-2210 | 3809-3810 | 7409 | 14610-14645 |

That is about 1 M0+ cycle per AP access on the same target, and 2-5 cycles on the
peripheral bus.

The larger effects of having the debugger attached:
- M33 WFI does not stop the M33 core clock: DWT ran at 76.76 MHz through WFI.
- EM2 is never entered while OpenOCD is attached. EMU.STATUS.EM2ENTERED stayed 0 with or
  without EM2DBGEN, with the M0+ idle or held in reset, and with TIMER1 stopped.
- WFI effects are exaggerated (§9).

My own experiment polling (one SRAM word per 20 ms) is below the resolution of these
measurements. An earlier driver in which the **M33** polled SEQRAM every ~300 cycles added
0.6% to the M0+ ALU loop, with ±2 cycles of jitter: 22146 vs 22009 for N=2000.

**9. Energy modes [F].**

| M33 state | M0+ SRAM ld | M0+ SRAM st | M0+ TIMER1 ld | M0+ CMU ld | M0+ SEQRAM/radio/ALU |
|---|---|---|---|---|---|
| halted / spinning in flash | 5809 | 5809 | 14610 | 8210 | baseline |
| WFI (EM1), debugger attached | 9709-9909 (+67-71%) | 10509-10709 | 15409 (+5.5%) | 9009 (+9.7%) | unchanged |
| WFI (EM1), **debugger detached** | **6309-6310 (+8.6%)** | - | **15409 (+5.5%)** | - | - |

- **EM2, debugger detached** (deep-sleep WFI woken by a BURTC compare after 0.5 s; 3 runs):
  - EM2ENTERED=1.
  - M33 DWT advanced 70-77 cycles.
  - **The M0+ advanced 82 cycles (`0xd0f`->`0xd61`) in 0.5 s. It stops whenever the M33 is
    in EM2.**
- **After EM2 wake** [F]:
  - HFXO.STATUS=`0x40000000` (not ready) and DPLL.STATUS=0 (unlocked). SYSCLKCTRL still read
    HFRCODPLL, so SYSCLK ran on a free-running HFRCO: the M0+ measured 38.374 MHz and the M33
    76.748 MHz vs the LFXO, about -0.07%.
  - The M0+ did not complete its stop handshake within 0.5 s of the M33 waking. It was later
    found in its idle loop with the values its reset handler writes (state=1, cmd=0). [I] It
    may have been reset or lost context; not resolved.
  - A chip reset restored the clocks.
- **Wake latency** of the M0+ from EM2: **unmeasured**.

**10. Interrupt latency under contention [F].**

SysTick period interrupt; latency = reload minus CVR at the handler's first load. n=199 per row.

| core | interferer | latency (cycles) |
|---|---|---|
| M0+ (WFI or busy) | none / M33 flash spin / M33 RAC ld / M33 SRAM st / M33 WFI | **16** (17 under RAC ld), sd 0 |
| M0+, WFI | M33 SEQRAM ld | med 45, 23-51, sd 7.2 |
| M0+, WFI | M33 SEQRAM st | med 47, 45-55 |
| M0+, busy | M33 SEQRAM ld / st | med 47 / 51, range 27-54 / 28-59 |
| M33 (vectors, ISR and stack in SRAM) | M0+ idle / ALU / SEQRAM st / RAC ld | 17 busy, 18 WFI, sd 0 |
| M33, busy / WFI | M0+ SRAM ld | 17-20 / 19-21 |
| M33, busy / WFI | M0+ SRAM st | 17-22 / 19-22 |

The M0+ handler entry, which stacks to SEQRAM and fetches its vector and code from SEQRAM,
goes from 0.42 µs to 0.6-1.54 µs while the M33 streams SEQRAM accesses.

**Not measured.**

- EM2 wake latency of the M0+.
- LDMA with a radio-register source or target.
- ICACHE enabled vs disabled.
- RX/TX-active radio (FRC/BUFC hardware DMA into SEQRAM/BUFC) as an interferer: the radio
  stayed in FORCEDISABLE throughout.
- Cross-core IRQ (doorbell) latency.
- M33 code executing from flash while the M0+ hammers flash: impossible, the M0+ has no
  flash access.

**What a QEMU model needs to capture (numbers from above).**

1. **M0+ clock**:
   - `f_M0 = SYSCLK / (RHCLKPRESC + 1)`, which is 38.4 MHz in stage 3. Independent of
     HCLKPRESC and EM01GRPA. SYSCLKCTRL writes take effect at once with no stall worth
     modelling (≤0.1 µs).
   - RADIOCLKCTRL.EN=0 freezes the M0+ with state retained.
   - M33 EM2 stops the M0+. After EM2 the HF clocks are not restored by hardware (HFXO off,
     DPLL unlocked).
2. **Base costs per M0+ op**, the clean single-core numbers (cycles): SEQRAM ld 2, SEQRAM st
   2.5, SRAM 3.25, radio regs 4.25, BUFC 3.25, TIMER1 8.75, CMU 4.75, SysTick IRQ entry 16.
3. **SEQRAM is a shared port with M33 priority.**
   - Each M33 SEQRAM access steals about 0.9 M0+ cycles from M0+ instruction fetch and data.
   - An M33 streaming SEQRAM loads makes M0+ code run ~2.8x slower and M0+ SEQRAM loads cost
     ~6 cycles.
   - The M33's SEQRAM access is 1.88 M33 cycles, rising to 2.38 whenever the M0+ is executing.
4. **Radio register path** (RAC/FRC/SYNTH/MODEM shared, BUFC separate):
   - Under M33 streaming, the M0+ access goes from 4.25 to 9.2-9.8 cycles.
   - The M33 access goes from 3.88 to 4.86 M33 cycles.
   - BUFC: M0+ 3.25 -> 4.77 under M33 BUFC streaming.
5. **Main SRAM**: M0+ +0.7 to +1.1 cycle/access under M33 SRAM data traffic. M33 +0.15 to
   +0.31 cycle/access under M0+ SRAM traffic.
6. **Peripheral bus** (TIMER1/CMU): mutual slowdowns of +9% to 3.5x (§4). The M0+ also pays
   +0.5 cycle per peripheral access during M33 SRAM traffic and while the M33 is in WFI
   (+5.5% for TIMER1, detached).
7. **M0+ interrupt entry** grows from 16 to 23-59 cycles (median ~45-51) while the M33
   streams SEQRAM. Contention on other targets leaves it at 16-17.
8. **Second-order effects, probably not worth modelling**:
   - flash/ICACHE traffic: nil;
   - debugger: ~1 cycle per AP access;
   - LDMA SRAM->SRAM: M0+ SRAM +0.41 cycle/op;
   - LDMA into SEQRAM: comparable to M33 SEQRAM streaming (M0+ ALU 1.66x, SEQRAM ld 1.87x).

**In QEMU (2026-11-01).** `vendor/qemu/hw/arm/efr32-interconnect.c` models these paths
(ports, round-robin handover, idle wait state, the SEQRAM fetch stream); the probes above,
run unchanged on the model, and the model-vs-silicon table are in docs/model-fidelity.md
("Interconnect calibration").

**Files.**

Kept outside the repository:

- **Probe sources**:
  - `m0.S`, `m0.ld`: the M0+ probe (mailbox at SEQRAM 0x2F00, results at 0x2000).
  - `m33.c`, `m33.ld`: the M33 SRAM probe (mailbox at 0x20003F00, results at 0x20003000).
  - `m33flash.S`: the flash-resident loops at 0x60000.
  - `build.sh`: builds all three.
- **Driver**: `xc.py`, plus `takeover.py` and `reload33.py`.
- **Experiment scripts**: `mat_m0.py`, `mat_m33.py`, `flashrun.py`, `base_halt.py`,
  `lat.py`, `dbg.py`, `clk.py`, `clkdyn.py`, `clk2.py`, `em2*.py`, `detached.py`, `ldma.py`,
  `rep.py`.
- **Logs**: the matching `*.log` and `*.json` files. `rep-contaminated-by-ldma.log` is an
  invalid run: the LDMA was still running.

### 7.75 Per-core clocks: each core runs at its silicon rate in QEMU

Tags: **[F]** measured on silicon, **[I]** inferred, **[P]** placeholder (unmeasured).
QEMU branch `core-clocks` in vendor/qemu (worktree `build/qemu-clocks-src`, build
`build/qemu-clocks`), on top of `a10ac495f2` (timer-fidelity). Scratch probes:
`scratchpad/clk/` (kern.S, run.py, runf.py, qrun.sh, dur.awk); raw results
`clk/out-*.json`, `clk/outf*.json`, `clk/runf*.log`, `clk/live-*.log`, `clk/rx2tx-si.log`.

**Setup and method.**

- Board: BGM220 explorer kit, J-Link, stage 3 (`build-stage3-head`) running to set up the
  clock tree, then the core under test driven from the debugger while the other core is
  halted (the M0+ also has its NVIC disabled). **This is the no-interference baseline**;
  the cross-core agent's numbers (`cross-core-findings.md`) agree with every row they
  share (M0+ SEQRAM 2/2.5, radio 4.25, BUFC 3.25; M33 XIP SEQRAM 1.88, radio 3.88).
- Live stage-3 registers [F]: `CMU.SYSCLKCTRL = 0x00010402` (HFRCODPLL, HCLK /1, PCLK /2,
  RHCLKPRESC = DIV2), `EM01GRPACLKCTRL = 1`, `DPLL.CFG1 = 0x0eff077f`, `MSC.READCTRL =
  0x00100000` (MODE = WS1), `RADIOCLKCTRL = 1`. ICACHE0 registers fault from the debugger
  (bus clock off). M0+ SysTick: `CSR = 0x00010005` (core clock), `RELOAD = 9999975`,
  `CALIB = 0xC0061A7F`.
- **Kernels** (`clk/kern.S`, position independent): bracket = core cycle counter (M33
  DWT.CYCCNT, M0+ SysTick CVR) and TIMER1.CNT read by the core itself; body = 8 copies of
  one operation + `subs/bne`; run at N = 10 and N = 110 iterations, 5 runs each, cost per
  iteration = difference / 100. TIMER1 free-running on HFRCODPLL with CFG.DEBUGRUN
  (16-bit). The M0+ reads TIMER1 at `0x5004C024` (its AP reaches it; `0x4004C024` reads 0).
- **Timebase**: TIMER1 = 76.8 MHz, calibrated by packet airtime (SS7.71: 8600 ticks =
  112 us) and by the timer agent against HFXO/LFXO.
- **Function CPI**: a firmware function called from the debugger with a bkpt return stub
  (M33: SRAM 0x20005000, MPU off, PRIMASK set; M0+: SEQRAM 0x2900 stub `ldr CVR; blx;
  ldr CVR; bkpt`), cycles at full speed, instructions by single-stepping the same call.
- All runs below were zero-spread unless a range is given. Runs started after the board
  disconnect (23:16) were redone from scratch after reconnection; the kernel tables were
  re-measured post-reconnect with identical results.

**1. Core clocks.**

| quantity | value | n | method |
|---|---|---|---|
| [F] M33 HCLK / TIMER1 | **1.000** (55000-55002 cycles vs 55016-55018 ticks incl. a 16-17 tick bracket; XIP 54999 / 55014) | 10 + 10 | CYCCNT vs TIMER1, ALU loop N = 5000 |
| [F] M33 HCLK | **76.8 MHz** in stage 3 | - | = TIMER1 |
| [F] M0+ clock / TIMER1 | **0.5000** (21890 cycles in 43780 ticks between N = 10 and N = 2000; every kernel pair exactly 1:2) | 10 | SysTick (CLKSOURCE = core) vs TIMER1 |
| [F] M0+ clock | **38.4 MHz** in stage 3 | - | = TIMER1 / 2 |
| [F] (cross-core agent) M0+ = SYSCLK / (RHCLKPRESC + 1): 19.2 MHz on HFXO, 9.89 MHz on FSRCO, independent of HCLKPRESC; RADIOCLKCTRL.EN = 0 stops it with state kept | | | |
| [I] the M0+ SysTick CALIB (`0xC0061A7F`) is NOREF + SKEW + RAC.STCTRL.STCAL as written by the firmware (`STCTRL = 0x01061a7f`); QEMU's CALIB does not follow RAC.STCTRL (unmodelled) | | | |

The docs' earlier 2% SysTick cross-check discrepancy (SS7.72) is not a rate error: the M0+
clock is exactly half of TIMER1.

**2. Per-access and per-instruction costs [F] (cycles of the accessing core).**

Loops of 8 copies; per-op cost = (cycles per iteration - loop overhead 3) / 8. n = 5 + 5.

| op | M33, code in SRAM | M33, code in flash (warm cache) | M0+ (code in SEQRAM) |
|---|---|---|---|
| ALU `adds` | 1 | 1 | 1 |
| NOP | 0.5 (dual issue) | 0.5 | 1 |
| SRAM ld / st | 1.5 / 1.5 | 1 / 1 | (not visible in QEMU's M0+ map; 3.25 per cross-core) |
| flash data ld | 4.0 | 4.5 | - |
| SEQRAM ld / st | 3.625 / 3.625 | 1.875 / 1.875 | 2 / 2.5 |
| BUFC ld | 3.625 | 1.875 | 3.25 |
| RAC/FRC/SYNTH/MODEM/AGC ld, RAC.SR2 st, SET-alias st | 5.625 | 3.875 | 4.25 |
| CMU ld | 5.125 | 3.875 | - |
| GPIO ld | - | 3.875 | - |
| TIMER1.CNT ld | 13.125 | 11.875 | 8.75 |

M0+ costs in context (literal-load pairs and ALU around the access, kernels `k_lls`,
`k_lll`, `k_saa`, `k_laa`, n = 5 + 5, measured twice incl. after the reconnect):
literal `ldr` 2; radio load or store **5.0**; BUFC load **4.0**; SEQRAM load 2, store 2.5.
So a radio access costs 0.75 cycle more when isolated than back to back.

**Firmware-function CPI** (debugger calls, n = 6 each, warm; cycles less the empty-call
overhead):

| core | function | cycles | instructions | cycles/insn |
|---|---|---|---|---|
| M33 | snprintk (compiled C, z_cbvprintf) | 7973 (cold 8073) | 4342 | **1.84** |
| M33 | memcpy 256 B | 2057 | 1286 | 1.60 |
| M33 | memset 256 B | 1542 | 1030 | 1.50 |
| M33 | strlen | 71 | 49 | 1.45 |
| M0+ | efr32_seq_delay(4) | 171 | 100 | 1.71 (7 cycles per loop pass) |
| M0+ | efr32_seq_delay(2) | 80 | 48 | 1.67 |
| M0+ | rx_frontend_stop | 61 | 22 | 2.77 |
| M0+ | synth_config_tx | 112 | 39 | 2.87 |
| M0+ | synth_lock (cached) | 468 | 247 | 1.89 |
| M0+ | pga_dc_interp (ALU) | 82 | 69 | 1.19 |
| M0+ | synth_power_down | 163 | 65 | 2.51 |

**Provenance of the M33 rows:** one run (n = 6 calls per function, zero spread except the
first cold call; the stepped counts are a property of the code and input) taken during the
board hold that ended in the USB disconnect, before it; the functions returned correct
results (snprintk produced "123456 beef hello world|42-deadbeef"). Two reruns after the
reconnect failed in the harness (the debugger-call setup no longer reached the return
stub: register arguments did not take, CONTROL.SPSEL = 1 at halt), so these rows are
**not re-confirmed post-reconnect** and should be repeated before they are cited as [F].
The M0+ rows and all kernel tables were measured after the reconnect.

[I] The M0+ follows the Cortex-M0+ instruction timings exactly once radio accesses are
taken as 5 cycles: rx_frontend_stop = 10 literal loads x 2 + 4 movs + 7 radio stores x 5
+ bx 2 = 61; synth_config_tx = 20 x 2 + 5 + 13 x 5 + 2 = 112; the delay loop pass = ldr 2
+ subs 1 + str 2 + bpl 2 = 7. A flat average cannot fit both 1.19 and 2.87.

**3. efr32_seq_delay(n) on silicon — contradicts the firmware's assumption [F].**

`radio_seq.S` names its arguments `*_US` (e.g. `EFR32_SEQ_DCCAL_ISOLATE_US 16`), but the
loop takes 7 cycles per pass and makes floor((195 n - 111) / 30) + 1 passes:
**delay(n) ~ 1.18 n us + 0.3 us at 38.4 MHz** in the real image (delay(4) = 171 cycles =
4.45 us, delay(2) = 80 cycles = 2.08 us). A copy at another SEQRAM alignment took 8 cycles
per pass (delay(n) = 17 + 8 x passes: 41, 97, 249, 513, 1033, 2593 cycles for n = 1, 2, 5,
10, 20, 50; 1.35 n us), so the code's placement changes every settle wait by ~14%.
Neither is n us. At the stock image's assumed core clock the constant 195/30 would match
1 us per unit at ~45 MHz (7 cycles) [I].

**4. Handler durations on silicon [F] (re-measured, PRS -> TIMER1, live stage 3).**

| interval | value | n |
|---|---|---|
| RACL.ACTIVE rise (TXEN) -> RACL.TX rise (TXWARM incl. handler) | **91.03 us** (all samples equal at 13 ns resolution) | 264 |
| RACL.TX fall -> RACL.RX rise (TX2RX incl. handler) | **123.53 us** (123.44-123.62) | 376 |
| RX2TX handler: entry -> synthesizer locked (open image's own SysTick / STIMER trace words) | **682 SysTick cycles = 17.76 us** (682-685); 85 STIMER ticks (85-86) | 58 |
| RX2TX: entry -> M33's respond decision seen | 208 STIMER ticks = 43.3 us | 58 |
| RX2TX: decision -> PA up (pa_start) | 138 ticks = 28.75 us | 58 |

**5. QEMU model (branch core-clocks).**

Design: icount per-CPU clocks (opt-in: `icount_cpu_clocks_enable`, enabled by the
efr32bg22 machine property `core-clocks`, default on, precise -icount only).
- Rounds of at most 500 ns, never past the next QEMU_CLOCK_VIRTUAL deadline. Each
  runnable vCPU runs until its own time reaches the round end; device timers fire only
  between rounds, so they are exact. QEMU_CLOCK_VIRTUAL inside a vCPU = that vCPU's time.
- Overshoot (an instruction or access charge cannot be split) carries into the next
  round. Halted vCPUs resume at the waker's time (interrupt raise, or the M0+ release).
  0 Hz = clock stopped (RADIOCLKCTRL.EN = 0).
- Time per vCPU: M33 = instructions x 1.84 cycles at HCLK (CMU: SYSCLKCTRL CLKSEL,
  HCLKPRESC; FSRCO 19.807 MHz at reset, DPLL as configured); M0+ = Armv6-M cycles counted
  per instruction class by the Thumb translator (1 + class extra; taken B<c> +1 at run
  time) at SYSCLK / (RHCLKPRESC + 1).
- Bus-model hook `IcountBusModel.access(master, addr, size, write) -> extra millicycles,
  target class`; occupancy per vCPU per round in `CPUState.icount_bus_accesses[]`;
  `round_end` callback for a future interconnect model. efr32 table: M33 radio/periph
  +2.875, BUFC +0.875, TIMERn +10.875, SEQRAM +0.875 (+1.375 while the M0+ executes, which
  also costs the M0+ 0.9 cycle per access, cross-core [F]); M0+ radio +3, BUFC +2.
  Exception entry: M0+ 16 cycles (cross-core [F]), M33 17 (cross-core [F], SRAM vectors).
  The M33 SEQRAM window at 0xB0000000 is now an I/O view so it reaches the bus model.
- SysTick processor clocks of both cores follow the same clocks.

Validation (run with -icount shift=5,sleep=off unless noted):

| check | silicon | QEMU core-clocks | QEMU before (shared rate, same base, core-clocks=off) |
|---|---|---|---|
| TXWARM (0->8 to 8->9) | 91.03 us | **85.65 us** (85.36-86.04, n = 10935) | 59.2 us |
| TX2RX (9->11 to 11->2) | 123.53 us | **118.98 us** (118.87-119.29, n = 10935) | 128.5 us |
| RX2TX entry -> lock (STIMER) | 85 ticks | **85 ticks** | (91 ticks with flat CPI 2) |
| RX2TX decision -> PA up | 138 ticks | **137 ticks** | |
| EOF -> SCAN_RSP SYNCSENT (dual, shift=3) | 180.5 (180.1-180.9) | **181.55 us** (181.52-181.68, n = 1689)* | 180.6 (SS7.72) |
| RX2TX entry -> M33 decision | 43.3 us | 19.0 us* (M33 at 1.0 cycle/insn) | |
| hwcfg advertiser, 20 s wall | 150 TX / 5 s | 150 TX / 5 s, no late/FATAL over 364 s virtual | late at 57 s |
| QEMU-config advertiser, 20 s wall | - | 150 TX / 5 s, no late/FATAL over 344 s virtual | panic at 19 ms (late 76 us) |
| dual: hwcfg advertiser + QEMU-config active scanner | - | **scanner panics at 3.69 ms**: "radio job completion ran 80883 ns late (61036)" | |

\* The active scanner panics (PANIC_ON_LATE, the BURTC SYNCBUSY defect another agent is
fixing) with the M33 at its measured 1.84 cycles/insn, so the dual-run rows used a scratch
build with the M33 at 1.0 cycle/insn (M0+ model unchanged); EOF -> SYNCSENT is set by the
M0+'s STIMER loop, so it is not sensitive to that.

[I] The remaining TXWARM (-5.4 us) and TX2RX (-4.5 us) deficits: each handler runs one
11-trial VCO calibration, and QEMU's SYNTH.LOCNTSTATUS never reports BUSY, so the LO-count
measurement time (~0.4-0.5 us per trial if it accounts for the whole gap) is missing. The
M0+ code segments that contain no hardware wait now match silicon exactly.

**6. Contradictions with firmware assumptions.**

1. efr32_seq_delay(n) is ~1.18-1.35 n us, not n us (section 3), and its duration depends on
   the loop's SEQRAM alignment.
2. The M33 runs compiled C at 1.84 cycles/insn (not 1); with that rate the QEMU-config
   active scanner misses its 61 us late-delivery tolerance at 3.69 ms.
3. Under per-CPU clocks neither advertiser build reaches a late delivery in QEMU over 340+ s,
   while silicon stage 3 logs ~15 per second (timer agent, late_count). So silicon still
   loses M33 time the model does not have [I]: instruction-cache misses (unmeasured), flash
   data, interrupt latency of flash-resident vectors, contention.

**7. Unmodelled / placeholders.**

- [P] LO-counter (VCO calibration) measurement time; SYNTH.LOCNTSTATUS.BUSY never set.
- [P] M33 instruction-cache misses; M33 CPI is one average (1.84, compiled C; library loops
  1.45-1.60); M33 flash data loads (+3.5) are RAM in QEMU.
- [P] M33 exception entry with flash-resident vectors (17 cycles assumed, from SRAM).
- M0+ exception return, M0+ SEQRAM store +0.5 in some loops, back-to-back radio accesses
  (4.25, modelled as 5), MUL timing (1 assumed).
- Contention other than SEQRAM (radio path, BUFC, SRAM, peripheral bus) — cross-core
  findings; occupancy counters are in place for a bus model.
- M33 EM2 stopping the M0+; the M0+ view of TIMER1/PRS/CMU/SRAM (QEMU's M0+ map has SEQRAM
  and radio only); RAC.STCTRL -> M0+ SysTick CALIB.
- Snapshot/migration of per-CPU time; adaptive -icount (shift=auto) falls back to the
  shared rate with a warning.

### 7.76 Scheduling: real priorities, one missed-deadline policy, admission control

Tags: **[F]** measured on silicon, **[I]** inferred, **[P]** placeholder (unmeasured).
Kernel: vendor/zephyr `kernel: sched: keep meta-IRQ threads cooperative within a priority`,
`kernel: deadline: one policy for missed scheduling deadlines`, `kernel: deadline: admission
control for deadline events`. Radio: modules/efr32-radio (hal/ll_threads.c, hal/cntr.c,
hal/radio.c, Kconfig, CMakeLists.txt). Notes and tooling: `scratch/scheduler-findings.md`,
`scratch/sched/`.

**1. Priorities.** The LLL context and every radio job (TX/RX launch, completion, QEMU TIFS)
run at meta-IRQ priority `K_HIGHEST_THREAD_PRIO + CONFIG_BT_CTLR_LLL_PRIO` (0), the ULL worker
(ticker worker and job, ULL mayflies) and the QEMU RX-descriptor drain at
`+ CONFIG_BT_CTLR_ULL_HIGH_PRIO` (1); `CONFIG_NUM_METAIRQ_PRIORITIES=2`. A due radio job
preempts a running ULL body. `mayfly_prio_is_equal()` follows Nordic's glue, so ULL->LLL
mayflies (the LLL prepares) are queued to the LLL context instead of running inline.

- [F] Before this, equal-priority meta-IRQ threads preempted each other by EDF at IRQ exit
  (kernel `should_preempt()`), against the documented meta-IRQ contract and the OpenISA LLL's
  equal-priority-IRQ assumption. Fixed in the kernel; deadline suite case
  `test_deadline_event_metairq_same_level_cooperative`.
- [I] OpenISA's preempt timeout named its preemptor by MFIFO slot and asserted when restarting
  an already scheduled timeout; with the LLL above the ULL an LLL-priority job due earlier can
  run between the ticker callback and `preempt()`. Patched at configure time as in Nordic's
  LLL (name by `prepare_param.param`, tolerate an already scheduled timeout).
- EDF keys are the instant the work fell due plus `EFR32_LL_START_TOLERANCE_CYCLES` (61036 ns,
  the existing clock-event miss tolerance): ticker compare -> compare time; mayfly and LLL
  doorbells -> now (previously the next ticker compare and `cntr_launch_deadline()`).
- Stacks [F]: LLL context 524 (adv) / 636 (scanner) bytes in QEMU, 820 on silicon with the
  bring-up trace; ULL worker 464 QEMU / 796 silicon; main 1008 silicon.

**2. Missed deadline -> safe shutdown.** The kernel checks every deadline-event job released
from idle at its start, and routes late clock events through the same
`SCHED_DEADLINE_MISS_POLICY`; the radio selects `SCHED_DEADLINE_MISS_CUSTOM`. Its handler
clears the PA enables (RAC.TX PA bits, PAENCTRL.PARAMP), withdraws SWRXEN, issues RXDIS and
TXDIS, writes CTRL with FORCEDISABLE and SEQRESET, clears the PA again, masks FRC/BUFC/
doorbell IRQs, cancels the controller's clock events, closes every wake path, suspends the LL
threads and prints the report from the system work queue.

- [F] Silicon, injected 2 ms overrun in the TX launch job at 8 s: completion 1.7 ms late,
  report names the TX job as running at release; afterwards M0+ TX counters +0 over 5 s,
  RAC.STATUS state 13, RAC.TX 0x00030000 (bleed bits only), PAENCTRL 0x10000000, host
  sightings 0 over 10 s (9 in 5 s before). CTRL reads back 0x381, without SEQRESET.
- [F] The bring-up trace's synchronous printk (immediate logging) in `ticker_fire()` delayed
  the first worker release 4.4 ms: `EFR32_RADIO_BRINGUP_TRACE=n` on hardware, TODO notices
  silent under immediate logging.
- [F] Stage 3 with the policy: 150 TX / 5 s, scan-response name received, miss_count 0 and
  late_count 0 over ~105 s.

**3. Admission control.** A budget (WCET per run) on a deadline event registers it; every
release from idle and every armed timed release is checked by simulating the scheduler's
policy (fixed priority, EDF within a priority, cooperative meta-IRQ levels, not-before
spins) over the admitted work plus one blocking allowance. A late job is reported as
`K_DEADLINE_MISS_UNSAT` before it runs, naming the job priority sacrifices. Timed releases
use the scheduler clock's delivery plan (`k_clock_event_sched_plan()`: deep LF with HF
start-up, running HF, an already armed expiry, or a spin; a spin is demand at its priority).

Execution budgets [F] (`CONFIG_SCHED_DEADLINE_STATS`, BGM220, stage 3, scheduler clock
33554432 Hz, host active scanning, four 60 s runs, one after zeroing at steady state):

| job | longest run | longest release-to-start |
|---|---|---|
| TX launch | 12.4 us (415 cycles, all runs) | 16.3 us |
| completion | 43.8-270.1 us | 17.7 us |
| LLL context | 178.0-179.9 us | 13.2 us |
| ULL worker | 344.8-346.6 us | 21.0 us |
| RX launch | [P] 24.8 us, never released in stage 3 | - |
| QEMU TIFS, QEMU RX drain | [P] 50 us each, QEMU-only | - |

Interrupt durations [F] (DWT, 76.8 MHz, steady state): TIMER0 clock event 39.4 us (90.6 us
only at boot), BURTC 34.2 us, FRC 20.1 us, LLL/worker doorbells 12.6/10.1 us. QEMU instruction
counts agree: clock-event ISRs 884-937 instructions on average, up to 1249.

[F] Result: with these budgets, the 39.4 us one-interrupt allowance and the 61 us start
windows, the radio level is not schedulable by analysis. In QEMU the first completion is
unsat (89.4 us of work ahead: the allowance and the 50 us TIFS placeholder); in the hwcfg
scanner a completion released during an LLL prepare has 219 us ahead. Runs never miss in
practice (0 misses on silicon, clean QEMU runs) because actual runs are far shorter than the
budgets and prepares do not overlap completions in these workloads. `EFR32_RADIO_ADMISSION`
is therefore off by default; it needs start-by deadlines derived from what each job must
achieve rather than one tolerance for all.

**4. Validation.** Kernel suites on native_sim/native/64: deadline 14/14 (also with
admission and stats), deadline_admission 11/11, clock_event_sched 13/13, deferred_release
5/5, clock_event 4/4, metairq, preempt, schedule_api. QEMU (`build/qemu-new`, per-core
clocks, dual machine, virtual seconds from the scheduler clock):

| configuration | shift 3 | shift 5 |
|---|---|---|
| QEMU-config adv + active scanner | 1135 s clean, 11338/11350 SCAN_RSP | 1139 s clean |
| hwcfg adv + hwcfg scanner | scanner shuts down at 11.3 s: ULL worker 1.2 us late | same, 11.3 s |
| hwcfg adv + QEMU-config scanner | scanner shuts down at 25.8 s: ULL worker 2.9 us late | - |
| single device, both builds | 28-29 s clean, ~30 TX/s | - |

The hwcfg failures are the policy working as specified: the ULL worker, now below the radio
jobs, is released 25-28 us after its ticker compare (clock-event interrupt) and waits 37-49 us
for completion and TIFS jobs, starting just past the 61 us window it has always had. With
ULL releases given the controller's own prepare margin instead (EVENT_OVERHEAD_START_US 300 us
[P in lll_vendor.h] less one ticker tick, 270 us; scratch experiment, not in the tree), the
hwcfg pair runs 2844 s / 2840 s clean at shift 3 / 5; the mixed pair then stops at 27.9 s on a
QEMU TIFS job that waited 34 us behind an LLL prepare at the same level, the conflict
admission reports.

Wrong turn: a release lead (arm early by the measured wake latency, then wait out the instant
with `k_deadline_event_post_at()`) on the QEMU TIFS and launch jobs made things worse: the
waits occupied the radio level for up to 56 us and pushed other jobs out of their windows.
Moving the QEMU RX-descriptor drain below the radio jobs fixed the TIFS misses instead.

Silicon (stage 3, `build-stage3-head`): 150 TX / 5 s, name received, 0 misses and 0 late
deliveries over ~105 s; injected 2 ms overrun -> safe shutdown (section 2); deliberately
infeasible build (`EFR32_RADIO_ADMISSION=y`, wake latency declared 100 us > 61 us window)
-> unsat at the first TX launch arm, 1.5 ms before its release, then M0+ TX counters +0,
RAC.TX 0, PAENCTRL 0x10000000, 0 host sightings over 10 s.

### 7.77 Deadlines from the Bluetooth specification

Tags: **[F]** measured on silicon, **[Q]** measured in QEMU, **[S]** Bluetooth Core
Specification v6.1 (extracts kept in `scratch/refdocs/extracts.md`), **[D]** controller design
value. Code: `include/hal/efr32/ll_deadlines.h` (spec constants and helpers), `hal/ll_threads.c`,
`hal/cntr.c`, `hal/radio.c`, `Kconfig` (`EFR32_RADIO_TIMING` and every measured time); kernel
(vendor/zephyr): per-publication budgets (`k_deadline_event_post_budget()`), admitted timed
releases keep their not-before bound, `k_clock_event_sched_active_target()`.

Section 7.76 gave every job one start-by deadline, "fell due + 61036 ns", which had no basis.
Each job's deadline is now the latest start that still lets the on-air behaviour it serves meet
the specification, worked back through measured paths. Nothing is added to either.

**Spec quantities [S].**

| name | value | source |
|---|---|---|
| T_IFS_150 | 150 us, end of one packet to the start of the next | Vol 6 Part B 4.1.1 |
| active clock jitter | instantaneous timings within 2 us of the average; applies to adv packet -> SCAN_REQ and SCAN_REQ -> SCAN_RSP | 4.2.1 |
| sleep clock jitter | within 16 us; applies to advInterval + advDelay and anchor points | 4.2.2, 4.4.2.2.1 |
| ADV PDU spacing | beginnings of consecutive ADV_IND in one event <= 10 ms | 4.4.2.3 |
| preamble + AA, LE 1M | (1 + 4) octets x 8 us = 40 us | 2.1 |
| shortest scan window | LE_Scan_Window 0x0004 x 0.625 ms = 2.5 ms | Vol 4 Part E 7.8.10 |
| receiver timing | "The clock used to time packet reception may have any accuracy" | Vol 6 Part B 4.2 |

**Measured quantities** (Kconfig, one value per platform, `EFR32_RADIO_TIMING_SILICON` /
`_QEMU`): silicon is stage 3 on the explorer kit with a host scanning and admission on, maxima
over 120 s after zeroing them 20 s after boot (DWT and scheduler-clock probes in a scratch
build, PRS/TIMER1 for the radio); QEMU is the three dual pairs at shift 3 for 120 s wall each.

| quantity | silicon | QEMU |
|---|---|---|
| TX launch: start -> TXEN hold (PRE) | 12.0 us [F] | 16.6 [Q] |
| TX launch: instant -> after TXEN store (HOLD_LATE) | 0.89 [F] | 0.68 [Q] |
| not-before wait exit -> start (SPIN_EXIT) | 5.8 [F] | 36.3 [Q] |
| wake latency (arm lead of timed commands) | 55.7 [F, 7.76]; every launch was dispatched before its bound | same |
| TX / RX launch run | 29.5 / 7.4 [F] (RX from `tests/radio-rx-port` on silicon) | 29.0 / 5.1 |
| completion run: TX->RX, RX with response, other RX, last TX, abort | 46.4, 38.9, -, 27.1, 267.5 [F] | 86.8, 45.6, 129.3, 186.8, 91.8 |
| completion start -> PENDING / -> RESPOND | 12.8 / 24.7 [F] | 54.4 / 43.2 |
| TX FRAMESENT -> completion post, unblocked | 1.0 + 1.04 + 9.5 = 11.54 [F] | (silicon) |
| RESPOND store -> sequencer at its RX2TX hold, PA up | 141 STIMER ticks = 29.4 us, all 44 [F] | 137 ticks |
| RX2TX hold | 650 ticks (`EFR32_SEQ_RX2TX_TX_AT_STIMER`, 7.71) | same |
| our T_IFS spread | 0.8 us (180.1-180.9, 7.71) [F] | - |
| longest interrupt, steady state (admission checks included) | 58.1 [F] (39.4 without admission; 95.1 at boot) | 72.4 |
| LLL / ULL run | 249.1 / 487.8 [F] | 266.9 / 439.6 |
| LLL doorbell -> LLL start | 13.0 + 43.9 [F] | 72.4 + 57.8 |
| QEMU TIFS job: PRE, HOLD_LATE, run | - | 36.4, 0.77, 152.7 |
| QEMU RX drain run (one record) | - | 264.2 |

Budgets of ULL-priority jobs are wall time that includes the radio level preempting them; the
ULL worker and the QEMU drain are therefore not admitted (their deadlines are checked when they
start), since admission already counts that level's own work.

**Derivation.** T is the instant a command is due; "post" the completion's publication (an
interrupt timestamp, taken before admission runs).

| job | serves | spec constraint | deadline (start-by) | silicon |
|---|---|---|---|---|
| TX launch | first PDU of an advertising event, a central's anchor | advInterval + advDelay held to 16 us [4.2.2] | timed command: not_before = T - PRE - SPIN_EXIT, start-by = T - PRE + 16 us - HOLD_LATE; armed the wake latency before not_before; TXEN held, interrupts locked, by polling TIMER0.CNT against the instant mapped onto the active provider | not_before T - 17.8 us, start-by T + 3.1 us; TXEN in [T, T + 0.89 us] |
| RX launch | opening a scan window | none on the receiver [4.2]; the window should be complete [4.4.3.1] and lasts >= 2.5 ms [E 7.8.10] | T + 2.5 ms - RX run | T + 2492.6 us |
| completion, TX then RX (ADV_IND of a scannable advertiser, hardware) | PENDING before the SCAN_REQ is recognised | SCAN_REQ starts >= T_IFS - 2 us after our last bit, recognised 40 us later [4.1.1, 4.2.1, 2.1] | post + 150 - 2 + 40 - FRAMESENT->post - one interrupt - start->PENDING | post + 105.6 us |
| completion, RX with response armed (hardware, RAC in RX2TX) | RESPOND early enough for SCAN_RSP at T_IFS | SCAN_RSP within 2 us [4.2.1], less our measured 0.8 us spread | RAC.STIMER (from frame end) limit 650 - 142 + 5 = 513 ticks (106.9 us); start-by = post + (513 - STIMER at post)/4.8 - start->RESPOND | about post + 71 us |
| completion, other kinds (RX without response, last TX, abort) | the event's next PDU | next ADV_IND within 10 ms [4.4.2.3]; in a scan window, 2.5 ms | last ADV TXEN + 10 ms - kind's run, or post + 2.5 ms - kind's run | >= 9.73 ms after the ADV_IND |
| LLL context | the prepare that arms a launch | TX launch's start-by: the prepare (same priority, cooperative) must end before it | ticker due + (XTAL 49 + START 9 - 1 ticks) - PRE + 16 us - HOLD_LATE - LLL run; due = the requesting worker run's due, or the doorbell for thread requests | due + 1493.5 us |
| ULL worker | ticker expiries, ULL mayflies | the LLL step above | LLL window - doorbell latency - ULL run | due + 948.8 us |
| QEMU TIFS, turnaround (QEMU builds) | the seam's TX->RX / RX->TX at its T_IFS instant | 2 us [4.2.1] about the seam's instant | timed command, as the TX launch with 2 us | [Q] not_before I - 72.7, start-by I - 35.2 |
| QEMU TIFS, no-response timeout | the next PDU | as "other kinds" | next-PDU rule less TIFS run, two interrupts and the abort completion | - |
| QEMU RX drain | gates the RX completion | as "other kinds" | next-PDU rule less drain run, two interrupts and the RX completion | - |

The prepare lead (EVENT_OVERHEAD_XTAL_US 1500 + EVENT_OVERHEAD_START_US 300) is a design value
[D] placeholder from the OpenISA LLL; it is not a deadline. The spec instant is the event's TXEN;
the lead only says how early the chain starts, and admission checks that it suffices.

The QEMU TIFS seam starts its 150 us after the LLL callback, not at the frame's end, so a QEMU
configuration's on-air turnaround is not spec-timed by construction; its callbacks therefore get
the "other kinds" rule and only the seam's own command is held to 2 us.

**Hold.** One read of the logical time takes ~2 us in QEMU and one read through the active
provider 1.09 us on silicon (TIMER0 SYNCBUSY wait) [F]: both quantise a hold to about the active
clock's whole tolerance. The instant is mapped once onto the active provider's count
(`k_clock_event_sched_active_target()`, rounding up) and TIMER0.CNT is polled directly (0.156 us
per read [F], 0.169 [Q]); `efr32_radio_hold_init()` checks TIMER0 is that provider.

**What else changed.**
- Completion deadlines and budgets are per kind (`k_deadline_event_post_budget()`): one budget
  for all made every turnaround unsat behind a 267 us close.
- The QEMU drain takes one record per run instead of a slice of half the old 61 us tolerance.
- `EFR32_RADIO_TODO` no longer prints from the LL contexts and radio jobs: its one-time printk
  in `radio_tmr_tifs_base_get()` and the chain-delay getters cost up to ~110 us in the first
  ADV_IND completion in QEMU, and with them gone the hardware-configuration advertiser in QEMU
  answers every SCAN_REQ of the QEMU-configuration scanner (6278/6278, was 2 of ~2500, 7.76).
- `CONFIG_CLOCK_EVENT_SCHED_LATE_TOLERANCE_NS` is no longer any job's deadline. It remains the
  scheduler clock service's delivery-fault bound for events without a deadline of their own
  (system timer, hardware RX timeout): one deep-sleep cycle (30518 ns, 7.73) plus one interrupt
  the delivering interrupt may wait behind: 88618 ns silicon, 102918 ns QEMU (was 61036, two
  deep cycles; QEMU delivered one 66.4 us late behind a 72 us interrupt).
- The RX2TX hold and STIMER rate moved to `include/hal/efr32/seq_abi.h`, shared by the M0+
  source and the M33 (the sequencer binary is byte-identical).
- RAM (hardware image, admission on): launch stacks back to the 448 B default, the 768 B having
  been for the bring-up trace; measured 208 B (TX) and 200 B (RX) peaks with admission [F].

**Feasibility.**
- Silicon: feasible; admission on by default. The tightest windows: the TX launch (3.1 us after
  T - PRE; every launch dispatched before its bound) and the RX-with-response completion (about
  71 us after post against 58.1 us of interrupt allowance and a 34.0 us longest
  release-to-start).
- QEMU: infeasible by analysis, so admission is off by default for QEMU-timed images. The
  model's longest interrupt (72.4 us) is longer than the hardware-configuration ADV_IND
  completion's window (46.6 us after post: 188 - 11.54 - 72.4 - 54.4 us) and the QEMU TIFS
  turnaround's (61.9 us after a release the seam's 150 us leaves too close to arm early). The
  model runs this firmware's radio paths 1.2-4x slower than silicon (start->PENDING 54.4 vs
  12.8 us, RESPOND 43.2 vs 24.7; the M33 at a flat 1.84 cycles per instruction, 7.75), so these
  are model gaps, not scheduling choices; priorities cannot resolve them (the blocking is one
  interrupt). Admission on in QEMU reports exactly these: "efr32_radio_completion_event ...
  deadline 46640 ns from the check, demand ahead of it 72419 ns" and "efr32_qemu_tifs_event ...
  deadline 61959 ns ... demand ahead 72419 ns".

**Verification.**
- Kernel, native_sim: deadline_admission 13/13 (two new cases: a publication's own budget, an
  admitted timed release keeping its not-before bound), deadline 14/14 (also with admission and
  stats), clock_event_sched 13/13, deferred_release 5/5 (also with admission), clock_event 4/4.
- Silicon, stage 3 (`EFR32_RADIO_ADMISSION=y`, silicon times): 75 s, 90 s and 75 s runs with a
  host scanning: miss_count 0, late_count 0, not halted, scan-response name received; M0+ per
  5 s: 150 TXWARM, 163 TX, 13 respond, 0 timeout. T_IFS (`scripts/bg22-tifs-probe.tcl`): EOF ->
  SCAN_RSP SYNCSENT 180.33 us (n = 1, 60 s) and 180.53-180.95 us (n = 5, 60 s), inside 7.71's
  180.1-180.9/stock 180.2-181.1 at timer resolution. Unchanged control image (7.76): 180.51-180.74
  (n = 3, 30 s).
- Silicon, deliberately infeasible (one interrupt declared 200 us): unsat at the first ADV_IND
  completion ("deadline -40620 ns from the check, demand ahead of it 200003 ns"), safe shutdown;
  then 0 TXWARM and 0 TX in 5 s, RAC.STATUS 0x0d000000, PAENCTRL 0x10000000.
- QEMU (admission off, the default for QEMU times; every start checked against the
  spec-derived deadlines, safe shutdown on a miss):

| pair | shift 3 | shift 5 |
|---|---|---|
| hardware-configuration advertiser + scanner | 1328.5 s clean | 1329.0 s clean |
| hardware-configuration advertiser + QEMU-configuration active scanner | 1247.4 s clean, 12474/12474 SCAN_RSP | 1244.6 s clean, 12445/12445 |
| QEMU-configuration advertiser + active scanner | advertiser stops at ~68 s: QEMU TIFS turnaround started 16.8 us past its 2 us-tolerance start-by, 88.5 us after its early release | same, ~68 s |

  Before this section the hardware-configuration pair stopped at 11.3 s and the mixed pair at
  25.8 s (ULL worker 1-3 us past the 61 us window), and the mixed pair answered ~0.2 % of
  SCAN_REQs; the QEMU-configuration pair ran clean only against the 61 us window, which the
  2 us active-clock tolerance replaces.
- QEMU with admission on: unsat at the first turnaround in every pair, as derived above.

**Open.** The QEMU M33 timing model (radio paths 1.2-4x slower than silicon) makes the
turnaround deadlines infeasible in QEMU; the QEMU-configuration seam cannot be spec-timed (its
T_IFS starts at the callback); the K_RX completion budget is the QEMU figure (never observed on
silicon); RX launch for a connection peripheral (window widening) and the connection roles are
not derived (their profiles count misses, QEMU only); the prepare lead (XTAL 1500 + START 300 us)
is still the OpenISA placeholder; the hardware scanner receives nothing on silicon.

### 7.78 The suit's part in QEMU; the stock image boots in the model

The suit control unit is a BGM220PC22WGA2 module (DEVINFO module name; the BGM220P data sheet
lists it with 352 KB flash and 32 KB RAM) around an EFR32BG22C222F352GM40 (DEVINFO.PART
0x021608AE: family BG, number 22, device 2222). The explorer kit the model was built from
carries a BGM220PC22HNA2 with 512 KB. The public part headers for C222F352 and C224F512
differ only in `FLASH_SIZE` (0x58000 vs 0x80000). The suit's dumps live only in the
proprietary baselines directory (`suit-backup-2026-10-31/`, analysis in
`suit-sim-2026-10-31/`).

**Board identity in the model (vendor/qemu).** Machine properties `devinfo=` and
`userdata=` take raw page images (1 KB each, RM Table 6.1: USERDATA at 0x0FE00000, DEVINFO at
0x0FE08000); the dual machine takes `devinfo0/1`, `userdata0/1`. Flash and RAM are sized from
DEVINFO.MSIZE (RM 6.4.2.4) as on silicon. Without the properties the board is the old
BGM220PC22HNA2 identity, so existing images and scripts are unchanged (qtests 98/98 apart
from the known-flaky `seq-sleeping-follows-wfi`; RF suite 24/24; the stage-3 dual run gives
the same ADV/SCAN_REQ/SCAN_RSP counts and the same pre-existing late start at the same
virtual time as before the change). An access past the flash end is unmapped, so it bus
faults: RM 4.2.1.2 and 6.6.4 say an access to an address not assigned to any device faults
while SYSCFG_CTRL.ADDRFAULTEN is set (its default). Whether the 352-512 KB hole of a 352 KB
part is "not assigned" is not stated in the RM; it needs a silicon read (TODO).

**What the stock image needed, and what was added [F unless marked].** Booting
`flash-a.bin` with its DEVINFO and USERDATA exposed these divergences, in order:

1. NVM3 erased every page of the suit's NVM3 area at the first boot. NVM3 writes the chip
   family into each page header, and the suit's pages carry 0x35, the RM's BG22 code for
   SYSCFG.CHIPREV.FAMILY (RM 6.6.6.4). The model returned CHIPREV = 0. CHIPREV.FAMILY now
   follows DEVINFO.PART through the RM's enumeration. MAJOR/MINOR stay 0 [U]: never read.
2. Flash programming: the MSC is modelled from RM 6.3.12 and 6.8 (WREN, ADDRB with INVADDR
   outside main flash and USERDATA, ERASEPAGE, WDATA programming that only clears bits with
   ADDRB += 4, PAGELOCKn, MISCLOCKWORD.UDLOCKBIT and MELOCKBIT, MSC_LOCK, IF/IEN on IRQ 49).
   [U] Erase and write complete at once: their duration and the flash read stall are
   unmeasured.
3. The app polls the RTCC (sleeptimer). RTCC (RM 15.3) counts at the CMU.RTCCCLKCTRL clock
   with CNTPRESC, compares CNT against the CCn output-compare values and wraps (IF.OF), IRQ 12.
   RM 15.3.1: RTCC writes take effect immediately. [I] IF.CCn sets on the edge where CNT
   becomes OCVALUE (RM Figure 15.3); BURTC on silicon fires one edge later (§7.73).
4. The app reads its battery through the IADC (single conversion of PD02, VDDX reference,
   then scales by a resistor divider). The IADC single queue is modelled (RM 24); a pin's
   voltage is board input, the machine property `analog=PD02=<mV>`, and VDDX is the board's
   3.3 V. [U] Conversion time.
5. The M33's RAC_SEQ line (IRQ 38) followed SEQIF & SEQIEN; with the stock M0+ enabling
   SEQIEN = 0x3FFF0000 that line never cleared and the M33 never left the handler. Both M33
   RAC lines come from RAC.IF & RAC.IEN (public RAC_S_field.py: bits 0-3 STATECHANGE,
   STIMCMPEV, SEQLOCKUP, SEQRESETREQ; bits 16-23 SEQ): RAC_RSM carries bits 0-3, RAC_SEQ
   carries IF.SEQ. The stock app enables only IEN bit 18 and services IRQ 38 by reading
   RAC.IF & RAC.IEN; stock runs on silicon with SEQIEN = 0x3FFF0000 and IRQ 38 enabled, so
   SEQIF cannot reach the M33. The open firmware does not use IRQ 38.
6. The stock M0+ HardFaulted reading EMU.TEMP at 0x40004088: the model's M0+ saw only SEQRAM
   and the radio window. §7.74's silicon map of the M0+ is now modelled: main SRAM at
   0x20000000, the non-secure peripheral window 0x5xxxxxxx, and the secure window 0x4xxxxxxx
   reading 0 without a fault (TIMER1.CNT at 0x4004C024 read 0 there). [U] Writes to the
   secure window are dropped.
7. RDMAILBOX: FLAGS read a constant 0 because the model had no M0+ peer. With the stock
   sequencer running, the stock M0+ posts on mailbox 0 (MSGPTR0, IF_SET, then waits for the
   flag to clear) and the M33 posts on mailbox 1. IF/IEN now latch; mailbox 0 drives the M33's
   RDMAILBOX IRQ 39 (stock M33 enables IEN there) and mailbox 1 drives M0+ IRQ 23 (the stock
   sequencer's vector table services mailbox 1 there).
8. EMU.RSTCAUSE read 0. It now reads POR after power-up, gains SYSREQ on an M33
   AIRCR.SYSRESETREQ and survives resets until EMU_CMD.RSTCAUSECLR (RM 12.3: read, then clear).
   [I] Causes accumulate until cleared. The bootloader and AppLoader honour the RAM reset
   word only with SYSREQ set.

**How far the stock image gets.** Gecko bootloader -> AppLoader (which finds no request and
jumps on) -> application. The application initialises clocks, NVM3 (its data is kept), GPIO,
two USARTs in synchronous mode, I2C0 (devices at 7-bit 0x20 and 0x69 get no answer: the I2C
controller and the board's devices are not modelled, so each transfer runs to its poll limit),
the IADC battery read, RTCC, RAIL (the radio configuration and the stock M0+ sequencer
load; the mailbox handshake completes), then runs its main loop (watchdog feed, LED GPIO,
periodic ADC). It never advertises: the Bluetooth stack never delivers its boot event, so the
application never creates an advertising set. [I] The stack waits on the protocol timer: it
programs PROTIMER (CTRL, PRECNT top, START, compare channels 0 and 1, IEN 0x03001804), and the
model's PROTIMER raises no compare or wrap flag (TODO: PROTIMER is not publicly documented).

**OTA in the stock application [F, static].** The application's GATT database has the OTA
control characteristic as a user-handled, write-only attribute with no security requirement
(handle 66, in service handle 64). The OTA handler (Silicon Labs' OTA DFU component pattern)
acts on any write to that handle whatever the value: it answers the write with success,
records an OTA request, sets the AppLoader's advertised name to the device's own name and its
OTA configuration flag 1, and closes the connection itself; on the connection-closed event it
resets with the Bluetooth stack's system reset in OTA-DFU boot mode. There is no precondition
(no bonding, no value check, no state). The ATT error 0x0E the host saw is BlueZ's, not the
suit's: BlueZ completes a request that is still pending when the link drops with an error
response and no PDU (`src/shared/att.c` `disc_att_send_op`), which `gatt-client.c`
`process_error` turns into BT_ATT_ERROR_UNLIKELY (0x0E). The suit closes the link right after
queueing its Write Response, so the host reports 0x0E. The follow-up scan matched the same
address and the name "OWO..." (the AppLoader is told to advertise the device's own name), and
service discovery then timed out; that is consistent with the suit being in AppLoader. Whether
it was is unproven (no capture of that advertisement).

**AppLoader entry in the model.** With the RSTCAUSE model:
- RAM word 0x00000002 at 0x20000000 + AIRCR.SYSRESETREQ: bootloader -> AppLoader, which sees
  SYSREQ and word 2 and stays (it reached its radio start-up). This is the AppLoader's own
  request format.
- 0xF00F0202 (`bootloader_rebootAndInstall`, BOOTLOAD) + SYSRESETREQ: the suit's bootloader
  tries its own install, fails, counts retries in the RAM word (signature 0xABC0 plus count)
  with reasons 0x0203/0x0204, and finally starts AppLoader with a word that is not 2, which
  boots the application. So the open firmware's current OTA trigger (docs/ota.md, which
  writes 0xF00F/0x0202) would not reach AppLoader on this bootloader in the model. [I] that
  silicon does the same: the failure is the bootloader's own storage path, not a modelled
  peripheral, but it was not run on the suit.
- AppLoader then runs (its RAIL cycles the RAC through SHUTDOWN and back to OFF with the
  stock sequencer) but never transmits in the model: no advertisement, so no connection, and
  the AppLoader OTA of `stock-from-backup.gbl` could not be run in simulation. The blocker is
  the same Bluetooth stack start-up as in the application. (Before items 6-7 the AppLoader sat
  in SHUTDOWN, waiting for OFF, with the M0+ in HardFault.)

### 7.79 Connection roles: anchors from received frames, spec-derived deadlines

Tags as in 7.77 ([F] silicon, [Q] QEMU, [S] Core v6.1, [D] design value, [P] provisional: a
QEMU figure standing in for an unmeasured silicon one). Code: `hal/radio.c`, `hal/cntr.c`,
`include/hal/efr32/ll_deadlines.h`, `include/hal/efr32/radio_conn.h` (new: what a connection
event tells the HAL), `CMakeLists.txt` (two LLL patches), `Kconfig`, `sequencer/radio_seq.S`;
tests `tests/radio-port` (`src/gatt.c`, `Kconfig`, `prj-connection.conf`), `tests/radio-rx-port`
(`src/central.c`, `prj-central.conf`, `run-qemu-connection-e2e.sh`). No kernel or QEMU change.
Tooling and logs: `scratch/conn/` (build.sh, dual.sh, meas.sh, tally.sh, ktest.sh).

**Why connections never held.** `radio_tmr_aa_get()`, `radio_tmr_ready_get()` and
`radio_tmr_end_get()` returned 0. The Peripheral's ULL resynchronises its anchor from
`aa - ready` against `EVENT_JITTER_US + EVENT_TICKER_RES_MARGIN_US + windowWidening + 40`
(`ull_drift_ticks_get()`), so every received event moved its anchor ~90 us early, and the first
transmit window was placed from a CONNECT_IND end of 0. Three further faults sat behind it, each
seen in the QEMU RAC trace of the hardware-configuration pair:
- The launch instant dropped the ticker remainder (`cntr_radio_cmp_set()` used the tick only):
  anchors moved by up to one tick (30.5 us) from event to event, against the sleep clock's
  16 us (4.2.2).
- An RX window opened from OFF (a Peripheral's anchor, an initiator's scan) was never marked
  PENDING, so the RAC went RX2RX and no response (CONNECT_IND, the Peripheral's reply) could
  follow; and its RX2TX had no synthesizer: the open sequencer powered it only in TXWARM, so the
  TX after such a window never ended (FRC stalls without a started synthesizer, 7.60) until the
  next prepare forced the RAC off.
- A cancelled RX2TX (the advertiser declining a CONNECT_IND) left the TXPD handler waiting out
  its PA ramp-down bound for a PA that never ramped: the RAC sat in TXPD ~18 ms in QEMU and the
  first connection event's prepare waited behind it.

**Frame timing from RAC.STIMER [F mechanism].** Every receive in a connection event, and an
advertiser's CONNECT_IND, is armed for a response, so at the frame's end the RAC enters RX2TX and
holds it until the sequencer acknowledges (7.65). RAC.STIMER restarts at every state change and
counts 4.8 ticks/us (7.66, 7.68); the RAC leaves RXFRAME 3.2-3.4 us after MODEM EOF (7.72,
`EFR32_RADIO_RX_EOF_TO_EXIT_NS` 3.3 us, `_MAX` 3.4 us). The FRC interrupt therefore dates the
frame: EOF = now - STIMER - 3.3 us, read with RAC.STATUS before and after (both RX2TX). RXDONE is
raised at the EOF, so the interrupt first waits, bounded by the 3.4 us, for the RAC to leave
RXFRAME (without that wait the capture missed every connection frame in QEMU and the anchor
drifted again). The access address ended (PDU + CRC) x 8 us earlier (2.1). Event-timer
microseconds count from the start tick of `radio_tmr_start()` (`efr32_radio_start_cyc`), as
OpenISA's EVENT_TMR does; `radio_tmr_ready_get()` is the planned ready time (instant + ready
delay: the launch is held to its instant). All receive-side times are the MODEM's: d_rx is not
separated from d_tx (7.71), and it delays the derived anchor and the opened window alike. No QEMU
change was needed: RAC.STIMER, its restart and the 3.3 us exit are modelled from these silicon
measurements; PROTIMER captures are not used. Measured result in QEMU: aa - ready 128-148 us per
event against the ULL's expected 16 + 32 + ww + 40 = ~138 us.

**Ready delays.** TX: TXEN to the first preamble bit, silicon 91.0 + 0.65 = 91.65 us [F, 7.72],
QEMU 85.65-86.43 + 0.65 = 87.08 us [Q, 2004 launches]. RX: SWRXEN (RAC in OFF) to RXSEARCH, QEMU
3.121 us in all 2003 launches [Q]; silicon unmeasured [P]. The Central's ULL schedules its anchor
TXEN the TX delay early and the Peripheral opens its window the RX delay early. Chain delays stay
0 (unmeasured split, see above).

**Spec quantities added [S].**

| name | value | source |
|---|---|---|
| T_MCES | 150 us, end of the Peripheral's last packet to the next anchor; no Frame Space Update here, so T_IFS_ACL_CP/PC = 150 | Vol 6 Part B 4.1.5, 4.1.1 |
| windowWidening | transmitterAllowance (txCA x (windowEnd - lastSync) + J, J = 16 us sleep clock) + receiverAllowance; listen from ww before receiveWindowStart until ww after receiveWindowEnd; ww < connInterval/2 - T_MCES | 4.2.4, 4.5.7 |
| transmit window | starts 1.25 ms (CONNECT_IND) + WinOffset after the CONNECT_IND's end, lasts WinSize (1.25 ms .. min(10 ms, connInterval - 1.25 ms)) | 4.5.3, 4.5.5 |
| anchor | the Central transmits at the anchor (sleep clock, 16 us); events connInterval apart (7.5 ms .. 4 s); the Central closes events T_MCES before the next anchor | 4.5.1, 4.2.2 |
| packets in an event | T_IFS apart, active clock 2 us | 4.2.1, 4.5.1 |
| closing | MD bits; two invalid CRCs or a missed packet close the event | 4.5.6 |
| supervision | connSupervisionTimeout 100 ms .. 32 s, > 2 x (1 + latency) x connInterval; 6 events to establish | 4.5.2 |
| instants | connection update, channel map, PHY update apply at connEventCount == Instant; the Central allows >= 6 events | 5.5 |

**Derivation (connection jobs).** T: a launch instant (start tick + remainder); post: the
completion's publication; at: the RAC.STIMER read.

| job | serves | constraint | start-by | QEMU | silicon [P unless F] |
|---|---|---|---|---|---|
| Peripheral RX window launch (timed; `efr32_dl_rx_window_*`) | receiver searching before the earliest Central packet | searching by receiveWindowStart - windowWidening [4.2.4, 4.5.7]; ull_peripheral/ull_conn place SWRXEN EVENT_TICKER_RES_MARGIN_US + EVENT_JITTER_US + window_widening_event_us + ready delay before the window start, and ww = window_widening_event_us + J, so the tolerance is EVENT_TICKER_RES_MARGIN_US (`efr32_radio_rx_window_set()`, patched into lll_peripheral.c) | not_before T - RX_PRE - SPIN_EXIT; start-by T - RX_PRE + 32 us - RX_HOLD_LATE; SWRXEN held to T | T - 43.5 / T + 23.55 us | T - 13.0 / T + 23.55 us |
| Central anchor TX launch | anchor | 16 us sleep clock [4.2.2] | as 7.77, instant now with the remainder | 7.77 | 7.77 [F] |
| RX->TX completion (`lll_conn_isr_rx`, both roles, every receive) | RESPOND before the sequencer's T_IFS hold | 2 us active clock [4.2.1] | at + (513 - STIMER)/4.8 us - CONN_TO_RESPOND (133.0 us) | always before `at`: infeasible | infeasible by analysis with the QEMU path |
| TX->RX completion (`lll_conn_isr_tx`: Central after its anchor packet, Peripheral after an MD reply) | PENDING before the reply is recognised | T_IFS - 2 + 40 us [4.1.1, 4.2.1, 2.1] | post + 188 - 11.54 - one interrupt - CONN_TO_PENDING (99.2 us) | post + 4.9 us | post + 19.2 us, then admission's 58.1 us allowance: unsat |
| closing completion (last TX, abort) | the connection's next event | its prepare must start by the LLL window; the next event's launch is at least connInterval/2 + T_MCES - J after this one's [4.2.4] | T + connInterval/2 + 134 us - LEAD_MAX + LLL window - close run (241.7 us) | T + connInterval/2 - 436.9 us | T + connInterval/2 - 414.7 us |
| window close (HCTO, initial window) | listen until ww after the window's end | the latest packet (End + ww + 16) is detected 40 + d_rx + 3.5 us later, d_rx <= 9.45 [F 7.71]: by End + ww + 69 us | OpenISA's HCTO is End + ww + 72 us on the event timer; armed absolute (`cntr_rx_timeout_arm_at()`); it can only fire late and never ends a frame being received | - | - |
| supervision, instants, channel map | ULL control flow at prepare and done | ms-scale timers and event counters [4.5.2, 5.5] | no radio job of their own: the prepare chain carries them; a connection update reopens a transmit window through the same Peripheral window rule | - | - |

`connInterval` reaches the HAL from the patched prepares (`efr32_radio_conn_event_set()`);
`radio_reset()` forgets it. LEAD_MAX is (XTAL + START + 1) ticks: the launch remainder after the
borrowed tick is under a tick. The prepare chain's LLL window now takes the earlier of the TX
launch's and the RX window's latest start (the TX launch's on both platforms: unchanged).

**Prepare lead (EVENT_OVERHEAD_XTAL_US 1500 + START_US 300).** No spec quantity applies: it is
how early the chain starts. Its minimum (launch at least (XTAL + START - 1) ticks = 1739.5 us
after the expiry) must cover the chain: compare interrupt + worker release-to-start + ULL run +
LLL doorbell + LLL run + the launch's wake latency + PRE + SPIN_EXIT:
silicon 39.4 + 21.0 + 487.8 + 56.9 + 249.1 + 55.7 + 12.0 + 5.8 = 927.7 us [F, advertising];
QEMU 72.4 + 38.7 + 485.6 + 130.2 + 266.9 + 55.7 + 16.6 + 36.3 = 1102.4 us [Q, connection pair].
From deep sleep the HFXO (276 us) and DPLL (302 us) restarts [F 7.73] would add 578 us: 1505.7 us
on silicon. The lead is therefore sufficient by analysis, by 234-812 us on silicon and 637 us in
QEMU; it stays a design value [D], and admission checks each event against it. Missing to set it
from measurement: whether the HF restart sits on the radio's chain at all (an LF wake to TXEN
measured in a deep-sleep build), and silicon ULL/LLL runs in connection events.

**Sequencer changes.** RX2TX after a window opened from OFF (RADIOEN.PREEN clear: the synthesizer
was powered down by the last TXPD/RXPD) now powers up, calibrates (and caches) and starts the
synthesizer as TXWARM does, PA still off until RESPOND (`efr32_seq_synth_power_up`, split from
`efr32_seq_synth_start`, which TXWARM still calls); after a TX->RX turnaround the retune with the
cached calibration is unchanged. `efr32_seq_pa_stop` skips the ramp-down wait when PARAMP was
never set. Both are QEMU-verified only; on silicon the RX side of a window opened from OFF is
still not tuned (the hardware scanner receives nothing, 7.77), so a silicon Peripheral cannot
receive at its anchor yet.

**Measurements [Q]** (`EFR32_RADIO_TIMING_PROBE` + `SCHED_DEADLINE_STATS`, hardware-configuration
Peripheral + Central, shift 3, 120 s wall; maxima over both devices): completion runs in
connection events TX->RX 125.3, RX->TX 136.6 (Central 134.6), last TX 53.1, abort 241.7 (Central
217.1) us; paths to PENDING 99.2, to RESPOND 133.0 (Central 121.2) us; held RX launch PRE 7.2 us,
run 69.6 us, hold lateness 48 TIMER0 counts at 38.4 MHz = 1.25 us (from a run without the PRE
probe's extra clock read, which inflates it); LLL 222.5 us, ULL worker 485.6 us, completion
release-to-start 61.8 us. Advertising kinds in the same images are within 7.77's. Silicon
defaults for every connection figure are these, marked PROVISIONAL in Kconfig.

**Feasibility.** In QEMU every connection-event turnaround misses its spec-derived start-by: the
model's M33 runs `lll_conn_isr_rx` to RESPOND in 133 us, past the ~100 us the sequencer's T_IFS
hold leaves after the interrupt, and `lll_conn_isr_tx` to PENDING in 99.2 us against 4.9 us of
slack after one 72.4 us interrupt. Priorities cannot help: the work is the callback itself. The
connection still holds because the sequencer waits up to 120 us for the decision (the reply then
goes out up to ~40 us late: T_IFS ~190 us on air in QEMU, a spec violation the deadlines report).
With the provisional figures silicon is infeasible by analysis too, so a silicon Peripheral with
admission on (the silicon default) would stop at its first connection event: the real silicon
paths decide it (advertising ran 1.2-4x faster on silicon than in QEMU, 7.77). If they do not fit,
the fix is architectural (stage the reply before the frame ends and complete SN/NESN in the
sequencer, or run the RX callback from the interrupt), not a wider tolerance.

**Verification.**
- Kernel (native_sim/native/64, unchanged kernel): deadline 14/14 (also with admission and
  stats), deadline_admission 13/13, clock_event_sched 13/13, deferred_release 5/5, clock_event 4/4.
- QEMU connection pair (hardware configuration, `prj-connection.conf` + `prj-central.conf`; GATT
  write and read-back of a 4-octet characteristic once a second; misses counted, and attributed by
  a scratch tally in the missed-deadline handler, `scratch/conn/radio.c.with-tally`), 900 s wall:

| | shift 3 | shift 5 |
|---|---|---|
| virtual time / connection events (50 ms) | 3882 s / 77 636 | 3896 s / 77 923 |
| disconnections | 0 | 0 |
| GATT rounds ok / failed | 3210 / 0 | 3240 / 0 |
| Peripheral misses: RX->TX / TX->RX / close | 77 637 / 6 475 / 9 | 77 924 / 6 499 / 9 |
| Central misses: RX->TX / TX->RX | 77 628 / 77 638 | 77 915 / 77 925 |
| launch misses (TX, RX window), unsat | 0, 0 (admission off, the QEMU default) | 0, 0 |

  Every miss is one of the two turnarounds above (worst: Peripheral RESPOND 99.5 us late, Central
  PENDING 33.8 us late) or a close behind them: when RESPOND comes after the sequencer's 120 us
  decision bound, the reply is dropped and the event stays open until the next prepare aborts it,
  whose close is then ~25 ms past its start-by (9 times; under the safe-shutdown policy the RESPOND
  miss would stop the radio first). No launch missed: the Peripheral's held windows and the
  Central's anchors all started in time. Before these changes every connection dropped with
  0x08/0x3E within 0.5-6 s.
- `run-qemu-connection-e2e.sh` (now asserts a sustained link and GATT): pass at shift 3 (20 s wall,
  30/30 GATT rounds) and shift 5 (40 s wall, 90/90).
- Injected infeasibility: Peripheral with `EFR32_RADIO_RX_LAUNCH_HOLD_LATE_NS` (since §7.82 `EFR32_RADIO_SEQ_TIMED_HOLD_LATE_NS`) declared 80 us
  (beyond the window's 32 us tolerance), safe-shutdown policy: the first Peripheral window launch
  started 15.5 us past its start-by, safe shutdown (RAC 0x1d000000, PA off), the Peripheral silent
  afterwards, the Central reports 0x3E. With admission on, QEMU reports the first ADV_IND
  completion unsat as in 7.77 (44.7 us window, 72.4 us allowance), before any connection exists.
- Advertising regressions (QEMU): hardware-configuration advertiser + QEMU-configuration scanner
  clean (shift 3, 120 s wall, 7112/7112 SCAN_RSP; shift 5, 4704/4704); the QEMU-configuration
  pair stops at 69.5 s (7.77: ~68 s on a TIFS turnaround), now on an advertiser completion 5.9 us
  past its next-ADV_IND start-by, posted ~10 ms after the event's last ADV_IND (696/696 SCAN_RSP
  before; not root-caused here: that configuration's seam is not spec-timed, 7.77); hardware
  advertiser + hardware
  scanner: the hardware scanner now sends SCAN_REQs (its windows are armed for a response), so
  this pair now exercises the advertiser's SCAN_REQ RESPOND against a spec-timed scanner, which
  QEMU cannot meet (7.77): the advertiser stopped after 54 s on a RESPOND completion 1.8 us past
  its start-by, released 39 us earlier behind interrupts (540 of 541 SCAN_REQs answered). The
  stage-3 silicon image builds at 99.80% RAM.

**Silicon measurements needed** (no hardware now; procedures as `scratch/sched/`):
1. RX ready delay: SWRXEN to RXSEARCH, PRS RACL.ACTIVE rise and RACL.RX rise into TIMER1
   (`scripts/bg22-tifs-probe.tcl` generalised, 7.72 method d), once the sequencer tunes the
   receiver for a window opened from OFF (prerequisite: such a window receives on silicon).
2. Connection-event paths: an `EFR32_RADIO_TIMING_PROBE` + `SCHED_DEADLINE_STATS` +
   `SCHED_DEADLINE_MISS_COUNT` build of the Peripheral, a host Central connected for 120 s,
   `efr32_radio_probe`, the LL contexts' stats and `miss_count` read over OpenOCD (as
   `scratch/sched/wcet*.tcl`): completion runs per connection kind, CONN_TO_PENDING/RESPOND, held
   RX launch PRE/run/hold lateness (TIMER0 counts).
3. Frame dating error: MODEMH.EOF captured by PRS into TIMER1 against the HAL's
   `efr32_radio_rx_eof_cyc`, per connection event.
4. Peripheral window margin: its MODEML.FRAMEDET against its RACL.RX rise, per event, versus
   windowWidening.
5. Connection T_IFS: EOF -> SYNCSENT of the Peripheral's reply (`bg22-tifs-probe.tcl`).
6. The sequencer's RX2TX power-up: SysTick probe (7.72 method e) from RX2TX entry to lock, against
   the 650-tick hold; TXPD after a cancel no longer waiting.
7. Prepare lead: LF wake to TXEN in a deep-sleep build (HF restart on the chain or not).

**Open.** Turnaround callbacks too slow for T_IFS in QEMU and (provisionally) on silicon; silicon
RX from OFF untuned; chain delays (d_rx/d_tx split) unmeasured; T_MCES and the Central's event
length are not enforced by the OpenISA LLL beyond its slot reservation; 2M/Coded PHYs not
supported (PHY update stays on 1M); the QEMU-configuration seams cannot run a spec-timed
connection (no air time; T_IFS from the callback).

### 7.80 Silicon receive from OFF; data channels; the first silicon connection

Tags as in 7.79. Board: BGM220 explorer kit (J-Link OB 000440200548). Code: `sequencer/radio_seq.S`
(RXWARM handler), `hal/radio.c`, `CMakeLists.txt` (one LLL patch), `Kconfig`; `tests/radio-rx-port`
gains a hardware profile (`prj-hardware.conf`, `boards/bgm220_ek4314a.overlay`);
`tests/radio-port/src/gatt.c` prints link events. QEMU: two `vendor/qemu` commits. Tooling and logs:
`scratch/silicon-rx/` (build.sh, hw.sh, prs-probe.tcl, chcrc.sh, recover2.sh, ...).

**Why a window opened from OFF heard nothing [F].** The open sequencer had no IRQ1 (RXWARM)
handler: the common tail acknowledged STATERXWARM and the RAC went to RXSEARCH with nothing
powered. Hardware passive scanner, 3 s: RXWARM, RXSEARCH, RXPD 30 each, RXFRAME 0. A dump of the
radio blocks held in that RXSEARCH (M33 halted) against our own RXSEARCH after a TX->RX turnaround
(the path that receives, 7.64-7.66): `RAC.SYEN` 0 (0x1FBF), `SYLOEN` 0 (3), `RFPATHEN` 0 (6),
`LNAMIXEN` 0 (1), `IFADCDBG` 0x0C (0x1F), `RAC.RX` 0 (0x6D6), `RAC.PRESC` 0 (7), `SYNTH.STATUS`
0x4 (0x6: not started, IF off), `PGACAL` uncalibrated.

**Fix.** Stock's IRQ1 (`fn_0x23a0`) opens with the same RAC.PRESC pulse as its TXWARM, then
dispatches stock's job machinery (not ported, 7.3). The open RXWARM now runs the PRESC pulse
(`efr32_seq_presc_pulse`), brings the synthesizer up exactly as TXWARM does
(`efr32_seq_synth_power_up`, TX calibration cached for this channel, so an RX2TX from this window
retunes with the cache as after TX2RX), then the receive half of TX2RX (`efr32_seq_rx_tune`, split
out of `efr32_seq_rx_turnaround`). RXSEARCH is reached in the TX2RX register state by construction.
Result [F]: the hardware passive scanner reports advertising from the devices around it (1165
reports in 40 s; no miss).

Two more defects found on the way [F]:
- OpenISA's scanner arms RX->TX for every window, a passive scanner's included. The armed window
  gives each received frame the T_IFS RESPOND deadline; admission found it unsat at the first
  advertisement (window 53.6 us after release against a 58.1 us interrupt allowance) and shut the
  radio down. A passive scanner never responds: `lll_scan.c` is patched to arm only for an active
  scanner or an initiator (`efr32_scan_switch()`).
- The RX->TX decision word (SEQRAM 0x4FA4) survives an M33 reset like the rest of SEQRAM: a
  PENDING left by the previous image made the sequencer request TX at every frame of a window
  never armed (an RX2TX and a decision timeout per frame). The HAL clears it with the image
  install.

**RX ready delay [F]** (7.79 item 1): PRS RACL.ACTIVE rise (SWRXEN with the RAC in OFF) reload-
starts TIMER1, RACL.RX rise captured (`prs-probe.tcl`; TIMER1 on HFRCODPLL 76.8 MHz,
EM01GRPACLKCTRL = 1, calibrated in 7.71): **166.55-166.76 us**, n = 899 (12791-12807 ticks).
`EFR32_RADIO_RX_READY_NS` silicon 166760 (was the QEMU 3121 placeholder).

**LO-count measurement time [F] and the QEMU model.** The model's RXWARM ran 155.5-156.3 us. The
known [I] gap (7.75: QEMU never reported `SYNTH.LOCNTSTATUS.BUSY`) was measured: a scratch
sequencer stamped the M0+ SysTick (38.4 MHz) from just after `LOCNTCTRL.RUN` to the first poll that
reads BUSY clear, 200 calibrations: 70 cycles at NUMCYCLE 5 (rarely 71), 40 at 4 (some 30), 20 at 3
and at 2 (the instrumented poll's floor). [I] BUSY lasting 2^(NUMCYCLE+1) HFXO periods reproduces
all four after the poll's granularity; modelled. Model after (dual run, shift 3): RXWARM
166.01-167.63 us (silicon 166.55-166.76), TXWARM 90.96-92.52 (91.03), TX2RX 124.50-125.51
(123.4-123.7; the model's PA ramp-down uses the 3.96 us median of a 0.81-3.96 us range).
`EFR32_RADIO_RX_READY_NS` QEMU 167630. The model also now detects a frame only with the receive
chain up (synthesizer started in IF mode, `RFPATHEN.LNAMIXEN`, `IFADCDBG.IFADCENCLK`; which one is
individually required is [I]); before, it received with all of them off.

**Data channels [F mechanism].** `radio_freq_chan_set()` knew only the three advertising channels
and returned without touching the synthesizer for every data channel, so no silicon connection
could follow its hop sequence. QEMU took the medium channel from `SYNTH.CHSP`, which the HAL did
not update either, so a QEMU pair kept meeting on the last advertising channel. The captured words
0x01F46AAB, 0x01F96AAB, 0x0204AAAB (2402, 2426, 2480 MHz) are each round(f x 2^19 / 38.4 MHz)
exactly: SYNTH.FREQ is the carrier in units of f_HFXO / 2^19 (public field FREQ, 28 bits). The HAL
now computes it for every channel; the model takes the carrier from FREQ (+ CHCTRL x CHSP, [I],
zero in every observed configuration). QEMU connection pair with hopping: 240/240 GATT rounds.

**Silicon receive is channel-dependent: an open blocker [F].** Per-channel CRC counts in the
hardware scanner (`FRC.WHITEINIT` channel at RXDONE, 20 s): ch37 1586 ok / 475 fail (77 %), ch38
16 / 1557 (1 %), ch39 1 / 204 (0 %). The Peripheral, the laptop actively scanning at about
-46 dBm: ch37 8 / 7, ch38 0 / 15, ch39 0 / 2. Failing frames mostly carry a correct header and go
wrong part way through (SCAN_REQs whose AdvA bits flip). This is why CONNECT_INDs rarely arrive.
Ruled out, each on silicon: VCO calibration (our per-channel results, recorded by channel and mode,
equal stock's three clusters 0x8334, 0x82CC, 0x81E6); HFXO tuning (stock runs the same `XTALCTRL`,
0x83959508); carrier offset (`MODEM.FREQOFFEST` mean 8.5, 8.7, 14.4 on ch37, 38, 39; stock about 9
on the laptop's SCAN_REQs); whitening (headers decode); interference (a strong close signal fails
the same); live pokes of stock's `AGC.CTRL0`, `MODEM.IRCALCOEFWR1`, `DIGMIXCTRL`, `BLEIQDSA` and the
retime limit fields (no change). Enabling RAC clock retiming (`XORETIMECTRL`/`DIGCLKRETIMECTRL`
ENRETIME with stock's RXSEARCH limits) in the RX tune wedged the debug port within milliseconds of
every boot (recovered: `scratch/silicon-rx/recover2.sh fpb`: J-Link nRESET, attach at release,
DHCSR halt and an FPB comparator on the reset handler, then a normal flash). Stock runs its CPU
from the HFXO (38.4 MHz), ours from HFRCODPLL (76.8 MHz); a build with SYSCLK on the HFXO left the
radio silent and was not pursued. Next: a stock RX capture on ch38 (AP3 watchpoint, as 7.64) and a
diff against ours.

**RX FIFO overflow [F]** (the ram-fit question): with the M33 halted 2 s while the scanner
received, frames filled the 512-octet FIFO (WRITEOFFSET 512, READOFFSET 0), `BUFC.IF` read 0x100
(channel 1 overflow) and the RAC sat in RXOVERFLOW (6) with no state event pending (SEQACTIVE
clear, the M0+'s IRQ6 never ran), and stayed there after the M33 resumed: the radio stopped. RXDIS,
SWRXEN withdrawn, BUFC CLEAR, `FRC.CMD.RXABORT` and clearing `BUFC.IF` each left it in RXOVERFLOW;
FORCEDISABLE went to OFF through SHUTDOWN and the radio resumed once it was cleared. The HAL has no
recovery path, so the RX FIFO must stay large enough never to fill (512 octets, consumed per
frame). Modelled in QEMU: a frame that does not fit writes what fits, raises BUF1OF, no RXDONE,
RXOVERFLOW without a SEQIF event; FORCEDISABLE leaves it.

**Jobs waiting on a frame in progress [F].** Hardware scanner, probe build: RX completion 75.7 us
(the Kconfig silicon budget had the QEMU 129.3 us), but ABORT completions ran 1206.9 us and RX
launches 1106.4 us: `efr32_radio_rx_start()` spins on `radio_is_idle()` while the RAC finishes a
frame it was told to abandon (an instrumented build saw a 297 us wait entered in RXFRAME,
`RAC.STATUS` 0x23400000). RXDIS does not end RXFRAME; FORCEDISABLE does (debugger, 5/5: RXFRAME to
OFF at once). Not fixed yet: the scanner's abort and launch budgets are exceeded on silicon whenever
a window closes during a frame. Also fixed for measurement: the probe recorded a completion under
the kind of a later publication (an abort the callback raises republishes the mailbox mid-run), so
7.77's ABORT maximum may hold RX runs; it now records the kind dispatched.

**First silicon connection [F].** Peripheral image (radio-port hardware profile, GATT test, probes,
admission off, misses counted) against the laptop's controller: in the third attempt the
Peripheral accepted a CONNECT_IND (`peripheral_connected: 0`) and dropped at the supervision
timeout (`0x08`); the host reported `le-connection-abort-by-local`; 8 misses counted. Figures from
that connection (TIMING_PROBE, scheduler clock 33554432 Hz):

| figure | silicon | QEMU (7.79) |
|---|---|---|
| conn completion TX->RX run | 143.7 us | 125.3 us |
| conn completion RX->TX run | 109.3 us | 136.6 us |
| conn close/abort run | 31.1 us | 241.7 us |
| path to PENDING (conn) | 95.1 us | 99.2 us |
| path to RESPOND (conn) | 103.2 us | 133.0 us |
| held RX launch PRE / run | 10.3 / 14.0 us | 7.2 / 69.6 us |
| held RX launch hold lateness | no hold waited | 1.25 us |
| adv completion RX->TX (CONNECT_IND) | 52.5 us | 45.6 us |

Kconfig silicon defaults now take these (the close budget keeps the QEMU 241.7 us, as no abort after
a receive was seen; the hold lateness stays PROVISIONAL).

**Feasibility (admission verdict).** RX->TX in a connection event: RESPOND must land by
EFR32_RX2TX_RESPOND_BY_STIMER, about 106.9 us after the frame end, and the completion is posted
about 11.5 us after it (FRAME_END_TO_COMPLETION_POST): 95.4 us for a 103.2 us path, before any
interrupt allowance. **Infeasible on silicon by measurement**, as 7.79 predicted. TX->RX (PENDING):
188 - 11.5 - 95.1 = 81.4 us of slack against one 58.1 us interrupt: feasible. A silicon Peripheral
with admission on would stop at its first connection event, correctly. The fix is the architectural
one 7.79 names (stage the reply before the frame ends, the empty PDU and SN/NESN completed by the
sequencer, or the RX callback at interrupt level), not a wider tolerance; not done here.

**Not measured (7.79 list).** Items 3-7 (EOF dating error, window margin, connection T_IFS, the
sequencer's RX2TX power-up, the prepare lead in deep sleep) need a connection that holds, which the
channel-dependent receive above prevents. The ram-fit TX launch stack watermark and the 64-octet
TX FIFO byte-exact check were not reached.
### 7.81 Link-layer encryption on RADIOAES

Tags as in 7.79. Details, sources and measurements: `docs/crypto.md`. The controller's AES (the
session-key ECB in the ULL worker, link-layer CCM in the completion job and a Central's prepare)
runs on the RADIOAES accelerator, called directly (`crypto/radioaes.c`); the host's ECB and CMAC
reach it through PSA Crypto. Encrypted connections now work in QEMU (they never had: no CCM, no
heap), which exposed two OpenISA-LLL contract points in `hal/radio.c`: a received PDU is now
decrypted (or, if empty, copied) into the LLL's buffer before the LLL callback, because
`lll_conn.c` reads SN/NESN first; and `radio_ccm_is_available()` is always true (the RV32M1's
"CAU free" meaning), where the old answer had stalled `LL_START_ENC_RSP`.

Deadlines: with `BT_CTLR_LE_ENC` the connection RX->TX completion budget and its path to RESPOND
gain decrypt + encrypt + one lower-priority accelerator run, a Central's LLL budget an encrypt, and
the ULL window the session-key ECB (Kconfig `EFR32_RADIO_CCM_DECRYPT_NS`, `_CCM_ENCRYPT_NS`,
`_AES_BLOCKING_NS`, `_ECB_NS`; silicon 15.69, 15.25, 5.37, 6.30 us for 27-octet PDUs [F];
`include/hal/efr32/ll_deadlines.h`). [Q] encrypted GATT: Peripheral RX->TX run 164.3 us, path to
RESPOND 159.9 us (unencrypted 136.6, 133.0); Central 160.5, 146.9 (134.6, 121.2).

### 7.82 The sequencer keeps the instants: timed launches and replies staged before the frame ends

Tags as in 7.79 ([F] silicon, BGM220 explorer kit, J-Link OB 000440200548; [Q] QEMU, this tree's
`vendor/qemu`, per-core clocks; [S] Core v6.1). Code: `sequencer/radio_seq.S`,
`include/hal/efr32/seq_abi.h` (the shared words), `hal/radio.c`, `hal/cntr.c`,
`include/hal/efr32/ll_deadlines.h`, `include/hal/efr32/radio_conn.h`, `lll/lll_conn_stage.c`,
`lll/lll_adv_stage.c`, `CMakeLists.txt` (LLL hooks), `Kconfig`; `tests/radio-port`
(`RADIO_PORT_ADV_CHANNEL_37_ONLY`). No kernel or QEMU change. Tooling and logs (worktree scratch,
the shared scratch being write-protected for this agent): `scratch/m0-timing/` (build.sh, dual.sh,
hw.sh, timed.tcl/timed.sh, tifs.sh, probe.sh).

**The problem.** Two failures had one cause: the M33 either busy-waited to an exact instant with
interrupts enabled, or decided a turnaround in software after a frame ended, and an interrupt or
the software path itself made it miss the specification.
1. A timed launch was released early and spun in the kernel's not-before wait until its instant;
   a TIMER0 clock-event interrupt (39.4 us, 90.6 us at boot, 7.76) inside that spin overshot the
   launch's start window.
2. A connection receive had to be answered (RESPOND) about 95.4 us after the completion was
   posted, and the measured path to RESPOND was 103.2 us before any interrupt allowance (7.80).
   The advertiser's SCAN_REQ turnaround had the same shape: on silicon with today's tree its
   window at the check was 56.1-56.4 us against the 58.1 us interrupt allowance, so admission
   stopped the advertiser at its first SCAN_REQ, before and after the launch change [F]
   (`deadline unsat: efr32_radio_completion_event ... deadline 56415 ns from the check, demand
   ahead of it 58114 ns`, master image, 1.47 s after boot).

**Timebase [F].** The sequencer waits on TIMER0, the scheduler clock's active provider, read
through the non-secure peripheral window (`TIMER0_NS_BASE` 0x50048000, CNT at +0x24; 7.74 found
the M0+ reaching TIMER1 there and the secure window reading a constant). Chosen over RAC.STIMER
(restarts at every RAC state change, 16 bits, 4.8 ticks/us against BURTC: 0.21 us resolution and a
rate known to two digits) and PROTIMER (start encoding not public): TIMER0 is the counter the M33
already maps instants onto (`k_clock_event_sched_active_target()`, 7.77), so the instant needs no
second calibration, and it counts at 76.8 MHz on silicon. Evidence: the sequencer's own
TIMER0 reads around 742 launches (post to first poll, instant to the command store, below), and
the limit word, `LIMIT - TARGET` = 1149 counts for 16 us less the hold-late, which is
76.8 MHz exactly. The M0+ wakes on RDMAILBOX1 (FLAGS +0x40 with SET/CLR aliases, IEN +0x44,
M0+ IRQ 23, bus clock `CMU.CLKEN1.RDMAILBOX1`; the layout is the stock stack's, 7.78 item 7):
[F] IRQ 23 entries count one per advertising event (+29 in 3 s next to +87 TXWARM).

**Design.**
- *Timed launches* (`efr32_seq_timed_post()`, `efr32_seq_timed_command`). The launch job does
  every preparation it did before (FIFO, packet stage, SR0 handshake, RX FIFO), maps the instant
  and a limit onto TIMER0, writes TARGET, LIMIT, then OP (TXEN, with SWRXEN after it for a TX that
  turns to RX; or SWRXEN for a Peripheral's window) and sets the mailbox flag. The sequencer polls
  TIMER0 and the OP word (the M33 cancels by clearing it), issues the command at the instant, and
  refuses it past LIMIT (instant + spec tolerance - hold-late), which the M33 reports as a missed
  start (safe shutdown). The not-before spin is gone: the job is released one interrupt allowance
  before its start-by and only posts.
  start-by = T - PRE - SEQ_WAKE + tolerance - SEQ_HOLD_LATE (16 us for a launch, the window
  tolerance for a Peripheral's receive), `ll_deadlines.h`.
- *Connection replies staged* (`lll/lll_conn_stage.c`, built into the derived `lll_conn.c`).
  Before each connection receive (the prepares, and the end of every `isr_rx()` that leaves the
  event open) the link layer works out, without changing its state, the two PDUs `isr_rx()` can
  answer with: CUR (no acknowledgement or a CRC error: the last PDU again, or the empty PDU) and
  NEXT (acknowledged: `isr_rx_pdu()`'s queue advance, then `lll_conn_pdu_tx_prep()`), with their
  MD, LLID, fragment length and, on an encrypted link, their CCM ciphertext (NEXT at the counter
  the acknowledgement makes: the AAD masks NESN, SN and MD, Vol 6 Part E 2.3.3, so the sequence
  bits can be set later). In RX2TX the sequencer reads the received header and the FRC CRC
  trailer from the RX FIFO and decides by `isr_rx()`'s rules (ack = CRC ok and NESN != SN;
  accept = CRC ok, SN == NESN and an RX buffer free as staged; done = second CRC error in a row,
  or CRC ok, MD clear and an empty reply; a Central that is done sends nothing), writes the reply
  with SN ^ ack and NESN ^ accept to the BUFC TX FIFO and turns around at its RX2TX hold; when the
  event continues it arms the next receive itself (SWRXEN, decision word STAGED). `isr_rx()` then
  runs unchanged except for three hooks: the RX-buffer test uses the staged answer, the TX prep
  puts the staged choice in place (`lll->empty`, the head fragment's length, MD, LLID: the queue
  may have grown meanwhile), and a MIC failure ends the event at once (the reply already left).
  The HAL asserts that the link layer's acknowledgement, SN and NESN equal the sequencer's.
  Decrypt-before-callback (7.81), retransmission on a CRC error and the empty PDU are unchanged.
- *Advertiser's scan response staged* (`lll/lll_adv_stage.c`): with no scan-request filter,
  privacy or scan-request reports, the sequencer answers a SCAN_REQ (type, 12 octets, CRC, RxAdd,
  AdvA, as `isr_rx_sr_adva_check()`) with the SCAN_RSP and nothing else; the HAL asserts that
  `isr_rx()` answers exactly the same frames.
- While a staged reply may be on air the HAL rewrites no framing or PHY register (FRC.WCNTCMP0 is
  reset by the sequencer before the reply) and never clears its TXDONE.
- Deadlines (`ll_deadlines.h`, all from Core v6.1 and measured paths): a connection receive's
  completion now only has to re-arm the RX FIFO before the next frame can begin
  (EOF + 2 x 148 + 80 us) and post the next stage before that frame can end
  (EOF + 2 x (148 + 80) us); the transmit completion before it must end before that receive's
  completion is due. The scan response's completion serves the event's next ADV_IND (10 ms).

**Measurements [F]** (stage-3 advertiser, laptop scanning, 70 s, admission on; Peripheral,
host Central on channel 37):

| quantity | silicon | QEMU |
|---|---|---|
| TX launch start -> post | 20.4 us (685 cycles, 742 launches) | 14.4 us |
| RX window launch start -> post | 15.2 us | 10.9 us |
| post -> sequencer's first poll | 136 counts, 1.77 us | 116 counts at 38.4 MHz, 3.02 us |
| instant -> TIMER0 read after the TXEN store | 111 counts, 1.45 us max; 76-86 (n = 200, PRS run) | 2.21 us |
| sequencer polling ahead of the instant (spin) | >= 4222 counts, 55.0 us | 64.3 us (connection) |
| RAC leaving OFF (PRS RACL.ACTIVE) -> that TIMER0 read | 70 TIMER1 ticks, 0.91 us, all 200 | - |
| SCAN_REQ EOF -> SCAN_RSP SYNCSENT (bg22-tifs-probe.tcl) | 180.51-180.95 us (n = 8), 180.30-180.79 (n = 17) | - |
| connection: received EOF -> our empty PDU's SYNCSENT | 179.54-181.68 us (n = 16) | - |
| sequencer takes the stage after RX2TX begins | <= 17 STIMER ticks, 3.5 us | 15 ticks |
| connection completion start -> next stage posted | 46.1 us (few events) | 67.7 us (Peripheral), 33.9 (Central) |
| connection receive completion run (commit + stage) | 50.0 us | 75.9 us |
| SCAN_REQ completion run (commit) | 47.0 us | 23.9 us |

So TXEN reaches the RAC 0.1-0.5 us after its instant (the 0.91 us between the edge and the read
that follows the store, less the read itself), never before, within the 16 us the sleep clock
allows, and no M33 interrupt can move it. The scan response and the connection reply both leave at
the stock-matched T_IFS (7.71: stock 180.2-181.1 us, our hold 180.1-180.9 us).

**Admission on silicon figures (before -> after).**
- Advertiser, silicon: before, unsat at the first SCAN_REQ (above); after, stage 3 with
  admission on ran 60 s with the safe-shutdown policy and 70 s counting, 0 misses, 0 late
  deliveries, the host receiving the scan-response name.
- Connection, silicon figures in QEMU images (`EFR32_RADIO_TIMING_SILICON`, admission on, safe
  shutdown on the device under test): before, the RX->TX turnaround could not be met by analysis
  (7.80); after, see the runs below, 0 misses on the Peripheral, also encrypted.
- Connection on silicon: the Peripheral accepted the laptop's CONNECT_IND on channel 37 and
  answered every connection-event frame at T_IFS from its staged replies (above), with admission
  on and 0 misses, but the link still ended at the supervision timeout (`0x08`, the host reporting
  a connection timeout): the stage that followed showed the CRC-error flag set, i.e. the receive
  quality problem of 7.80, which another agent is chasing.
- Not feasible yet: a scanner or initiator answering an advertisement (SCAN_REQ, CONNECT_IND) is
  still decided by the M33 (its reply depends on filter policy and, for CONNECT_IND, on the
  frame's timing). With silicon figures in QEMU the Central's initiator was found unsat when its
  completion was released while the LLL context ran a prepare (cooperative, 249 us budget), both
  before and after this change; and with QEMU figures every turnaround the M33 still decides is
  infeasible (72.4 us interrupt allowance, 7.77). The product is a Peripheral; the test Centrals
  run with misses counted.

**QEMU** (`EFR32_RADIO_TIMING_SILICON`, admission on, device under test with the safe-shutdown
policy, peer counting misses; virtual seconds from the scheduler clock):

| pair | shift 3 | shift 5 |
|---|---|---|
| Peripheral + Central (GATT write/read-back) | 2584.6 s, 1980/1980 rounds, Peripheral 0 misses | 2650.9 s, 2010/2010, 0 misses |
| the same, LE Secure Connections passkey pairing (level 4), encrypted GATT | 2556.3 s, 1950/1950, 0 misses | 2625.6 s, 2010/2010, 0 misses |
| advertiser + active scanner | 141.6 s, 5667 advertiser / 1416 scanner (SCAN_REQ) TX, 0 misses | 143.1 s, 5727 / 1431, 0 misses |

(600 s wall each; no disconnection, no late delivery. The Central peer counted one miss in every
connection run, the initiator case above; the scanner peer none.)
| injected: one interrupt declared 200 us | unsat at the first ADV_IND completion (`deadline -39815 ns from the check, demand ahead of it 200003 ns`), safe shutdown, 1 TX | - |

Kernel suites (native_sim/native/64, kernel unchanged): clock_event_sched 13/13, deadline 14/14
(also with admission and stats), deferred_release 5/5, clock_event 4/4, deadline_admission 13/13.
qtests: efr32bg22-test 98, rf 27, dap 9, board 7, unit test-jlink-swd-io 3, all pass.

**QEMU model.** The model already has what silicon showed: RDMAILBOX1 raising M0+ IRQ 23
(7.78) and the M0+'s non-secure peripheral window reaching TIMER0 (7.74, 7.78): 4721 timed
launches in a 485 s pair ran through them. It runs the sequencer's timed path slower than
silicon (post to first poll 3.02 vs 1.77 us, hold-late 2.21 vs 1.45 us: the M0+ handler and
its peripheral reads under the per-core clock model), and its sequencer took the connection
stage as early as silicon (15 vs 17 ticks after RX2TX began).

**Open.** Silicon connection hold-up (receive quality, 7.80); scanner and initiator responses
still decided by the M33; the QEMU M33 timing gap (7.77); budgets of the new completion paths
come from few silicon connection events (`EFR32_RADIO_CONN_COMPLETION_TO_STAGED_NS` is marked
provisional; the receive completion keeps 7.80's longer 109.3 us budget); a qtest for the
mailbox-to-M0+ path.
### 7.83 Channel-dependent receive: the demodulator's sample rates follow the channel

Tags as in 7.79. Board: BGM220 explorer kit (J-Link OB 000440200548). Code: `hal/radio.c`
(`efr32_modem_channel_rates_set()`, called from `radio_freq_chan_set()` and `radio_phy_set()`;
diagnostic `CONFIG_EFR32_RADIO_RX_CHANNEL_STATS`); QEMU: `hw/arm/efr32bg22.c`, qtest
`released-rx-srcchf-follows-channel`. Tooling, logs and the fits: `scratch/ch38/` of the
worktree (`lib.tcl`, `late3.tcl`, `poll.tcl`, `fit.py`, `verify.py`, `stock-src.txt`); stock
dumps only in the baselines directory (`ch38/`).

**Root cause [F].** The IF ADC is clocked from the LO, so its sample rate moves with the channel
(3.3 % from 2402 to 2480 MHz), and two MODEM registers that the PHY table gives in ADC samples are
right only on the table's base channel, 2402 MHz. Stock recomputes both on every channel; the
open HAL left the 2402 MHz values everywhere. On 2426 MHz the demodulator therefore ran 1 % off
rate and on 2480 MHz 3.2 %: the header decodes, the timing error then exceeds a symbol part way
through the frame and the CRC fails -- exactly 7.80's symptom.

**How it was found.** Stock's RX state was captured in RXSEARCH on each advertising channel.
Stock takes no RXSEARCH (IRQ2) interrupt in most windows and the J-Link round trip (about 1 ms)
is longer than one channel's TX and RX, so: reset to the application entry, attach the M0+ (AP3)
without halting it, pre-load M0+ DWT comparator 0, poll RAC state and `SYNTH.FREQ` until stock is
in TXWARM/TX on another channel, enable the comparator (one write) on `RAC.SR1`'s SET alias (the
first store of every stock handler), follow handler entries until IRQ11 (STATETX2RX) runs on the
wanted channel, move the comparator to the `RAC.SEQIF` CLR alias and resume until the
acknowledgement lands; the RAC is then in RXSEARCH with the M0+ halted and the blocks are dumped
over the M0+ bus (`RAC.STATUS` 0xB2000002 before and after). Between stock's own ch37, ch38 and
ch39 captures the only configuration words that differ are `SYNTH.FREQ`, `CHSP`, the VCO
calibration, `RAC.PGACAL` (DC calibration, run per turnaround), and **`MODEM.SRCCHF` and
`MODEM.DIGMIXCTRL`**; ours carried the 2402 MHz words on every channel. To get more than three
points, the laptop connected to the stock image and the M33 view of `SYNTH.FREQ`, `SRCCHF` and
`DIGMIXCTRL` was sampled non-intrusively during the connection (a triple kept only when FREQ read
the same before and after): 23 channels, 2402-2480 MHz.

**The rule, fitted to all 23 channels exactly [F]** (`verify.py` replays the HAL's integer
arithmetic: 23/23 words equal):
- f_LO = f_RF + f_IF (`SYNTH.IFFREQ.LOSIDE` high side; f_IF = IFFREQ x f_HFXO / 2^19, the unit of
  `SYNTH.FREQ`); f_ADC = f_LO / 2^9 with `RAC.IFADCTRIM0.IFADCENHALFMODE` (the 1M PHY), f_LO / 2^8
  without (2M, Coded: [I], consistent with their tables, unmeasured on silicon).
- `DIGMIXCTRL.DIGMIXFREQ` = round(floor(f_IF in Hz) x 2^20 / f_ADC). Without stock's truncation of
  f_IF to whole hertz 3 of the 23 words round the other way.
- `SRCCHF`: SRCRATIO1 / 2^7 x SRCRATIO2 / 2^14 = f_ADC / f_SRC_OUT, f_SRC_OUT = 4 MHz for 1M (4
  samples per symbol). SRCRATIO1 is the largest code that leaves stage 2 at or above 25/24 (the
  code steps between 2404/2418, 2426/2450 and 2464/2466 MHz bracket that limit to
  1.0409-1.0417; 25/24 is the round value inside), SRCRATIO2 rounds the remainder. Coded runs with
  SRCENABLE1 clear: SRCRATIO2 / 2^14 alone = f_ADC / f_SRC_OUT.
- Applied to each PHY table at 2402 MHz the rule returns the table's own SRCCHF for 1M, 2M (8 MHz)
  and Coded (8 MHz), and the 2M/Coded DIGMIXFREQ; the 1M table's DIGMIXFREQ is 156405, stock's
  runtime value 156406 (the HAL follows stock).

**Ruled out on the way [F].** The CPU clock: a scanner with SYSCLK on the HFXO (38.4 MHz, as stock;
`cpu0` clock-frequency matched, admission off) received like the 76.8 MHz DPLL build (ch37/38/39
79/70/68 % vs 68/70/70 %, both with the fix). 7.80's "HFXO build left the radio silent" was not
reproduced with admission off. Stock does not enable RAC clock retiming in RXSEARCH either
(`XORETIMECTRL` 0x770: limits only, ENRETIME and RESETN clear; `DIGCLKRETIMECTRL` 0); 7.80's wedge
came from setting ENRETIME. The remaining non-channel differences against stock's ch38 capture
(`AGC.CTRL0`, `MODEM.IRCALCOEFWR1`, `BLEIQDSA`, `MODEM` +0xC4/+0xCC/+0x21C, `RAC.PGACTRL`
PGAENSUBG, `SYNTH` +0x78, `FRC.RXCTRL`/`TRAILRXDATA`) are the same on ch37, which now receives as
well as the others; not chased here.

**Result [F]** (`efr32_radio_rx_channel_stats`, CRC result per channel at RXDONE):

| image | ch37 (2402) | ch38 (2426) | ch39 (2480) |
|---|---|---|---|
| scanner, before, run 1 (30 s) | 1369 ok / 382 fail (78 %) | 20 / 1262 (2 %) | 0 / 94 (0 %) |
| scanner, after (30 s, between the two) | 1359 / 447 (75 %) | 1019 / 258 (80 %) | 925 / 236 (80 %) |
| scanner, before, run 2 (30 s) | 1213 / 478 (72 %) | 21 / 1263 (2 %) | 0 / 131 (0 %) |
| Peripheral, before, laptop scanning (3 runs) | 42 / 7 | 0 / 48 | 0 / 6 |
| Peripheral, after, laptop scanning (4 runs) | 49 / 2 (96 %) | 45 / 14 (76 %) | 50 / 15 (77 %) |

Same build configuration on both sides (only the rate computation disabled), interleaved; the
scanner's remaining ~20-25 % failures are equal on all three channels (weak neighbours). In a
connection the Peripheral received data channels 26, 29 and 33 with 6 of 7 CRCs good; the link
still ends at the supervision timeout (7.80's RX->TX response path, unrelated). Per-channel
rates on all 40 channels were not measured: the laptop's channel map and the short-lived
connections give only a few data channels; the stock capture covers 23.

**QEMU.** The model now checks the demodulator rate at frame end: from `SYNTH.FREQ`, `IFFREQ`,
`IFADCTRIM0` and `SRCCHF` it computes the relative SRC error, and a frame whose accumulated drift
over 8 x (octets + 3) symbols exceeds half a symbol gets its CRCOK trailer bit clear ([I] model of
a measured effect: the threshold, the demodulator's timing tracking and the DIGMIXFREQ frequency
error are not modelled; an SRCCHF without SRCENABLE2 is not checked). The old HAL therefore loses
2426/2480 MHz frames in QEMU as on silicon. With the fixed HAL the QEMU connection pair (hardware
configuration, hopping data channels, `run-qemu-connection-e2e.sh 25`) completes 60/60 GATT rounds;
qtests 98/98 and 28/28 (new `released-rx-srcchf-follows-channel`).

**Open.** 2M and Coded rates are derived from their tables, not measured on silicon; the 25/24
stage-2 limit is a bracketed fit, not a documented constant; all-40-channel rates need a longer
lived connection or a test transmitter.

### 7.84 Silicon connections hold: GATT, LE Secure Connections, and the measurements they allow

Tags as in 7.79. Board: BGM220 explorer kit (J-Link OB 000440200548), the laptop's controller
(hci0, BlueZ 5.87 `bluetoothctl`) as Central. Code: `tests/radio-port/prj-hardware.conf` (no libc
heap), `tests/radio-port/prj-pairing.conf` (the product's controller privacy, accept list and GATT
caching settings), `modules/efr32-radio/Kconfig` (silicon figures). No HAL, sequencer, kernel or
QEMU change. Tooling and logs: `scratch/sil-conn/` of the worktree (build.sh, hw.sh, run.sh,
conntest.sh, meas.sh, meas.tcl, gattcheck.sh, addr.sh, ww.sh, eof-probe.patch); the shared scratch
was write-protected for this agent.

**Images.** Hardware profile (`prj-hardware.conf` + `prj-connection.conf`, + `prj-pairing.conf`
for the pairing runs) with the hardware policy throughout: admission on, a missed or infeasible
deadline shuts the radio down (`SCHED_DEADLINE_MISS_CUSTOM`; the kernel still counts misses in
`miss_count` and late deliveries in `late_count`, both read over the debugger after each run),
advertising on channels 37, 38 and 39 (`RADIO_PORT_ADV_CHANNEL_37_ONLY` off),
`EFR32_RADIO_RX_CHANNEL_STATS` on. Measurement images add `EFR32_RADIO_TIMING_PROBE`,
`SCHED_DEADLINE_STATS` and `INIT_STACKS`. The device takes a new random static address at every
boot (no stored identity); each run reads it from `bt_dev.id_addr[0]` over the debugger
(`addr.sh`) before the laptop connects, and the test application advertises once per boot, so each
run starts from a reset.

**Workload.** `bluetoothctl` scans, connects to that address, (pairing runs: passkey entry as
`agent KeyboardOnly` with the test's fixed passkey), discovers the test service and then writes a
4-octet value to the test characteristic and reads it back once a second until the end, then
disconnects (`conntest.sh`). Every read-back is compared with the write before it
(`gattcheck.sh`). The laptop chose a 45 ms interval (`efr32_radio_conn_interval_us` 45000) and a
channel map of 26 data channels (0-10, 22-36).

**Two configuration faults, found on silicon.**
- The pairing image did not link: RAM overflowed by 3092 octets. The hardware profile kept a
  3504-octet libc heap for the host's PSA ECB/CMAC contexts; since 7.81 those run on RADIOAES and
  the linker discards malloc, calloc and free from every radio-port image (`zephyr.map`): the
  arena was unreferenced. Now 0; pairing image 95.6 % RAM.
- The pairing image shut the radio down at the laptop's first SCAN_REQ, before any connection:
  `deadline unsat: efr32_radio_completion_event ... deadline 55640 ns from the check, demand ahead
  of it 58114 ns`. `BT_SMP` defaults `BT_CTLR_PRIVACY` on, and with controller privacy the
  sequencer cannot stage the scan response (`lll_adv_stage.c` needs no resolving list), so the M33
  decides the SCAN_REQ at its end and that turnaround is infeasible on silicon (7.82's "before").
  Admission was right. The product (`prj.conf`) has controller privacy, the filter accept list and
  GATT caching off; the pairing profile, which claims the product's security, now matches it
  (GATT caching also logged `CMAC operation init failed -134`: its database hash needs multi-part
  CMAC, which `radioaes_psa.c` does not offer).

**Results [F].**

| run | image | connected | GATT rounds (exact) | misses / late | data-channel frames ok / CRC fail | end |
|---|---|---|---|---|---|---|
| L1 | Peripheral, 7.83 figures | 10 min 43 s | 620 (620) | 0 / 0 | 14294 / 0 | host disconnect (0x13) |
| P2 | pairing, level 4, 7.83 figures | 10 min 43 s after pairing | 620 (620) | 0 / 0 | 14495 / 0 | host disconnect |
| L2 | Peripheral, 7.84 figures | 10 min 42 s | 616 (616) | 0 / 0 | 14260 / 0 | host disconnect |
| P3 | pairing, 7.84 figures | 10 min 42 s after pairing | 621 (621) | 0 / 0 | 14473 / 0 | host disconnect |
| M2, M3, C1, C2 (measurement) | probes | 5 min, 3.3 min, 5 min, 5 min (C2 encrypted) | 295, 196, 294, 290 (all exact) | 0 / 0 | - | host disconnect |

Pairing: LE Secure Connections only, MITM enforced, passkey entered on the laptop, the
characteristic requiring an authenticated encrypted link (`BT_GATT_PERM_*_AUTHEN`): every read and
write of the run went over link-layer AES-CCM on RADIOAES at security level 4 (SC + authenticated
+ 128-bit key; the device printed `peripheral_paired: bonded 1`). The bond the laptop made for that
run's address was removed afterwards (`bluetoothctl remove`, that address only, checked against the
device's name and the debugger-read identity).

**Per-channel receive [F]** (`efr32_radio_rx_channel_stats`, the four 10-minute runs together):
every data channel the link used, 0-10 and 22-36, received 57522 frames with 0 CRC failures
(per channel 9 to 998 frames per run: `scratch/sil-conn/stats-*.txt`); the advertising channels in
the same runs (SCAN_REQs and CONNECT_INDs from the laptop and neighbours) 60 ok / 7 failed. 7.83's
rate rule therefore holds on 26 of the 37 data channels; 11-21 were outside the laptop's channel
map.

**Measurements [F].** Probe images (k_cycle at 33.554432 MHz; TIMER0 counts at 76.8 MHz; TIMER1
on HFRCODPLL 76.8 MHz, `EM01GRPACLKCTRL` 1, calibrated in 7.71). "Clean": the probe read only
after the link closed; M2/M3 also polled the debug port during the connection.

1. *Connection T_IFS* (`scripts/bg22-tifs-probe.tcl`, PRS MODEMH.EOF reload-starts TIMER1, our
   reply's SYNCSENT and FRAMESENT captured, 30 s of a held link, M2): received EOF to our reply's
   SYNCSENT **179.91-180.87 us, n = 941** (empty replies, 40 us on air: n = 863, 179.93-180.87;
   80 us: 39; 112 us: 39). Stock 180.2-181.1 us (7.71); 7.82 (n = 16) 179.54-181.68 us.
2. *Frame-dating error* (7.79 item 3; scratch patch `eof-probe.patch` in `efr32_radio_rx_end_capture()`:
   with TIMER1 reload-started by MODEMH.EOF, the true time since EOF is TIMER1.CNT read right after
   RAC.STIMER, the HAL's estimate STIMER/4.8 + EOF_TO_EXIT 3.3 us; histogram in 50 ns bins, M3):
   **-0.84 to +0.38 us, mean -0.26 us, n = 667** (85 % within -0.45..0 us); the estimate is early by
   the read gap between the two loads plus STIMER's 0.21 us step. Far inside the 16 us sleep-clock
   tolerance the anchor needs. (A first version read TIMER1 with its bus clock off and took a bus
   fault at the first received frame: the patch now checks `CMU.CLKEN0` bit 5.)
3. *Peripheral window margin* (7.79 item 4; PRS RACL.RX rise reload-starts TIMER1, MODEMH.EOF
   captured, 30 s, M3; MODEML.FRAMEDET as signal 7 of source 0x2D, pulse or level, captured
   nothing and needs the BG22 PRS signal table): RX ready to the first frame's EOF 123.5-158.9 us
   (n = 580 empty first PDUs; the longer tail is data PDUs), so the receiver was searching **43.5 to
   78.9 us (mean 60.5 us) before the Central's preamble** (EOF less 80 us of preamble, AA, header and
   CRC). Planned: ready EVENT_TICKER_RES_MARGIN_US 32 + EVENT_JITTER_US 16 + window widening before
   the computed anchor; the widening, read from `conn_pool` (`lll.periph.window_widening_periodic_us`,
   offsets from the image's DWARF) was 34 us per event (our SCA 500 ppm, the laptop's SCA code 2),
   so 82 us. The Central's packets therefore began 3.1-38.5 us (mean 21.5 us) before our computed
   anchor, within its 34 + 16 us allowance; the 30 us width of that spread is one 32768 Hz tick (the
   laptop's own scheduling granularity is the likely source). Open: whether the 21.5 us mean is our
   anchor (chain delay, `radio_tmr_ready_get()`) or the laptop's.
4. *The sequencer's RX->TX path* (7.79 item 6; its own record at SEQRAM 0x4F40/0x4F20, STIMER and
   SysTick, n = 468 turnarounds, M2): every turnaround took the cached-calibration retune (the
   synthesizer is up from RXWARM, 7.80): RX2TX entry 1.9 us after the RAC entered RX2TX, synthesizer
   locked 24.0-27.3 us after entry (SysTick 23.98-27.27 us), PA up 54.0-57.3 us after entry, against
   the 650-tick (135.4 us) hold: at least 76 us to spare. The power-up branch (RADIOEN.PREEN clear)
   was not taken once.
5. *Staged-path budgets and completion runs* (7.82; clean C1 unencrypted, C2 encrypted):

| figure | C1 | C2 (encrypted) | before (Kconfig) | now |
|---|---|---|---|---|
| conn completion TX->RX run | 179.1 us | 147.6 us | 143.7 | **179.2** |
| conn completion RX->TX run (commit + stage) | 98.9 us | 108.7 us (CCM in it) | 109.3 (+ CCM) | 109.3 |
| conn completion start -> next stage posted | 95.0 us | 97.2 us (CCM in it) | 46.1 (PROVISIONAL) | **95.1** |
| conn completion start -> PENDING/STAGED store | 144.0 us | 112.5 us | 95.1 | **144.0** |
| conn completion start -> RESPOND (FIFO re-armed) | 78.9 us | 55.7 us | 103.2 | 103.2 |
| conn last TX / abort run | 63.4 / 36.1 us | 52.2 / 36.4 us | close 241.7 | 241.7 |
| adv completion TX->RX (ADV_IND) | 61.4 us | 61.9 us | 46.4 | **62.0** |
| adv completion RX->TX (SCAN_REQ) | 70.2 us | 71.0 us | 52.5 | **71.1** |
| adv completion TX (SCAN_RSP) | 39.5 us | 39.5 us | 27.1 | **39.6** |
| adv start -> PENDING / RESPOND | 27.6 / 27.8 us | 27.9 / 28.2 us | 12.8 / 24.7 | **27.9 / 28.2** |
| held RX launch: start -> SWRXEN post | 15.71 us | 15.50 us | 15.2 | **15.71** |
| held RX launch run | 18.8 us | 18.5 us | 14.0 | **18.9** |
| TX launch: start -> TXEN post | 20.30 us | 20.39 us | 20.4 | 20.4 |
| sequencer: post -> first poll | 136 counts, 1.77 us | 140, 1.82 us | 1.771 | **1.823** |
| sequencer: instant -> command store | 117 counts, 1.52 us | 121, 1.58 us | 1.446 | **1.576** |
| sequencer polling ahead of the instant (min) | 23.7 us | 15.5 us | - | - |
| outcome wait / outcome after EOF | 30.3 / 112.5 us | 21.9 / 145.2 us | - | - |
| sequencer took the stage after RX2TX began | 17 STIMER ticks | 17 | 17 (7.82) | - |

   With the debug port polled during the connection (M2, M3) the same figures came out within the
   clean maxima except the RX window launch (62.4 us while `probe_watch` read TIMER1 at ~17 k/s) and
   the sequencer wake (142 counts); the product runs without a debugger, so those are not taken.
   The 7.77/7.82 advertising figures came from the stage-3 advertiser image; the connection
   profiles' advertising paths are longer and their maxima now set the defaults. Every run
   exceeding an old budget had still met every start-by (no miss, no unsat): the budgets were
   optimistic, which admission cannot see. With the new figures admission still admits the whole
   job set: L2 and P3 held 10 minutes with 0 misses and no unsat.
6. *Encrypted connection-event CPU time* (C2): the receive completion (decrypt, commit, encrypt
   the two staged replies, stage) 108.7 us longest, the TX->RX completion 147.6 us: 256 us of
   completion work per exchange, 0.57 % of the 45 ms interval (the prepares and the ULL are not
   in it).
7. *ram-fit checks*: the 64-octet BUFC TX FIFO carried every connection PDU of every run (GATT
   writes and reads, SMP and ATT PDUs up to 27 octets) with `BUFC.IF` reading 0x4
   (BUF0THR only: no BUF0OF or BUF0UF) after each measurement run (M2, M3, C1, C2); every read-back equalled its write (3552 rounds in
   the runs above, `gattcheck.sh`). Stack high-water (`INIT_STACKS`, untouched 0xAA from the low end
   after the run): TX launch job 208 of 640 octets (encrypted 428), RX launch job 208 (368),
   completion job 368 of 960 (664), LLL context 544 of 960 (748), ULL worker 648 of 960 (648). The
   TX launch job's 436-of-448 QEMU figure (ram-fit) is not reached on silicon here; with encryption
   it needs 428, so 448 would be too tight for an encrypted Peripheral (the connection profile keeps
   640).
8. *Prepare lead in deep sleep* (7.79 item 7): not measurable: the stack never enters EM2 (no
   power-management path in the radio HAL; the debugger also prevents EM2, 7.74), and the M0+ stops
   in EM2 and was found reset after it (7.74). Stays a design value [D].

**QEMU.** No model change: every silicon figure here agrees with what the model already
implements (T_IFS from the sequencer's hold, RX2TX retune with the cached calibration, a gated
TIMER read faulting, BUFC thresholds). The model's own paths keep their QEMU figures. With the
changed pairing profile the QEMU pair (`run-qemu-connection-e2e.sh 40`, `PAIRING=1`,
`build/qemu-new`) pairs at level 4 and completes 60/60 encrypted GATT rounds.

**Open.** The 21.5 us mean early arrival of the Central's packets against our computed anchor (item
3); MODEML.FRAMEDET's PRS selector on BG22; data channels 11-21 (outside the laptop's map); the
connection close/abort budget still the QEMU 241.7 us (no abort after a receive seen); EM2 and the
prepare lead; scanner and initiator responses still decided by the M33 (7.82); controller privacy
with a staged scan response (a resolving list in the sequencer) if the product ever needs it.
### 7.85 The synthesizer below and above the 2.4 GHz band; amateur Morse identification on silicon

**Superseded in part by §7.90:** the QEMU lock range is now 2158-2644 MHz.

Tags as in 7.79. Board: BGM220 explorer kit (J-Link OB 000440200548). Code and context:
`docs/amateur-radio.md`, `hal/amateur.c`, `hal/amateur_rules.c`, `hal/radio.c`
(`efr32_radio_cw_transmit()`, `EFR32_AMATEUR_SYNTH_MIN/MAX_KHZ`), `tests/amateur-port`
(`prj-probe.conf`, `prj-txcheck.conf`). Logs: `scratch/amateur/hw-*.log` of the worktree; every
transmission outside the plain Bluetooth plan is in the main checkout's
`scratch/amateur/tx-log.txt` (amateur operation under the user's licence, KD3APL, with his
go-ahead for 2300-2310 and 2390-2450 MHz).

**Question.** The BGM220P datasheet specifies the 2.4 GHz radio for 2400-2483.5 MHz (Tables 4.5,
4.6) and the reference manual says only that the synthesizer covers "the supported frequency
bands" (5.1.1). Amateur operation in the 13 cm band needs 2302-2308 and 2392-2400 MHz too.

**Receive-only probe [F].** A passive scanner (`EFR32_RADIO_AMATEUR_SYNTH_PROBE`: any
transmission stops the radio) moved its three advertising channels across 2290-2500 MHz in
5 MHz steps (0.9 s per three carriers) and sampled every 0.5 ms in RXSEARCH: `SYNTH.FREQ`,
`SYNTH.STATUS`, `SYNTH.VCOTUNING`. 488-1644 samples per carrier. The M0+'s successive-
approximation VCO calibration (7.60) converged everywhere, far from both ends of the 11-bit code
(VCAPSEL 16 throughout); `CAPCALERROR` never set. VCOTUNING (low 11 bits, min-max over the
samples):

| MHz | 2290 | 2300 | 2310 | 2350 | 2370 | 2375 | 2380 | 2390 | 2400 | 2450 | 2480 | 2500 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| code | 1418-1420 | 1367-1372 | 1325-1329 | 1142-1144 | 1065-1066 | 931-1049 | 913-914 | 868-869 | 826-836 | 626-628 | 487-526 | 424-425 |

About -4.7 codes/MHz, monotonic, with a ~130-code step between 2370 and 2380 MHz (the 2375 MHz
samples show both sides) and a smaller one near 2480 MHz [I: VCO sub-band boundaries]. On this
slope the code would reach its ends (0x7FF, 0) near 2160 and 2590 MHz [I, extrapolated, not
measured]. `STATUS.INLOCK` was never set in receive, as 7.64/7.72 found on every channel: lock
is not observable in RXSEARCH.

**Transmit checks [F].** Single-carrier images (`prj-txcheck.conf`): 2 s of non-connectable
advertising with the call sign as the Complete Local Name, then `efr32_amateur_disable()`, which
sends the closing Morse identification (KD3APL, 20 WPM, 200 Bd OOK, 104 octets) and returns to
Part 15. A 0.5 ms sampler in the RAC TX state:

| carrier | TX samples | INLOCK | CAPCALERROR | VCOTUNING | identification end |
|---|---|---|---|---|---|
| 2445 MHz | 8328 | 8328 | 0 | 0x28C-0x290 | 6372 ms after boot |
| 2395 MHz | 8328 | 8328 | 0 | 0x354-0x358 | 6372 ms |
| 2304 MHz | 8328 | 8328 | 0 | 0x54C-0x54D | 6372 ms |

8328 samples x 0.5 ms = 4.16 s in TX, almost all of it the identification frame (4.32 s
including its 160 ms sync; the advertising PDUs are each well under 1 ms): the OOK frame is sent
at the configured rate and length, and the synthesizer reports lock for all of it at all three
carriers. The TX codes match the receive probe's (the receive LO sits 0.7 MHz higher, f_IF).
The identification's job, ticker slot and safe-shutdown watchdog ran as in QEMU, and the
on-target API checks passed (27/27) on silicon.

**Periodic identification [F].** 660 s of advertising on 2445 MHz (in the ISM band) under
amateur rules: the periodic identification ended at 570.0 s after boot (QEMU, same image
configuration: 570.2 s), the closing one at 664.4 s after the advertising stopped; 22415 TX
samples, 22414 with INLOCK (the other at a TX edge [I]); no shutdown. This ran the fixed slot
handling (the reservation held until the frame ends; docs/amateur-radio.md section 5); the three
short checks above ran before that fix, with the radio otherwise idle during their
identification.

**In-band standard channels [F].** Default amateur plan (2402 and 2426 MHz kept, channel 39 and
the data channels on spare in-band carriers): the laptop's passive scan (BlueZ) reported the kit
as "KD3APL" during its 20 s of advertising.

**Consequence.** `EFR32_AMATEUR_SYNTH_MIN/MAX_KHZ` = 2290-2500 MHz (the probed range); the
QEMU model's `EFR32_SYNTH_LOCK_MIN/MAX_HZ` the same. Beyond it nothing was measured and
everything is refused.

**Not shown.** That the OOK frame actually keys the carrier (key-down power, on/off ratio,
edges): only the TX state, its duration and the synthesizer lock were observed. Output power,
occupied bandwidth, harmonics and spurious emissions at any carrier, and receive sensitivity off
the Bluetooth channels: no RF instrument was used. The SDR captures that would settle these are
in `docs/amateur-radio.md` section 8. Lock in transmit was checked at three carriers, not across
the band.

**Data identification (BLE-ASI) [F, Q].** `CONFIG_EFR32_RADIO_AMATEUR_ID_DATA`
(`docs/amateur-radio.md` 5.2, `docs/ble-amateur-station-id.md`; `hal/amateur.c`,
`efr32_radio_id_adv_transmit()` in `hal/radio.c`). Images: `tests/amateur-port` with
`prj-hardware.conf` and `prj-dataid.conf`, `AMATEUR_PORT_PLAN_INBAND` (channels 37 and 38 on
2402 and 2426 MHz, every other channel on 2402 MHz: standard in-band carriers only),
`AMATEUR_PORT_EOC_EVERY_S` (an end of communication, so a FINAL identification, every 5 s, or
10 s with CW) and the test-only `EFR32_RADIO_AMATEUR_ID_PROBE`, 60 s of advertising; the same
in the QEMU configuration, single machine. Logs: `scratch/amateur-data-id/hw-*.log`,
`q-probe-*.txt` of the worktree. Longest of 12 identifications (6 with CW):

| figure | silicon, data | silicon, data + CW | QEMU, data | QEMU, data + CW |
|---|---|---|---|---|
| job, own CPU time (thread usage) | 89.2 us | 89.7 us | 63.3 us | 63.5 us |
| job, wall time (preempted by radio jobs) | 830 us | 967 us | 291 us | 293 us |
| LLL start: entry to the first TXEN | 232.0 us | 235.9 us | 213.6 us | 207.1 us |
| completion callback (next PDU; CW launch) | 49.4 us | 184.6 us | 36.4 us | 157.6 us |
| TXEN to TXEN of consecutive parts | 618 us | 618 us | 620 us | 620 us |
| first TXEN to the end of the last frame | 1.71 ms | 4.329 s | 1.63 ms | 4.322 s |
| reserved span (`efr32_amateur_id_span_us()`) | 20.42 ms | 4.350 s | 20.42 ms | 4.350 s |

The first wall-time figures (21 ms) included the console report, then printed from the job:
it now runs on the system work queue. The job's budget `EFR32_RADIO_WCET_AMATEUR_ID_NS` takes
the CPU-time figure (89.8 us silicon, 63.5 us QEMU; it was a 200 us placeholder); the radio jobs
that preempt it are admitted on their own. The LLL start fits the LLL context's budget
(249.1 us silicon, 266.9 us QEMU). The completion callback's budget
(`EFR32_RADIO_WCET_AMATEUR_ID_COMPLETION_NS`) adds to a transmit's measured completion. On air:
3 x 328 us per data identification. The data+CW span on silicon read 34.0 ms from a 32-bit
nanosecond conversion that wrapped once (4.329 s); the probe now prints it in microseconds.

Each PDU went out whole: FRC.WCNT at each completion 0x21 (33 octets) of 33, FRC.WCNTCMP0 the
dynamic-length sentinel 0xFF before the first PDU and 0x20 (the previous frame's length less one,
left by the dynamic-length engine) before the next ones, as in `lll_adv.c`'s own PDU chains
(diagnostic build, not kept). The laptop's passive BlueZ scan saw the identification's own
address in four of eight runs and, read with `bluetoothctl info`, held its service data under
the BLE-ASI UUID: `11 4B 44 33 41 50 4C` (FINAL, KD3APL). BlueZ reports a device once and drops
it 30 s after its last report, so no reception rate follows from this; in the other four runs
(24-36 good-channel frames each) it reported none, unexplained. Not shown: a reception rate,
why the laptop missed whole runs, or anything an RF instrument would show.

**Connections (QEMU only).** A Peripheral and a Central under amateur rules, the Central
scanning between its connection events: three periodic data identifications from each during
29 virtual minutes of connection, each received by the Central, the link held. In QEMU the
connection-event turnarounds miss their spec deadlines (7.79), which the QEMU connection profiles
count (`SCHED_DEADLINE_MISS_COUNT`); amateur operation needs the safe-shutdown policy for its own
rules, so the test-only `EFR32_RADIO_TEST_QEMU_COUNT_LL_MISSES` counts scheduler misses and still
stops on `efr32_radio_policy_stop()`. In the hardware configuration of this run it counted 0. A
connection on silicon under amateur rules was not run (no second amateur station; the laptop's
host may not connect to an amateur-plan carrier set).

### 7.86 Thread stacks from the product's own watermarks; which threads can push an FP frame

Tags as in 7.79. Question: the product (`owo_suit -DOWO_HW=ON`) was 52 B over its 31 000 B RAM
budget (`scripts/check-hardware-ram.sh`) after 7.84 raised the launch stacks to 536 B, while the
ULL worker (640 B) was below its silicon peak (648 B, 7.84). Tooling: `scratch/ram2/` of the
worktree (`dual.sh`, `scen.sh`, `ramdump.py`, `maxtab.py`, `fpgraph.py`, `addrtaken.py`).

**Which threads execute FP instructions [F, from the image].** Build flags: `-mcpu=cortex-m33
-mfloat-abi=hard -mfpu=fpv5-sp-d16` (C, every one of 414 units) and `thumbv8m.main-none-eabihf`
(Rust); `FPU_SHARING=y`, so `z_prep_c` sets `FPCCR = ASPEN | LSPEN` (automatic, lazy state
preservation): CONTROL.FPCA is set by the first FP instruction a thread executes, an interrupt
then reserves the 104-byte extended frame on its stack, and otherwise pushes the 32-byte basic
frame (+ up to 4 B of alignment). New threads start with EXC_RETURN 0xBC (basic frame) and
`z_arm_pendsv` clears FPCA on every switch-in. Disassembly of the linked LTO image
(`arm-none-eabi-objdump -d`, every mnemonic starting with V): FP instructions occur in five
functions only:

| function | FP instructions | reached from |
|---|---|---|
| `start_read` (IADC driver) | 44 (float gain/offset arithmetic) | `iadc_read`/`iadc_read_async` only, whose addresses are stored only in `iadc_api`; the only ADC API call in the compiled sources is `owo_adc_start` (`read_async`), called directly from `Stimulator::fire`/`phase`/`convert`/`battery_check` -> `rust_main` -> `bg_thread_main`: the main thread |
| `k_clock_event_provider_register` | 2 (`vldr`/`vstr d7`: a 64-bit copy) | `burtc_clock_event_init` (init level), `counter_silabs_timer_init` (device init): boot |
| `z_prep_c` | 2 (`vmsr fpscr`) | reset |
| `z_arm_pendsv` | 3 (the FP context switch itself) | exception handler, MSP |
| `p4wq_loop` | 2 (`vldr`/`vstr d8`) | thread entry for P4WQ pools; the pool list is empty (`_k_p4wq_initparam_list_start == _end`), never runs |

From the radio's thread entries (`efr32_radio_tx_job`, `_rx_job`, `_completion_job`,
`ll_context_thread`) and `idle`, `work_queue_main`, `recv_thread`, `prio_recv_thread`, the direct
call graph reaches no FP function; their only indirect calls are the scheduler clock service's
backend calls (`burtc_backend`, `counter_silabs_timer_ce_backend`, six functions each), which
reach none either. So only the main thread (the output supervisor's conversions) can carry an FP
frame; every other thread's interrupt reserve is a basic frame, 36 B. The 104-byte reserve the
radio Kconfig help assumed for the launch jobs and LL threads does not arise.

**Product watermarks [Q].** QEMU (`build/qemu-new`, vendor/qemu 7813574), INIT_STACKS high-water
marks (the stack buffer minus its untouched 0xAA run from the low end; the sentinel word counts as
used). (a) Dual machine: the product (`-DOWO_HW=ON`, `sim/product-qemu.conf`, suit board model,
radio stacks enlarged so the run measures depth) against the Zephyr Central
(`prj-central-product.conf` + `prj-central-product-stim.conf`): LE Secure Connections passkey
pairing to level 4, then 870 rounds of encrypted GATT write/read-back and 87 stimulation cycles
over the link (widths, Start, a 5 s frame, a bad-checksum frame answered by a notification, Stop
every second cycle; notifications enabled), no failure. (b) Single machine, `CONFIG_OWO_SIM_INJECT`:
the 23 scenarios of `sim/scripts/make_scenarios.py` that need no gdb inferior call (all but the
radio shutdowns: a gdb inferior call runs on whatever stack is current), with the RAM also taken
at the reset entry after the watchdog resets.

**Silicon [F].** BGM220 EK (J-Link OB 000440200548), the radio-port pairing profile of 7.84
(`prj-hardware.conf`, `prj-connection.conf`, `prj-pairing.conf`, the 7.84 safety policy) with
`INIT_STACKS`, once as in 7.84 (logging on, "log") and once with `CONFIG_LOG=n` as the product
("no log"); the laptop (`bluetoothctl`) paired with the passkey and ran 118 / 117 GATT
write/read-back rounds over the encrypted link; every stack read over the debugger afterwards
(`scratch/ram2/sil.sh`), the bond removed by the device's own address. Not the product image (its
board and code partition are the suit's), so its figures are the same code built into a
different binary.

| thread | before | QEMU (a) | QEMU (b) | silicon log / no log | 7.84 | margin | after |
|---|---|---|---|---|---|---|---|
| main | 2048 | 1384 | 1420 | 944 / 636 (test app) | - | 108 (FP) | 2048 |
| ISR | 1024 | 516 | 536 | 540 / 540 | - | 36 | 1024 |
| idle | 512 | 348 | 292 | 48 / 48 (no PM) | - | 36 | 512 |
| system work queue | 1024 | 384 | 708 (mailbox, test only) | 420 / 420 | - | 36 | 1024 |
| BT RX | 1200 | 828 | 212 | 988 / 728 | - | 36 | 1200 |
| BT long work queue | 1400 | 732 | 684 | 976 / 968 | - | 36 | **1008** |
| BT TX processor | 1024 | 416 | 428 | 432 / 432 | - | 36 | 1024 |
| controller RX | 896 | 396 | 212 | 408 / 408 | - | 36 | 896 |
| controller priority RX | 640 | 380 | 196 | 416 / 416 | - | 36 | 640 |
| LLL | 768 | 656 | 452 | 748 / 748 | 748 | 36 | **784** |
| ULL worker | 640 | 532 | 420 | 648 / 648 | 648 | 36 | **688** |
| completion job | 768 | 604 | 340 | 636 / 636 | 664 | 36 | 768 |
| TX launch | 536 | 260 | 260 | 428 / 440 | 428 | 36 | **480** |
| RX launch | 536 | 372 | 204 | 368 / 368 | 368 | 36 | **408** |

Sizes: the largest peak of all sources plus the frame reserve, rounded up to the 8-byte stack
alignment. The long work queue runs only the host's P-256 key generation and DH key (GATT caching
off), every pairing both; its deepest point is p256-m's field multiply in every image (the return
addresses left below the watermark: `m256_mul` under `scalar_mult` and `generate_dh_key` in the
product, under `point_to_affine` on silicon, where LTO kept that function separate). The product's
QEMU and hardware images have identical p256-m and SMP symbols and sizes, so its own 732 B holds
for its hardware build; the silicon image's 968 B is the size taken. The TX launch job's silicon
peak is the scheduler clock service's arm path (`sched_arm_abs` -> `hw_arm` ->
`k_clock_event_arm_abs_with_generation`). The other stacks keep their sizes: their measured
headroom is large and their paths (ISR nesting, the host's ATT and L2CAP error paths, bt_enable)
are wider than the runs. `EFR32_RADIO_LAUNCH_STACK_SIZE` now only defaults the new
`EFR32_RADIO_TX_LAUNCH_STACK_SIZE` and `EFR32_RADIO_RX_LAUNCH_STACK_SIZE`.

**RAM** (linker map, `check-hardware-ram.sh` arithmetic): `owo_suit -DOWO_HW=ON` 31 052 -> 30 540 B
(budget 31 000); `owo_suit` 29 692 -> 29 180; `bg22_ek4108a` 30 092 -> 29 580.

**Not shown.** The product image itself on silicon (main, idle with power management, the
output stage's interrupts): only its QEMU figures exist; the silicon figures are the radio-port
image's. The configuration as committed was run in QEMU at the tighter sizes of the first pass
(long work queue 768, TX launch 464: (a) 510 rounds and 52 stimulation cycles, (b) 23
scenarios, no fault); the silicon figures then raised those two.
### 7.87 Scanner and initiator replies staged: SCAN_REQ and CONNECT_IND at T_IFS without the M33

Tags as in 7.79 ([Q] QEMU, `build/qemu-new`, per-core clocks; [S] Core v6.1; [D] derived here;
nothing here is measured on silicon: no hardware was available). Code: `sequencer/radio_seq.S`
(`efr32_seq_conn_decide`'s SCAN branch, `efr32_seq_scan_match`, `efr32_seq_rx_addr_eq`,
`efr32_seq_init_in_slot`, `efr32_seq_umod`), `include/hal/efr32/seq_abi.h` (`EFR32_SEQ_CONN_SCAN`
and the words after it), `hal/radio.c` (`efr32_radio_scan_stage()`, the commit check, the
completion windows), `include/hal/efr32/radio_conn.h`, `include/hal/efr32/ll_deadlines.h` (SCAN_REQ
and CONNECT_IND air times, `EFR32_DL_CONNECT_IND_WIN_OFFSET`, `efr32_dl_rx_staged_start_by()`,
`efr32_dl_init_*`), `lll/lll_scan_stage.c` (built into the derived `lll_scan.c`), `CMakeLists.txt`
(the lll_scan.c hooks), `Kconfig` (three new figures). No kernel or QEMU change. Tooling and logs:
`scratch/stage-scan/` of the worktree (build.sh, buildall.sh, dual.sh, ktest.sh, qt.sh, *.sum).

**The problem.** 7.82 staged the advertiser's scan response and every connection-event reply;
the scanner's SCAN_REQ (active scanning) and the initiator's CONNECT_IND were still decided by
the M33 at the advertisement's end, in a completion whose RESPOND deadline is about 80 us after the
frame. Device under test with silicon figures in QEMU (`EFR32_RADIO_TIMING_SILICON`, admission on,
the safe-shutdown policy), before this change [Q]:
- active scanner (+ the hardware-configuration advertiser): `deadline unsat:
  efr32_radio_completion_event ... deadline 50693 ns from the check, demand ahead of it 58114 ns`
  at its 33rd SCAN_REQ, safe shutdown;
- Central (+ Peripheral): `deadline unsat: efr32_radio_completion_event ... deadline 86963 ns
  from the check, demand ahead of it 58114 ns, running at release: efr32_lll_thread`, 1.03 s after
  boot: the CONNECT_IND's completion was released while the LLL context ran a prepare, which,
  cooperative at the same priority, finishes first (the report's "ahead" omits it, the simulation
  does not). After the safe shutdown the host's next command took an LL_ASSERT in `ull_scan.c`
  `disable()` (a stopped controller asked to stop scanning): not changed here.
- The initiator's first anchor could not be met at all: OpenISA sends WinOffset 0 without
  `BT_CTLR_SCHED_ADVANCED` (which this module does not support), so the first connection event's
  ticker expiry fell before the advertisement had even ended (below): the Central's "one miss in
  every connection run" of 7.82.

**What the sequencer decides, and from what [S].** Everything `isr_rx_pdu()` (`lll_scan.c`) decides
from is either in the received frame or fixed for the receive, except address resolution:
- *Active scanner*: answer an ADV_IND or ADV_SCAN_IND (Vol 6 Part B 2.3.1.1, 2.3.1.4: AdvA and 0-31
  octets of AdvData, so length 6-37) with a good CRC by a SCAN_REQ (2.3.2.1: ScanA, AdvA) whose RxAdd
  is the frame's TxAdd and whose AdvA is the frame's (2.3, 4.4.3.2). The sequencer writes the
  staged SCAN_REQ into the BUFC TX FIFO with those two fields from the RX FIFO, and arms the
  SCAN_RSP receive itself (SWRXEN, decision word STAGED) as after a connection reply.
- *Initiator*: answer, by a CONNECT_IND (2.3.3.1), an ADV_IND from the advertiser it initiates to
  (AdvA and TxAdd equal to the staged ones, 4.4.4), or an ADV_DIRECT_IND (2.3.1.2, length 12) from
  it whose TargetA is our InitA with RxAdd our address type or, with the extended filter policy,
  any resolvable private TargetA (RxAdd 1, top bits 0b01, 1.3.2.2). The CONNECT_IND does not depend
  on the frame: its AdvA and RxAdd equal the staged advertiser's by the match, every other field
  is the initiator's. `isr_rx_pdu()`'s own scheduling rule (no CONNECT_IND for a frame ending later
  than the scan slot less T_IFS, the CONNECT_IND, EVENT_OVERHEAD_START_US and
  EVENT_TICKER_RES_MARGIN_US after the event timer's start, modulo the scan interval when scanning
  continuously) is checked on TIMER0: the frame's end is TIMER0 at the decision less RAC.STIMER
  (counting from RX2TX's start) and the RXFRAME exit, converted with the TIMER0 rate the M33 maps
  from the scheduler clock; the remainder by shift and subtract.
- *Both*: an RX buffer must be free for the report or the connection node.

| rule | where | why |
|---|---|---|
| CRC, PDU type, length (AdvA present) | sequencer | in the frame |
| initiator: AdvA and TxAdd are the target's, directed TargetA and RxAdd | sequencer, against staged addresses | in the frame; the target is fixed while initiating |
| initiator: the CONNECT_IND fits the scan slot | sequencer, on TIMER0 | frame timing; `isr_rx_pdu()` follows the sequencer's verdict (`efr32_scan_declined()`): its own estimate of the frame's end differs by the two cores' timer reads |
| an RX buffer is free | staged when the receive is set up | only the LLL takes buffers, so one free then is free at the commit; the commit follows the staged answer |
| own address and type, WinOffset, interval, channel map, ... | staged, built as `isr_rx_pdu()` builds them (over its scratch PDU's header) | fixed for the receive |
| filter accept list in the filter policy | no turnaround armed | this HAL has no matcher (`radio_filter_has_match()` is 0): no frame passes `isr_rx_scan_check()`, the window is a passive one |
| resolving list: a target identity the list requires to resolve (`ull_filter_lll_rl_addr_allowed()`) | staged (no turnaround when not allowed) | fixed while initiating |
| address resolution enabled (`ull_filter_lll_rl_enabled()`) | M33 at the frame's end, as before | matching an RPA is AES over every peer IRK: not within T_IFS on the M0+; infeasible on silicon by analysis (7.82, 7.84) |

The HCI keeps the staged facts valid: the filter accept list, the resolving list and address
resolution cannot change while scanning or initiating uses them (Vol 4 Part E 7.8.16, 7.8.38,
7.8.44). The commit (`isr_rx()` unchanged but for two hooks) re-derives the same answer; the HAL
asserts that the PDU it commits is the one sent (the SCAN_REQ against the staged one with the
frame's RxAdd and AdvA from the RX FIFO, the CONNECT_IND octet for octet), and that a receive the
link layer closes unanswered got no answer from the sequencer.

**WinOffset [S][D].** The transmit window after a CONNECT_IND on LE 1M starts transmitWindowDelay
(1.25 ms) plus transmitWindowOffset (WinOffset x 1.25 ms) after the CONNECT_IND's end, and the
Central's first packet is its first anchor (Vol 6 Part B 4.5.3, 4.5.5; WinOffset 0 to
connInterval, 2.3.3.1). The offset is therefore fixed relative to the CONNECT_IND, which the
sequencer sends T_IFS after the advertisement: it does not depend on the frame's timing (only
`BT_CTLR_SCHED_ADVANCED`'s alignment to a planned anchor would, which `lll_scan_stage.c` refuses at
build time). What it must cover is the set-up after the CONNECT_IND: its transmit completion
(`lll_done()`), then the ULL worker that rings (started within `EFR32_DL_ULL_WINDOW_CYC`, its run
with the session-key ECB) starting the connection's ticker, before the first event's expiry,
which is at most the prepare lead (59 ticks, 1800.5 us) and the TX ready delay before the anchor.
`EFR32_DL_CONNECT_IND_WIN_OFFSET` is the smallest WinOffset that still leaves the CONNECT_IND's
completion, released at the latest frame_end_to_post and one interrupt after the CONNECT_IND, one
more interrupt and one LLL context run before its start-by. Silicon figures (a Central with
link-layer encryption): set-up 1414.2 us, need 1842.1 us, window without offset -642.2 us:
WinOffset **2** (the first anchor 3.75 ms after the CONNECT_IND, 15.7 us to spare); QEMU figures:
WinOffset 3. `isr_rx_pdu()` puts its anchor at that window's start; WinSize stays 1.

**Deadlines [D]** (`ll_deadlines.h`, figures in `Kconfig`):
- staged scanner receive (commit, SCAN_REQ): re-arm the RX FIFO before the SCAN_RSP can begin
  (EOF + 2 x 148 + 176 us, the SCAN_REQ being 176 us on air) and stage its receive before the
  shortest frame there can end (+ 80 us): `efr32_dl_rx_staged_start_by()`, 7.82's connection rule
  with the reply's own air time;
- the SCAN_REQ's transmit completion ends before that receive's completion is due
  (`efr32_dl_tx_staged_start_by()`);
- staged initiator receive (commit, CONNECT_IND): it, the CONNECT_IND's completion and the set-up
  end within the window above (`efr32_dl_init_commit_start_by()`); the CONNECT_IND's completion
  likewise (`efr32_dl_init_tx_start_by()`);
- a staged receive the sequencer did not answer keeps those deadlines (its completion cannot
  know at its post whether a reply left).

**Figures.** New, QEMU-measured (120 s wall, shift 3, QEMU figures, `EFR32_RADIO_TIMING_PROBE`),
each standing in on silicon with the connection path that QEMU runs longer (7.77's
EFR32_RADIO_FRAME_END_TO_COMPLETION_POST_NS is the precedent, the other way round) until measured:

| Kconfig | QEMU | silicon default (stand-in) |
|---|---|---|
| EFR32_RADIO_WCET_SCAN_COMMIT_NS | 59.3 us (initiator; scanner 55.8) | WCET_CONN_COMPLETION_RX_TX, 109.3 us (QEMU 76.3) |
| EFR32_RADIO_SCAN_COMMIT_TO_RESPOND_NS | 37.2 us | CONN_COMPLETION_TO_RESPOND, 103.2 us (QEMU 72.5) |
| EFR32_RADIO_SCAN_COMMIT_TO_STAGED_NS | 56.8 us | CONN_COMPLETION_TO_STAGED, 95.1 us (QEMU 68.8) |

The other scanner and initiator runs stay within their kinds' QEMU figures: the SCAN_REQ's
transmit completion 35.8 us (TX_RX 86.8), the CONNECT_IND's 21.9 us (TX 186.8), the scanner's
abort completion, which now stages, 84.0 us (ABORT 91.8), the Central's LLL context 263.3 us
(266.9). (The QEMU-figure Peripheral's LLL context ran 304.9 us in the same runs, above its 266.9:
not a path changed here, recorded for the Peripheral's own figures.)

**Admission on silicon figures, by analysis [D]** (latest post EOF + 11.54 + 58.1 us; at worst one
interrupt and one LLL context run ahead):

| completion | before | after: start-by | window from the latest post | worst demand ahead |
|---|---|---|---|---|
| scanner, SCAN_REQ | RESPOND, window at the check 50.7 us: unsat | EOF + 368.8 us | 299.2 us | 58.1 us, + 249.1 with an LLL run |
| SCAN_REQ transmit completion | PENDING before the SCAN_RSP is recognised | post + 465.2 us | 465.2 us | 307.2 us |
| initiator, CONNECT_IND | RESPOND, unsat behind a prepare | EOF + 834.4 us | 764.8 us | 358.3 us (LLL with CCM) |
| CONNECT_IND transmit completion | the scan rule (2.5 ms); first anchor unmeetable | post + 374.0 us | 374.0 us | 358.3 us |

Every staged path is feasible with the interrupt allowance; with an LLL context run already in
progress at the post as well, the initiator's paths are, and the scanner's commit is short with
the stand-in figures: by 8 us in a scanner image (LLL run 249.1 us; feasible once its path to the
RX FIFO re-arm is measured below 95.2 us), by 59 us in a Central image scanning actively (LLL run
with the anchor's CCM 300.2 us; below 44.1 us). QEMU: 37.2 us. Moving the RX FIFO re-arm out of
the commit (to the start of the completion, before the LLL callback) would remove that term. Admission checks each release against what is actually pending, so
that case, should it arise (only another role's prepare can fall on a scan window's frame),
stops the radio safely; it arose in no run below.

**QEMU runs** (silicon figures, admission on; the device under test, second, with the safe-shutdown
policy, the peer counting misses; virtual seconds from the scheduler clock):

| pair | shift 3 | shift 5 |
|---|---|---|
| advertiser + active scanner | 97.8 s, 3915 / 978 (SCAN_REQ) TX, 1955 reports, 0 / 0 misses | 105.8 s, 4231 / 1057 (SCAN_REQ) TX, 2113 reports, 0 / 0 misses |
| Peripheral + Central (GATT write/read-back) | 1489.2 s, 1140/1140 rounds, 0 / 0 misses | 1534.3 s, 1170/1170 rounds, 0 / 0 misses |
| the same, LE Secure Connections passkey pairing (level 4), encrypted GATT | 1497.3 s, 1140/1140 rounds, 0 / 0 misses | 1517.9 s, 1140/1140 rounds, level 4, 0 / 0 misses |
| injected: the scanner's commit declared to reach its RX FIFO re-arm 400 us after it starts | unsat at the first staged SCAN_REQ (`deadline 57488 ns from the check, demand ahead of it 58114 ns`), safe shutdown, no SCAN_REQ on air | - |

(600 s wall each, the injected run 60 s; no disconnection, no late delivery, no assertion.) The
Central's 7.82 miss is gone with the WinOffset. The Central pairing image fits RAM with the
safe-shutdown policy only with `SCHED_DEADLINE_MISS_HISTORY=2` and a 128-octet log buffer (it
overflowed by 220 octets before this change, 268 after; QEMU test configuration only). With QEMU
figures and admission on (the measurement runs, misses counted) the scanner and the Central had no
miss either (every turnaround they had was infeasible with QEMU figures before); the QEMU-figure
advertiser and Peripheral still miss, as 7.77 says.

Kernel suites (native_sim/native/64, kernel unchanged): clock_event_sched 13/13, deadline 14/14
(also with admission and stats), deferred_release 5/5, clock_event 4/4, deadline_admission 15/15.
qtests: efr32bg22-test 104, rf 36, dap 13, board 7, unit test-jlink-swd-io 3, all pass; the host
contract tests (`tests/efr32-radio-contract`) pass; the QEMU-configuration images (QEMU TIFS seam,
staging off) build.

**Silicon measurements needed** (BGM220 EK as active scanner, then as initiator, against the laptop
or a second kit advertising; probe images with `EFR32_RADIO_TIMING_PROBE`):
1. T_IFS of our SCAN_REQ and of our CONNECT_IND: the advertisement's EOF to our SYNCSENT
   (`scripts/bg22-tifs-probe.tcl`, PRS MODEMH.EOF reload-starting TIMER1); expected the SCAN_RSP's
   180.1-180.9 us (the same RX2TX hold).
2. The sequencer's decision time in the SCAN and INIT modes (`EFR32_SEQ_CONN_TAKEN_AT`, the RX2TX
   trace at SEQRAM 0x4F40): the address compares and the slot check run before the synthesizer
   retune, and 7.84 item 4 left at least 76 us before the hold.
3. The slot check: the frame end the sequencer computes against TIMER1's PRS-captured EOF (7.84
   item 2's method), and its verdict against `isr_rx_pdu()`'s own `pdu_end_us` near the limit.
4. The new figures: the scanner's and the initiator's commit runs (EFR32_RADIO_WCET_SCAN_COMMIT_NS),
   the scanner commit's paths to its RX FIFO re-arm and to the next stage
   (EFR32_RADIO_SCAN_COMMIT_TO_RESPOND_NS, _TO_STAGED_NS); and the scanner paths the existing kinds
   cover without a silicon sample: the SCAN_REQ's transmit completion (`isr_tx`, TX_RX), the
   CONNECT_IND's (`isr_cleanup`, TX), the scanner's abort completion with its stage (ABORT), the
   scanner's and initiator's prepare with the stage (EFR32_RADIO_WCET_LLL_NS).
5. The first anchor: the CONNECT_IND's end to the Central's first SYNCSENT, expected 1.25 ms +
   WinOffset x 1.25 ms (3.75 ms with the silicon figures).

**Open.** Address resolution in the sequencer (controller privacy as scanner or initiator, and as
advertiser, 7.84); the silicon measurements above; the scanner commit's worst case with an LLL
run ahead (above).

### 7.88 32-bit time across 2^31 and 2^32: the sequencer's LIMIT check

Tags as in 7.79. Question: a dual-SoC QEMU run (`b-adv`/`b-scan` images of 31 Oct) made a safe
shutdown at 64.2 s, "deadline late start: efr32_radio_completion_event ... deadline
-2139858319", just past 2^31 scheduler-clock cycles (2^25 Hz). Is a 32-bit wrap in the deadline
arithmetic the cause, and can it reach silicon?

**That miss is not a wrap.** [Q] It is deterministic (the same release, deadline and now at
icount shift 2, 3 and 4). Under the gdbstub, logical time was moved 0x58000000 cycles ahead at the
first provider registration, so 2^31 fell at 20 s of uptime: the same miss came at the same
uptime (64.63 s, as in the unshifted control under the same stop), at logical 3 644 950 554, far
from 2^31 and 2^32, and nothing happened at 20 s. The completion was posted 9.8 ms after the
event's last TXEN, so its start-by (`efr32_dl_adv_next_start_by()`, 10 ms after that TXEN) was 28
cycles away. "-2139858319" is only `k_deadline_miss_print()` printing the unsigned deadline with
`%d` (now `%u`). Current images do not reproduce it: 0 misses in 451 s.

**A real wrap, in the sequencer.** `efr32_seq_timed_command` decided "past LIMIT" with
`subs r4, CNT, LIMIT; bgt late`. `bgt` reads V as well as N and Z, and V is set when TIMER0's
count and LIMIT lie on either side of 0x80000000 (the difference overflows as a signed value), so
the test is not modular there:
- CNT < 2^31 <= LIMIT, the command on time: reported `EFR32_SEQ_TIMED_LATE`, which
  `efr32_seq_timed_check()` turns into a late start of "efr32 sequencer timed command" and a safe
  shutdown;
- LIMIT < 2^31 <= CNT, the command late: issued anyway, the miss unreported.

The crossing of 2^32 (0xFFFFFFFF to 0) is correct either way, and the instant wait (`bmi`, N only)
is correct at both. Fixed: late is `LIMIT - CNT` negative, `subs r4, LIMIT, CNT; bmi late`.

**On silicon.** TIMER0 counts EM01GRPACLK, 76.8 MHz on silicon and in QEMU (`active_hz` read
76 800 000 from a running image): it passes 0x80000000 27.96 s after it starts and then every
55.92 s. A false LATE needs a command's [TARGET, LIMIT] to straddle that count, so the chance per
crossing is the timed-command rate times `LIMIT - TARGET` (1149 counts, 15 us, for a TX launch; a
Peripheral's receive-window tolerance is wider). At one command per 30 ms that is 5e-4 per
crossing, about 3 % per hour. The 10-minute silicon connection runs crossed it about 10 times.
Consequence: a spurious safe shutdown (radio stopped, link lost) or, rarer, a late TXEN or SWRXEN
issued past its spec tolerance without the miss being reported.

**Test.** `tests/sequencer-timed-wrap` (bg22_ek4108a on the QEMU model, M0+ included) installs the
open sequencer as `radio_reset()` does, starts TIMER0 just below 2^31 and 2^32 and posts SWRXEN
(RAC forced disabled) with the window, the instant or LIMIT across each boundary. Before the fix
4 of 6 cases pass: `test_window_across_2_31` (on time, reported LATE) and `test_late_across_2_31`
(issued past LIMIT) fail. After it all 6 pass.

**Audit** of every 32-bit time comparison on these paths (the rule is the half-range one: values
compared lie within 2^31 of each other):
- kernel: `deadline.c` (coalescing, not-before wait, late start), the EDF compare in
  `priority_q.h`, `deadline_admission.c` (`rel()` to now, then relative times), `deadline_miss.c`,
  `deferred_release.c`, `clock_event.c` (lead check), `clock_event_sched.c` (64-bit logical time,
  `extend()`, `to_active()`/`to_deep()` within their re-anchored spans), `clock_event_timer.c`,
  `silabs_burtc_clock_event.c`, `counter_silabs_timer.c`: wrap-safe. A regression case in
  `tests/kernel/clock_event_sched` crosses 2^31, 2^32, 2^32 + 2^31 and 2^33 with short-lead,
  deep-then-active and late deliveries.
- radio: `hal/cntr.c` (64-bit, sign-extended 32-bit offsets), `ll_threads.c`, `radio.c` (hold,
  RX EOF, completion windows, timed post), `ll_deadlines.h`: wrap-safe. One signed subtraction of
  two absolute times (`efr32_dl_conn_rx_staged_start_by()`), undefined on overflow, is now an
  unsigned difference read as signed.
- sequencer: the LIMIT check above. The instant wait, and RAC.STIMER (restarts per state), are fine.

**RAM.** `owo_suit -DOWO_HW=ON`: 30 556 B before and after (one instruction of the SEQRAM image, a
format string, a header-only rewrite).

**QEMU dual run** (`dual.sh`, icount shift 3): fixed images 455.5 s virtual, 0 misses (8 crossings
of TIMER0 = 2^31, logical 2^31, 2^32 and 2^32 + 2^31 all passed); unfixed images 451.4 s, 0.

### 7.89 A silicon batch: multi-part CMAC, transmit power registers, IADC and TRNG timing, RX FIFO overflow, frame detect

Tags as in 7.79. Board: BGM220 explorer kit (J-Link OB 000440200548), HFRCODPLL 76.8 MHz. Every
transmission was on a standard Bluetooth carrier at the kit's normal levels; the on-off keyed
identifications were on 2402 MHz only (below). Tooling and logs: `scratch/sil-batch/` of the
worktree (`hw.sh`, `run.sh`, `txwatch.tcl`, `prs.tcl`, `ovf.sh`, `stab.sh`, `fit-iadc.awk`,
`cmp.awk`, `ramp-probe.patch`). Timebase: the M33 DWT, calibrated in `tests/periph-timing-port`
against the system timer on the LFXO over 65536 LFXO cycles: 76 791 476-76 791 496 Hz (nominal
76.8 MHz, -110 ppm); TIMER1 on EM01GRPACLK (HFRCODPLL, `EM01GRPACLKCTRL` 1) for PRS captures; the
M0+ SysTick at HCLKRADIO 38.4 MHz.

**1. `tests/crypto-port` [F].** RADIOAES PSA build: **330/330 pass** (`crypto_results` over SWD:
magic 0xC0FFEE01, pass 0x14A, fail 0), the 134 multi-part CMAC checks of 2fb0973 included; software
PSA build (`prj-software.conf`) 330/330. New timed operations, n = 64, DWT cycles (us):

| operation | RADIOAES PSA | software PSA |
|---|---|---|
| multi-part CMAC, 65 octets in 3 updates (import, setup, updates, finish, destroy) | 9 564-9 586 (124.5-124.8) | 32 352-36 365 (421-473) |
| multi-part CMAC, 256 octets in 16-octet updates | 16 891-16 894 (219.9-220.0) | 61 751-65 976 (804-859) |

The other figures repeat docs/crypto.md 4 within a few cycles (ECB 437-475, CCM 27 octets
1 195-1 212, P-256 key generation 27 060 001 cycles); one PSA CCM call reached 7 893 cycles with a
system-timer interrupt in it (the PSA calls run with interrupts enabled).

**2. Transmit power registers [F]** (`tests/tx-power-port` with the new `prj-hardware.conf`, the
explorer kit overlay, and an amateur plan with every channel on 2402 MHz, so the Morse
identifications are sent there: 43/43 on silicon and in QEMU).
- `MODEM.RAMPLEV` reads **0x9F9F9F** after a system reset, before the first event (the test's own
  check and the SWD sampler). Each phase then reads the level of docs/tx-power.md 3: 0x3A3A3A
  (+4 dBm), 0x242424, 0x202020, 0x2E2E2E under the 0 dBm ceiling, 0x202020 under -27 dBm, 0x999999
  (+9 dBm). `RAMPCTRL` stays 0x555; its RAMPVAL field read 0 in every sample, in TX too.
- **On-off keying**: during both identifications (`MODEM.CTRL0.MODFORMAT` 6, RAC in TX, SYNTH.FREQ
  0x01F46AAB = 2402 MHz) `RAMPLEV` held the ceiling's level (0x2E2E2E, then 0x202020) and `RAC.APC`
  read 0xFF000000 (ENAPCSW clear, AMPCONTROLLIMITSW 0xFF): the sequencer's PA start sets ENAPCSW and
  clears it again once PARAMP is set, so the key-down power is set by the ramp level and not by APC.
  Whether the PA's output follows RAMPLEV under OOK (stock uses APC for it, docs/tx-power.md 2) needs
  an RF measurement [U].
- **Ramp** (`ramp-probe.patch`, measurement build only: SysTick at PARAMP set/clear and at
  `MODEM.SEQIF.TXRAMPDONE`, n = 43 transmissions): ramp-down from PARAMP clear to TXRAMPDONE is
  below the probe's 0.89 us floor at levels 0x20-0x2E, 45 counts (1.17 us) at 0x3A and 133 counts
  (3.46 us) at 0x99: about 24 ns per RAMPLEV step, so about 4 us at the reset level 0x9F, far inside
  the sequencer's bounded wait. TXRAMPDONE already reads 1 at the first poll after PARAMP is set in
  TXWARM, at every level, and the frame does not wait for a ramp: PRS RACL.TX rise to MODEMH.SYNCSENT
  is 40.61-40.72 us at every level (n = 55, 3119-3127 TIMER1 ticks), the preamble and access address
  plus 0.6 us. Ramp-up timing is therefore not observable this way.
- Not done: relative output per level from the laptop's RSSI (no image holds a level long enough
  for BlueZ's reports); output power, ramp shape and the spectrum mask need an SDR or meter.

**3. A defect found on the way: the CW identification overran its slot [F].** In this image the
closing identification shut the radio down every time (`deadline unsat: efr32_amateur_id_event ...
5.3 us past the deadline`): its slot and frame timer reserved the Morse frame's nominal air time,
4320 ms, but on silicon first TXEN to the end of its completion is **4327.26 ms** (ID probe, 10
identifications, spread under 2 us; QEMU 4320.36 ms), so the frame timer (slot + 1.87 ms of event
overheads) expired 5.6 ms before the completion, counted the frame as cut and posted a retry with a
zero start-by. The data identification's span had hidden it. `EFR32_RADIO_AMATEUR_CW_TAIL_NS`
(silicon 7 261 000, QEMU 363 000) now adds the measured tail to the CW part's span; where the 7 ms
lie (TX ready, the modem's end of an OOK frame at 200 Bd, the completion) is not yet split, and the
QEMU model's OOK frame end is 6.9 ms shorter than silicon's.

**4. IADC and TRNG timing (`tests/periph-timing-port`) [F]**, the same image in QEMU for
comparison (`cmp.awk`). Each figure is a command write to a status poll (poll period 0.27 us), 32
conversions with the start dithered over 211 CPU cycles, or 8 TRNG fills.

IADC, CLK_CMU_ADC = EM01GRPACLK 76.8 MHz:
- Warm-up: five TIMEBASE intervals from power-down (NORMAL, every request: 8.37 us total at TIMEBASE
  38, 13.48 us at 77), one from standby (4.32 us), and again after EN is set; the RM's formula holds
  within 0.36 us. STATUS.ADCWARM rises 0.55-0.7 us after the five intervals.
- A conversion: the OSR slope is exactly 4 ADC_CLK per OSR step (OSR 2-64 at 9.6 MHz: 3.11, 3.84,
  5.68, 8.96, 15.52, 29.00 us), but a request's first result comes later than (4 OSR + 2) ADC_CLK:
  a fit over 15 configurations (ADC_CLK 2.4-9.6 MHz, CLK_SRC_ADC 19.2-38.4 MHz, OSR 2-64) gives
  **(4 OSR + 2 + 9) ADC_CLK + 18 CLK_SRC_ADC + 0.71 us**, residual within 0.18 us (the 0.71 us
  includes the measuring loop). A scan's first entry takes 7 ADC_CLK more; later entries follow about
  (4 OSR + 2) ADC_CLK apart. Changing the input between conversions costs nothing measurable (the
  model's 2-cycle switch cost is gone).
- The product's read path (Zephyr's driver, the sense channel of `boards/owo_suit_hw.overlay`:
  NORMAL, TIMEBASE 76, 9.6 MHz, OSR 2): `adc_read_async()` to its callback **24.5-24.8 us** (QEMU
  21.5: the model's CPU), `adc_read()` 42.8-53.1 us.

TRNG (CRYPTOACC RNGCTRL):
- After a system reset the TRNG is already running when `main()` starts, RNGCTRL 0x00040671 (ENABLE
  and five interrupt enables) and INITWAITVAL 0x200, although nothing in the image writes it (the
  boot ROM is the likely writer [I]); the RM's reset values are 0x00040000 and 0xFFFF.
- The rings are sampled at **HCLK** / (CLKDIV + 1), not PCLK (PCLK is HCLK / 2 here); INITWAITVAL
  counts HCLK cycles (16 to 16384: 13.0 ns per count). After the wait the NIST start-up test takes
  512 samples (BYPNIST skips it) and the first block 512 samples whatever NB128BITBLOCKS; its first
  word is in the FIFO 1.41 us later (NB 1 and 8 come up to 1.0 us earlier). Later blocks take
  NB128BITBLOCKS x 128 samples + 0.55 us (0.13 us at CLKDIV 1 and 3, 0.95 us at NB 1). A block's four
  words reach FIFOLEVEL over 1.0-2.1 us. Default settings from ENABLE: first word 21.8 us, FIFO full
  (64 words) 130.0 us.
- QEMU (vendor/qemu): the model now follows all of this; against silicon it is within 0.43 us over
  50 IADC configurations except a 32-times oversampled scan's second entry (0.8 us), and within
  0.27 us for TRNG fills with INITWAITVAL set (CLKDIV 1: 6.4 us late over a fill). It keeps the RM's
  reset values for the TRNG, not the post-ROM state.

**5. RX FIFO overflow recovery [F].** Reproduced as in 7.80 (hardware scanner, the M33 halted 2 s):
`FRC.IF.RXOF` (bit 8) latches with `BUFC.IF.BUF1OF`, RXDONE does not, the RAC holds RXOVERFLOW
(STATUS 0x86400002); master's image stays there (3 of 4 runs: state 6 one second after resuming).
The HAL now takes RXOF on FRC_IRQn: SWRXEN withdrawn, FORCEDISABLE until OFF/SHUTDOWN, released,
BUFC channel 1 CLEARed, both flags dropped, and the receive completes as one that received nothing
(the scanner's `isr_done()` re-arms); `radio_disable()` recovers too. On silicon, 6 halts and 2 runs with FRC.IEN cleared over SWD: back in
RXSEARCH within 20 ms of resuming, reception continuing (1 143 frames in the next 3 s), recovery
**6.8-7.0 us** (227-236 cycles of the 33.55 MHz scheduler clock). QEMU raises RXOF too (qtest
released-rx-fifo-overflow).
Open: with the rebased tree the hardware scanner crashed in 2 of 6 halt runs ("Invalid buf type"
from `bt_hci_core`, a bus fault in `net_buf_ref`), and in 6 of 18 boots across three
builds with this change, one of them with RXOF masked so that the recovery never ran (its counters
read 0); master's `radio.c` gave no crash in 15 boots (4 with halts). On top of the
abandoned-frame fix (6, close/abort budget) the same recovery gave **10 of 10 clean boots** and
recovered **4 of 4 halts** (back in RXSEARCH, 4 956-5 050 frames per 30 s run, recovery 6.9 us).
Master never ran a full boot without its own safe shutdown cutting it short, while master with
that fix ran 10 full boots clean: the crashes came with the abandoned-frame wait, not the recovery
[I]: a frame RXDIS leaves running ends after the abort, and its RXDONE and FIFO contents can reach
the next receive. The mechanism is not traced.

**5a. The PA and the power range [F unless tagged].** The module (DEVINFO.MODULENAME read over
SWD: "BGM220PC22HNA2") is an EFR32BG22 with its two integrated 2.4 GHz PAs, the "6 dBm" (RAIL
HP, levels 0-127) and the "0 dBm" (LP, 0-15) one (`rail_chip_specific.h`, Series 2 config 2);
nothing external. DEVINFO.MODULEINFO reads 0x00570100: PADCDC (bit 29) = 0, **PAVDD is fed from
the DCDC output** (RM 6.4.2.25; module schematic, datasheet fig. 3.2: VREGSW, 2.2 uH, DVDD, RFVDD
and PAVDD), and RF2G4_IO goes through the module's matching network to its built-in antenna
behind the shield. Ratings (BGM220P datasheet rev 1.3): VDD absolute maximum 3.8 V, 200 mA total
into VDD (table 4.1; no RF-pin or mismatch rating is given for the module); POUTMAX 8.2 dBm typ for
the 8 dBm OPN, POUTMIN -27 dBm (6 dBm PA) and -28 dBm (0 dBm PA) (table 4.5); 10.6 mA TX at 8 dBm.
RM 12.3.7: PAVDD >= RFVDD. EFR32BG22 SoC datasheet rev 1.3: any supply pin, PAVDD included,
absolute maximum 3.8 V, PAVDD operating 1.71-3.8 V; RF2G4_IO input level +10 dBm maximum and
-0.3 V to VPAVDD + 0.3 V (tables 4.1, 4.2); POUTMAX 6 dBm for the 6 dBm PA and 0 dBm for the 0 dBm
PA, POUTMIN -27 and -28 dBm, specified with PAVDD 1.8 V from the DCDC (table 4.13), with note 3:
the PA can deliver more than 6 dBm but characteristics and application circuits are specified at
6 dBm, and above it harmonics may exceed regulatory limits. **Figure 4.9** plots the 6 dBm PA's
output against RAIL power settings **0-127** (about -27 dBm at 0, 0 dBm near 16, about 8.5 dBm at
127, flattening: under 0.1 dB per setting above about 100) and the 0 dBm PA's against 0-15 (about
-29 to 0.5 dBm); its note adds that current rises above 6 dBm. The module is rated 8 dBm (BGM220P
ordering table, its own certification) on this same PA, so levels 0-127 are the documented range
for this OPN.
- Lower: Silicon Labs' 0 dBm PA table bottoms at -28.7 dBm against the 6 dBm PA's -27.5 dBm at
  level 0 (`sl_rail_util_pa_dbm_powersetting_mapping_table_{0,6}dbm.h`): 1.2 dB, and using it needs
  the sequencer's PA start to enable the TX0DBM path, whose enable/bias sequence is in no open
  source. Not done.
- Higher: level 127 (RAMPLEV 0x9F, the reset value) is the top of the documented range and is
  already the product's level. No other documented mode exists on this part (no 10/20 dBm PA;
  `SL_RAIL_UTIL_PA_VOLTAGE_MV` describes the board's PA_VDD to pick a curve, it sets nothing).
  RAMPLEV 0xA0-0xFF (the field is 8 bits) was **not tried**. Its voltage stress is bounded [I]:
  PAVDD stays at the DCDC's 1.8 V whatever the level, the same PA is rated for PAVDD up to 3.8 V,
  and the RF pin's own limit is PAVDD + 0.3 V, so no ramp level can raise the supply the PA sees.
  But Figure 4.9 shows the output already saturating at 127 (a few tenths of a dB at most to
  gain), the map from ramp level to PA slices is undocumented, and the one thing a higher level
  could raise, the PA's current, cannot be watched without an ammeter in series with the kit's VDD
  (the explorer kit has no current sensing). Little to gain, not observable: not worth the risk.
  RAMPLEV below 0x20 (below level 0) is undocumented too, and would gain nothing documented over
  level 0's -27 dBm. Not tried.
- Within range: the sweep image (`scratch/sil-batch/pwr-sweep`) selected levels 0, 1, 2, 3, 4, 5,
  8, 14, 26 and 121 as requested (RAMPLEV read back each step), advertising non-connectable on
  channels 37-39, 4 s per level, 3 cycles. Laptop RSSI (BlueZ 5.87 `bluetoothctl`, duplicate data
  on, kit on the desk about 1 m away; a sanity check only, not a power measurement), mean (n):

  | PA level | 0 | 1 | 2 | 3 | 4 | 5 | 8 | 14 | 26 | 121 | 127 (reset, no TX_POWER) |
  |---|---|---|---|---|---|---|---|---|---|---|---|
  | published dBm | -27.5 | -21.2 | -16.0 | -12.7 | -10.4 | -8.6 | -4.8 | -0.6 | 3.4 | 8.5 | - |
  | RSSI dBm | -82.6 (5) | -74.9 (11) | -68.2 (4) | -66.5 (8) | -63.1 (11) | -60.6 (5) | -59.8 (12) | -53.8 (5) | -49.7 (7) | -42.8 (13) | -43.3 (30) |

  Monotonic from 0 to 121 within the 2-5 dB scatter; 39.8 dB from level 0 to 121 against the
  table's 36.0; 127 is indistinguishable from 121, as Figure 4.9's saturation predicts. The lowest
  documented level is still received about 10 dB above the laptop's weakest reports (-93 dBm) at
  this range: the usable range is the documented one, -27.5 to about 8.5 dBm.
- Out-of-range levels in firmware: none added, as none was tried. Should one be justified later
  (an ammeter on VDD and an SDR or meter on the output), it belongs behind its own Kconfig option,
  default n, depending on `EFR32_RADIO_TX_POWER`, with help text stating the risk and this
  evidence, and refused for the product the way amateur operation is (a FATAL_ERROR in the
  top-level CMakeLists.txt).

**6. §7.84 items.**
- *Frame-detect PRS signal [F]*: MODEML is PRS source **0x2A** (`efr32bg22_prs_signals.h`); 7.84
  used 0x2D, which is FRC. MODEML.FRAMEDET = signal 7, captured as a pulse or a level by TIMER1:
  RACL.RX rise to FRAMEDET 42-720 us, FRAMEDET to MODEMH.EOF 80-360 us (n = 893; the 320-360 us
  cluster is 42-octet ADV_INDs, 336 us of header, payload and CRC): it marks the access address.
  `prs.tcl` carries the corrected table.
- *Close/abort budget [F, from failures]*: master's hardware scanner (`radio-rx-port`
  `prj-hardware.conf`) safe-shut down in 7 of 11 boots of 40-45 s: `deadline late start: ull worker
  context`, released while `efr32_radio_completion_job` ran; the completion event started 41.8 us
  after that release and the ULL worker only 1 226-1 319 us after it, so the completion ran at least
  1.18-1.28 ms (n = 7) against the 241.7 us close budget: 7.80's wait for an abandoned frame.
  **Fixed**: `radio_disable()` now ends an RXFRAME the RXDIS leaves running with a FORCEDISABLE
  pulse (set, wait for OFF/SHUTDOWN, release; the abandoned frame never reaches BUFC, 7.80), so
  the next launch finds the RAC idle. Silicon: 10 of 10 boots of 45 s clean (7 197-7 716 frames
  each), tests/tx-power-port 43/43. A connection event closing on a frame in reception takes the
  same path; not exercised on silicon here.
- Not done in this batch: the 2M PHY connection with the laptop, the laptop's 21.5 us early
  arrival, data channels 11-21.

**7. BLE-ASI reception gap (7.85).** Not investigated in this batch.

### 7.90 Synthesizer frequency switching: mechanisms, settling, range; LoRa and scan viability

Tags as in 7.79. Board: BGM220 explorer kit (J-Link OB 000440200548). Probe: `tests/fswitch-probe`
(Kconfig `FSWITCH_PROBE_SET` selects experiments E1-E16, `FSWITCH_PROBE_TX` the one in-band
transmit check; `prj-qemu.conf` runs it on the model). A passive scanner with a 10.24 s window brings the radio to RXSEARCH on 2402 MHz
through the open sequencer (7.80); the M33 then locks its interrupts, so the link layer never
ends the window, and measures the synthesizer directly. Receive only except E15. Register names:
Silicon Labs' public xG22 register model (`SYNTH_S`/`RAC_S`/`MODEM_S` `_field.py`, Simplicity
SDK v2025.6.1). Timebase: M33 DWT.CYCCNT, HFRCODPLL 76.8 MHz (7.74). Logs: `scratch/fswitch/`
of the worktree (a1-a11 silicon, q1-q2 QEMU). QEMU: `hw/arm/efr32bg22.c`,
`hw/arm/efr32-rf-medium.[ch]`, qtests in `tests/qtest/efr32bg22-rf-test.c`.

**The instrument: the LO counter [F] (E1, E10).** `SYNTH.LOCNTSTATUS.LOCOUNT` (19 bits) after
`LOCNTCTRL.RUN` holds the LO cycles in 2^(NUMCYCLE + 1) HFXO periods: 512576 at NUMCYCLE 12 in
RXSEARCH on 2402 MHz = 2402.700 MHz, the carrier plus the IF (high side). NUMCYCLE saturates at
12 (13-15 read and last the same, 213.8 us), so the finest resolution is 4.6875 kHz per count in
213.3 us and the shortest window 26 ns (NUMCYCLE 0, 4.8 MHz per count). Above 2457.6 MHz LO the
NUMCYCLE-12 count wraps. With the synthesizer running it counts only with
`LOCNTCTRL.FCALRUNCLKEN` (bit 11; 0 without it); stopped, in the calibration state, it counts the
open-loop VCO. `LOCNTTARGET` in the calibration state is the programmed LO's count for the
NUMCYCLE of the last RUN; CAPCALSTART and CLEAR set the NUMCYCLE-0 value (125); closed loop it
held 4004 (the NUMCYCLE-5 value) whatever the NUMCYCLE. Single short windows read about +0.25
count high on average (a control with no step); every timing below subtracts a same-timing
control or a settled reference. Hazard found on the way: an RXWARM recalibration started while
the probe had left `FCALRUNCLKEN` set ended with `VCOTUNING` 0x0780 and the LO unlocked near
2236 MHz; the sequencer's calibration needs `LOCNTCTRL` idle (the open firmware never sets that
bit).

**Mechanisms that move the carrier [F unless marked].**
- `SYNTH.FREQ` (28 bits, f_HFXO / 2^19 = 73.24 Hz per unit), written while the loop runs: the LO
  moves through the loop; no recalibration, no RAC state change (RXSEARCH / RXFRAME continued in
  every experiment), frames kept arriving.
- `CHCTRL.CHNO` x `CHSP` (6 and 18 bits): CHSP 13653 (1.000 MHz) moved the LO by 213-214 counts
  (NUMCYCLE 12) per CHNO; the same loop path.
- `CALOFFSET` (15-bit two's complement, FREQ units): 0x100, 0x1000, 0x4000, 0x7FFF moved it by
  +5, +64, -256, 0 counts, i.e. +19 kHz, +300 kHz, -1.2 MHz, 0: a +-1.2 MHz fine offset.
- `MODEM.DIGMIXCTRL.DIGMIXFREQ` (receive, digital): shifts what the demodulator sees without
  touching the LO; the frequency-offset estimate follows it as it follows an LO offset (below).
- `VCOTUNING` written closed loop, `CMD.VCOADDCAP` / `VCOSUBCAP` (VCAPSEL +-1, once): the loop
  absorbed the change; not frequency steps.
- `MODEM.AFCADJRX` / `AFCADJTX` are read-only (the AFC writes the synthesizer itself, LE Coded
  only). `DIVCTRL.LODIVFREQCTRL` and the sub-GHz test divider (`SYTRIM1.SYLODIVSGTESTDIV`,
  `SYLOEN.SYLODIVSGTESTDIVEN`) exist in the register model; not exercised. The FSK modulator
  (TX deviation) also moves the synthesizer per symbol; not used here.

**FREQ step response without recalibration [F] (E2, E11, E14; RXSEARCH, 2399 and 2402 MHz).**
Staggered NUMCYCLE-3 windows (0.42 us) after the write, 64 repetitions, control subtracted;
fraction of the step reached at the window's start (1 and +-4 MHz have the same shape):

| after the write | 0.31 us | 0.76 us | 1.03 us | 1.32 us | 1.95 us | 2.6 us | 4 us |
|---|---|---|---|---|---|---|---|
| receive loop, +4 MHz | 0.39 | 0.65 | 0.79 | 0.99 | 1.13 | 1.19 | 1.14 |
| receive loop, -4 MHz | 0.35 | 0.65 | 0.80 | 0.89 | 1.10 | 1.22 | 1.14 |
| loop configured for TX, +4 MHz | 1.15 | 1.11 | 1.02 | 1.05 | 1.06 | 0.97 | 1.04 |

Second-order fit over n = 240 windows each (rms 0.09 / 0.08 of the step): receive loop zeta 0.45,
f_n 196 kHz (overshoot 20 %, peak 2.9 us, within 2 % after about 7 us; NUMCYCLE-5 traces stayed
within +-2 % from 4-12 us on); TX configuration (the sequencer's `LPFCTRL1/2TX`, `DSMCTRLTX`,
`SYNTHENCTRL.LPFBWSEL`, `SYEN.SYCHPLPEN` clear and the `SYTRIM0` TX value, applied while
receiving) zeta 0.46, f_n 683 kHz (peak 0.8 us, within 5 % after about 1 us). The LO moves
within 0.3 us of the write. Settled, the LO is exact: 1, 2, 10 and 24 MHz steps read the count of
the programmed LO to the count.

**Large steps without recalibration [F] (E16; one repetition, NUMCYCLE 10, 53 us windows).** From
2399.7 MHz LO, steps up to +9.9 / -11.6 MHz arrive with the fast response (+8, +10, -8, -10 MHz:
within 0.02 MHz by the second window); beyond, the LO jumps to that edge at once and covers the
rest in uneven stages, on average 10.6 MHz/ms: +12 MHz in 0.33 ms, +16 in 0.65, +20 in 0.97,
+24 in 1.4 ms; -16 in 0.49, -24 in 1.4 ms; back from +24 in 1.1 ms. No interrupt flag changed
(`IF` 0x204 throughout). One calibration covers -11.6 / +9.9 MHz at full speed and at least
+-24 MHz eventually.

**Retune with calibration [F] (E4).** The M33 ran a port of the sequencer's TX->RX synthesizer
half (SYNTHSTOP, FREQ, VCDAC and loop filter, CAPCALSTART, the 11-step successive approximation
of `VCOTUNING`, SYNTHSTART; the sequencer's delay units taken as 1 us); n = 8 per row, steps of 1,
2, 10, 24 and 78 MHz. Entry to the calibration state 5.1 us, approximation 22.3-22.4 us,
SYNTHSTART at 27.4-27.6 us, sequence 32.6-32.7 us. With a cached code instead of the
approximation: SYNTHSTART at 5.4 us, sequence 10.6 us. After the sequence (5.2 us after
SYNTHSTART) the LO was 1.4-2.3 MHz off over the next 1.7 us, under 1 MHz at 3.4 us and within
about 0.2 MHz by 10 us, the same for 1 and 78 MHz (the loop acquires from the calibrated code,
not from the old carrier). The approximation's 11 counts are 9.4 us of hardware time; the rest is
register traffic (the M0+ pays 4.25 cycles of 26 ns per radio access, 7.74, the M33 3.9-5.9 of
13 ns). The code repeated within 2 (0x8332 against the sequencer's 0x8334 at 2402 MHz).

**Range [F] (E3, E5, E13; receive only, nothing radiated but LO leakage).** Calibration codes
(VCAPSEL 16, the sequencer's) fall about 4.7 per MHz: 2047 at 2160, 1880 at 2200, 1366 at 2300,
824 at 2400, 621 at 2450, 420 at 2500, 83 at 2600, 0 at 2640 MHz. Open loop (calibration state,
NUMCYCLE 8) the VCO spans 2165.7 MHz (code 2047) to 2631.3 MHz (code 0) at VCAPSEL 16, 2199.7 to
2692.9 at VCAPSEL 0 and 2140.4 to 2586.7 at VCAPSEL 31; the code's top bit has a step of its own
(code 1024 is 14 MHz above 960), as 7.85's 2370-2380 MHz jump suggested. Closed loop, 400 us
after each calibration, the LO count against the programmed LO (carrier + 0.7 MHz IF), 10 MHz
steps over 2000-2900 MHz and 2 MHz steps at the edges:

| VCAPSEL | LO on the programmed value (within 4.7 kHz), carriers | LO stops at (beyond) |
|---|---|---|
| 16 (the sequencer's) | 2158-2644 MHz | 2156.47 / 2647.57 MHz |
| 0 | 2192-2702 MHz | 2189.34 / 2703.76 MHz |
| 31 | 2134-2596 MHz | 2134.12 / 2600.92 MHz |

The synthesizer reaches 2134-2702 MHz with the VCAPSEL chosen per edge and 2158-2644 MHz with the
open sequencer as it is. At the edges the calibration code sits at 2047 or 0 and the loop's own
range covers the last 2-4 MHz. `STATUS` bit 16 (`VCOFREQACTIVE`) was set during calibration and,
after it, wherever the LO missed its value and at the last edge points it still reached (7 of
336), clear everywhere else: a near-the-edge flag. `STATUS.INLOCK` stays 0 in receive (7.64).
Receive sensitivity off the 2.4 GHz band was not measured: there is no reference signal there.
Nothing was transmitted outside the ISM band. `EFR32_AMATEUR_SYNTH_MIN/MAX_KHZ` (the amateur
carrier policy, 7.85) is unchanged.

**The demodulator's frequency-offset estimate [F] (E6; frames from nearby advertisers on 2402
MHz, 1.5 s per offset).** `MODEM.FREQOFFEST` (13-bit two's complement) is live during a frame and
reads 0 again by RXDONE. Sampled while the RAC is in RXFRAME, LO offsets of -300..+300 kHz
(SYNTH.FREQ) moved it by -3.915 kHz per LSB (least squares, n = 1637 frames) and DIGMIXFREQ
offsets by +3.937 kHz per LSB (n = 1468): within 0.6 % of each other, so the LO lands where it is
programmed. [I] The LSB is 4 MHz / 2^10 (the 1M PHY's SRC output rate). Per-frame scatter for
one device 1.7-5.6 LSB (7-22 kHz). Frames still passed CRC at every offset to +-300 kHz, LO or
mixer. `TIMDETSTATUS.TIMDETFREQOFFEST` read 0 throughout.

**Stepped chirps from the M33 [F] (E12, E14).** Two up-chirps around 2402 or 2399 MHz written on
a DWT schedule (lateness <= 7 cycles, 91 ns), one LO count per repetition started at a chosen
step, 32 repetitions, compared with the programmed staircase after a same-timing control: mean
frequency error per window (NUMCYCLE 8: 13.3 us; 7: 6.7 us; 5: 1.7 us):

| chirp | steps per symbol | step | error, receive loop | at the wrap (window after) |
|---|---|---|---|---|
| SF7, 812.5 kHz | 128 / 32 / 16 | 1.23 / 4.92 / 9.84 us | -14..+11 kHz | 0..+14 kHz |
| SF7, 1625 kHz | 128 / 32 | 0.62 / 2.46 us | -24..+15 kHz | +64 / +72 kHz, then -55 / -28 |
| SF7 406.25 kHz, SF7 203.125 kHz, SF8 812.5 kHz | 128 / 128 / 256 | 2.46 / 4.92 / 1.23 us | -19..+20 kHz | not resolved |
| scaled: 12.8 MHz in 160 us | 128 | 1.25 us | -0.10..+0.19 MHz | +9.4 MHz, then -0.16 MHz |

One count over 32 repetitions is 2.3 kHz at NUMCYCLE 8: the spread is the method's. No tracking
lag is resolved; the scaled chirp, 16 times steeper, bounds it to within about 2 us. With the
loop configured for TX: 812.5 / 1625 kHz -21..+18 / -39..+27 kHz, no wrap excess resolved; the
scaled chirp's 12.8 MHz wrap, beyond the fast range, left -3.2 MHz 20 us later.

**Transmit [F] (E15; in band: channel 37 advertising at normal power, a step to 2402.5 MHz and
back inside each of 40 PDUs).** The RAC stayed in TX through every step (40 / 40) and `INLOCK`
read 1 in all 1200 samples: the transmitter takes FREQ writes while transmitting without losing
lock. The LO counter in TX read means 2-3 MHz below the carrier with +-1.5 MHz scatter between
windows: not interpretable. Whether the radiated carrier follows needs an SDR.

**What needs an SDR.** Phase continuity across FREQ writes (a VCO in a loop cannot jump in phase,
but how the DSM takes a new word is unknown): I/Q of a carrier stepped by 1-800 kHz at known
instants, >= 4 MS/s, phase residual against the programmed trajectory. The radiated frequency
during stepped transmission (E15), a stepped chirp's spectrum and a gr-lora_sdr decode of a
stepped-chirp frame. Spurs from the LO counter running closed loop. Receive sensitivity off the
band and the LO leakage during a range sweep.

**Viability [I from the measurements above].** LoRa symbol time 2^SF / BW: SF7 630 / 315 / 158 /
79 us and SF8 twice that at 203 / 406 / 812 / 1625 kHz; one step per chip is 4.92 / 2.46 / 1.23 /
0.62 us.
- Stepped-chirp TX: timing is not the limit. The M0+ writes FREQ in 0.11 us; one step per chip is
  possible at every bandwidth, and 16-32 steps per symbol keep the staircase's phase error small
  (the loop smooths it further). The LO follows a stepped ramp without resolved lag. The weak point
  is the symbol-boundary wrap (a -BW jump): 20 % overshoot settling in ~7 us in the receive loop
  (4-9 % of a 158 / 79 us symbol), ~1 us in the TX one (under 2 %). Open: the radiated carrier and
  its phase (SDR), and a modulator mode that sends an unmodulated carrier for a whole frame (the
  OOK path of 7.85 holds the carrier at key-down).
- De-chirp RX by LO sweep: the LO can be stepped while receiving without disturbing the RAC or the
  demodulator, and static offsets land exactly (FREQOFFEST, LO count). The ~+-3 kHz holds once
  settled: a 6.35 kHz step (SF7, 812.5 kHz) overshoots ~1.3 kHz and is within 2 % in ~7 us. The
  wrap again costs ~7 us per symbol in the receive loop. The digital mixer (DIGMIXFREQ) shifts the
  same way without analogue settling over at least +-300 kHz (frames decoded); a de-chirp could
  sweep the mixer within the channel filter and move the LO only between symbols. Per-symbol
  readout is the gap: FREQOFFEST has 3.9 kHz steps and updates only within a detected frame.
- Spectrum scan: steps within -11.6 / +9.9 MHz settle to 2 % in ~7 us without recalibration;
  beyond, a recalibration (SYNTHSTART 27.4 us after the start on the M33, 5.4 us with a cached
  code) plus ~10-15 us. The 2400-2483.5 MHz band in 1 MHz steps is ~1 ms of tuning (84 x ~7 us
  + 6 recalibrations); the span is 2158-2644 MHz with the sequencer's VCAPSEL, 2134-2702 MHz with
  VCAPSEL chosen per edge. The RSSI time per point (AGC) was not measured.

**QEMU.** The model now has the LO in time: the LO counter (FCALRUNCLKEN closed loop; the open-loop
VCO in the calibration state from the measured curve and its VCAPSEL dependence; TARGET on RUN,
CAPCALSTART and CLEAR; NUMCYCLE saturating at 12; the 19-bit wrap), so the sequencer's calibration
converges to silicon-like codes (the probe in the model: 0x832f, 0x82c5, 0x81e3 at 2402, 2426,
2480 MHz against silicon 0x8332-0x8334, 0x82cc, 0x81e5; counts 512576, 512789 and 514709 at
2402, 2403 and 2412 MHz, as on silicon; LO on target 2160-2646 MHz at VCAPSEL 16, silicon
2158-2644; the +-24 MHz slews within 0.2 ms of silicon's); the closed-loop LO follows carrier changes
with the fitted second-order response (receive or TX loop by `LPFBWSEL`), the fast range and the
slew beyond it, and stops at the measured VCO limits per VCAPSEL. `CHNO` x `CHSP` (6 / 18 bits;
the CHSP mask was 6 bits) and `CALOFFSET` add to the carrier. The medium matches no receiver while
a running LO is more than half the raster from its carrier. The lock range is 2158-2644 MHz (was
2290-2500), compared to the kHz, and the medium's channel index is 16 bits (the 1 MHz raster from
2158 MHz has 487 channels). [I] A SYNTHSTART with a code far from the carrier (no calibration) is
taken as locked. qtests: `synth-locnt-counts-lo`, `synth-calibration-open-loop`,
`synth-freq-step-settles`, `synth-large-step-slews`, `synth-lo-stops-at-vco-limit`,
`synth-channel-and-offset-fields`, and the range qtests at the new edges; rf 43/43 and machine
123/123 qtests pass, and the amateur dual-machine e2e passes.
