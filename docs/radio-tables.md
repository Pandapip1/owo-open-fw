# EFR32BG22 radio: register/table knowledge needed by the Zephyr LL port

**Unvalidated on hardware.** Labels: **[F]** = fact from a cited public source or directly observed in a stock-firmware trace in the QEMU
model; **[I]** = inference, not confirmed. No blob bytes or disassembly are reproduced here. The sibling task (QEMU radio model,
`docs/radio-block.md`) had not committed when this was written; this file does not duplicate it and should be reconciled with it.

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
| RF mailbox | 0xA8028000, 0xA802C000 | [I] |
| BUFC | 0xAA000000 | [F] layout matches the xG21 public header; matched by stock init trace |

The public rev-B2 models establish a narrower, usable fact: FRC, AGC, RFCRC, MODEM, SYNTH, and RAC each have writable one-bit `EN.EN` at `+0x004` (their respective `*_S_register.py:24-37` and `*_S_field.py:17-24` files). `radio_setup()` enables exactly those six blocks, and QEMU masks the matching latches to bit zero with a native regression. Separately, the stock helper writes `1` at secure BUFC and PROTIMER `+0x004` at `flash-stock.bin` offsets `0x26de4` and `0x26dfc` (target literals `0xAA000004` and `0xA801C004` at `0x2723c` and `0x2724c`); the HAL mirrors those direct writes through the established non-secure aliases because it uses both blocks. This is not a claim that either bit is named `EN`, nor does it establish ordering, reset sequencing, or interrupt policy. No conclusion is drawn for PRORTC or mailbox blocks. IRQ lines (AGC 31 ... SYNTH 42) are [F] from the device header
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

The BLE 1M delta begins with `0x03014FFC, &phyInfo_0`, labelled `SEQ.PHYINFO` by the public table (`ble_efr32xg22_configurator_out.c:474-476`). The generated rev-B2 model locates that field at `0xB0004FE0 + 0x1C = 0xB0004FFC` and defines it as a full-width address (`SEQ_register.py:130-143`, `SEQ_field.py:127-134`). `phyInfo_0` itself is a 20-word configuration object containing several source-language pointers (`ble_efr32xg22_configurator_out.c:87-110`), so its source address is not portable into the open image. The native QEMU machine aliases `0xB0000000..0xB0007FFF` to sequencer/RFC RAM, which naturally retains the pointer word; no duplicate MMIO model is appropriate. The open HAL must not copy the vendor pointer record until an address-independent PHYINFO representation and sequencer consumption contract are established.

The OpenISA call order makes the public BLE baseline's dynamic defaults safe to replace, rather than a reason to omit the whole table: both advertising and scanning call `radio_phy_set()` before `radio_pkt_configure()`, then write their access address and CRC immediately afterwards (`lll_adv.c:352-361`; `lll_scan.c:159-172`). In particular, the base table's default `MODEM.SYNC0`, `RFCRC.INIT`, and `RFCRC.POLY` cannot survive those later API calls. Whitening is also deliberately not entrusted to the table: the LLL applies `radio_whiten_iv_set(chan)` after its channel selection (`lll.c:445`). `hal/radio.c` now imports the exact public base (137 words) plus 1M, 2M, and coded deltas (85 words each) through a readable decoder, omitting only each `SEQ.PHYINFO` record; a mechanical comparison against the source arrays verifies every retained word. OpenISA defines `PHY_FLAGS_S2 = 0` and `PHY_FLAGS_S8 = BIT(0)` (`pdu.h:282-288,314-318`), so the importer maps coded S2 to the public `ble_LR_DSA_500kb_modemConfig` table (`configurator_out.c:663-724`) and S8 to `ble_LR_DSA_125kb_modemConfig` (`:600-661`); the associated public profile names identify their 500/125 kb/s roles (`configurator_out.h:68-76`). This is a source-backed configuration step, not a claim that the table alone configures packet transmission or sequencer behavior.

## 3. Per-function blockers (IDs used in `modules/efr32-radio/hal/radio.c`)
| ID | Needs | Status | Functions |
|---|---|---|---|
| T1 | block enables/reset, which IRQ ends an event | **[F] six core EN latches implemented; stock's raw BUFC/PROTIMER `+0x004 = 1` setup writes are mirrored without assigning bit semantics; RAC has separate RSM and SEQ flag/enable banks; reset and event policy remain unknown. OpenISA-compatible sleep/wake availability accounting is software-only.** | `radio_setup/reset/sleep/wake/is_off`, `LL_RADIO_IRQn` |
| T2 | RAC command + state/status registers | **[F] core command/state map known; sequencer behavior still abstract** | `radio_rx_enable`, `radio_tx_enable`, `radio_disable`, `radio_is_*`, `radio_status_reset`, TIFS switch |
| T3 | PHY table apply (MODEM/FRC/AGC/SYNTH data) | **[F] format decoded; address-independent BLE base plus 1M/2M/coded S2/S8 deltas imported; PHYINFO remains unresolved** | `radio_phy_set` |
| T4 | SYNTH channel/frequency registers | **[F] implemented from public BLE channel config and CHCTRL field** | `radio_freq_chan_set` |
| T5 | FRC descriptors: sync word, CRC poly/init, whitening, length/header | **[F] RFCRC and BLE whitening seed implemented; public BLE 1M FRC baseline and descriptor fields decoded, but sequencer handoff/buffer behavior remains incomplete** | `radio_aa_set`, `radio_crc_configure`, `radio_whiten_iv_set`, `radio_pkt_configure`, `radio_crc_is_valid` |
| T6 | BUFC RX/TX buffer setup | **[F] TX channel-0 and RX channel-1 staging setup implemented; a dormant normalized-PDU RX copy helper enforces controller bounds, but FRC/BUFC handoff remains incomplete** | `radio_pkt_rx_set`, `radio_pkt_tx_set` |
| T7 | PA power registers | **[F] RAIL routes raw and dBm requests through configuration-dependent helpers; a board-safe PA setting remains unresolved** | `radio_tx_power_*` |
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

The stock trace gives one tighter ordering fact for buffer 3: at `radio-trace.log:283-287` (virtual time 99,318,096 ns), it writes `BUFC+0x0f4 = 1`, reads `BUFC+0x0d0 = 0x20002f50` and `+0x0d4 = 0`, writes global `BUFC+0x100 = 0`, then writes `BUFC+0x0f4 = 4`. The same five-access sequence repeats at `radio-trace.log:293-297` (99,320,816 ns). This demonstrates that firmware consumes the programmed address around these command values, but it does **not** establish which command starts DMA, direction, byte count, or completion condition; the QEMU model therefore still does not perform a DMA transfer.

The public BG21 header (`platform/Device/SiliconLabs/EFR32BG21/Include/efr32bg21_bufc.h:224-243`) gives the compatible `BUF.CMD` mask `0x0f`, with bit 0 named `CLEAR` and bit 1 named `PREFETCH`. The QEMU model retains that masked register value and implements only `CLEAR`, which agrees with the trace's first command. Bit 2 (`0x4`) is within the mask but unnamed in that header; it is retained for software visibility with no invented side effect.

The RAIL archive narrows the `CMD=4` sequence without naming that bit. In `librail_efr32xg22_gcc_release.a(rfhal_bufc.o)`, section `.text.sli_rail_adf0dd5a9b9483ac88bd66365969a5a1` starts at object file offset `0x3a6`. At file offsets `0x3cc..0x3e2` it writes `CMD=1`, then buffer-3 `CTRL`, `ADDR`, and `WRITEOFFSET`; at `0x3de` it writes `CMD=4`; and, only when the write offset is nonzero, at `0x3ea` it writes public `PREFETCH` (`CMD=2`). This corroborates the trace ordering and shows `CMD=4` is part of buffer setup, but does not assign it a name or establish DMA/packet behavior, so QEMU still gives bit 2 no side effect.

The same public header defines identical global `IF` and `IEN` masks, `0x8F0F0F0F` (`efr32bg21_bufc.h:247-338`): four sparse four-bit buffer groups at bit positions `0..3`, `8..11`, `16..19`, and `24..27`, plus bus error at bit 31. Within each group, bit 0 is overflow, bit 1 underflow, bit 2 threshold, and bit 3 corrupt (`efr32bg21_bufc.h:250-329`). Its threshold flag/enable bits are therefore at `2`, `10`, `18`, and `26`, rather than a compact four-bit bitmap. This is corroborated by the stock write `BUFC.IEN = 0x00050000` at `radio-trace.log:52-53`, which enables buffer-2 overflow and threshold positions. QEMU masks both latches to that source-backed value, uses the public threshold positions, and latches the channel-overflow bit when pointer-derived occupancy exceeds its configured capacity; its regression covers a 64-byte buffer's 65th port write. Underflow, corruption, and any RAIL packet/error recovery policy remain unmodelled.

The downloaded stock sequencer-RAM capture adds a distinct programmed-I/O fact. In the untracked `scratch-radio.tgz(radioram.dump)`, Thumb code at RAM offset `0x0ce4` (`0xA0000CE4`) loads the literal at member offset `0x0e30`, `0xA8004090` (`FRC.RXRAWDATA`), then loads the `0xAA000064` literal at member offset `0x0e34` (secure buffer 1 `WRITEDATA`). The instruction range `0x0d50..0x0d68` performs four 32-bit stores to that port: `0x0d56` writes `RXRAWDATA[15:0]` after `UXTH`, and `0x0d5e`, `0x0d64`, and `0x0d68` write separately extracted bits `15:8`, `23:16`, and `31:24`. Thus the capture establishes repeated programmed-I/O writes from the raw-data word, but does *not* establish four one-byte bus accesses: its first store's value contains two low raw-data bytes, and the peripheral's port-width/value-consumption behavior is not recoverable from these instructions alone. The public revB2 model identifies the source as read-only `FRC_S.RXRAWDATA` (`FRC_S_register.py:871-885`); the trace-established xG22 layout identifies the destination as `0xAA000000 + 0x04c + 0x018`. This proves that the captured sequencer feeds that port with programmed I/O from FRC raw RX data; it does not establish packet framing, FIFO ownership, byte count, or DMA behavior. QEMU aliases secure and non-secure BUFC windows to the same data-port state and mirrors each port write into the configured guest-RAM backing address: the public RAIL contract defines that address as a circular FIFO buffer receiving incoming packet data (`rail.h:1817-1821`). Its data-port regression verifies both the secure-buffer-1/non-secure-alias read and the resulting guest-RAM byte. It also makes the source register read-only to guest writes, matching the public model; a future internal receive producer may supply it.

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
This is only a literal-search fact: it does not exclude a computed channel-2
address, and it is not evidence that any listed access executes. It does rule
out claiming a plainly encoded sequencer descriptor producer from this image.

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

The stock handler separates FRC status acknowledgement from descriptor
delivery. It computes and W1C-acknowledges the entire enabled FRC pending mask
at `0x28660..0x28672`, but its direct FRC branches test bits 16, 2, and 14
only (`0x28708..0x28780`); it does not branch on FRC `RXDONE` bit 4 before the
whole-mask acknowledgement. Conversely, BUFC pending bit 18 branches at
`0x2883e..0x28846` to `0x29664`. That helper selects an eight-byte record from
the software ring rooted at RAM `0x20001178` (literal at `0x29860`), advances
the ring by eight bytes, writes the updated position to BUFC `0xaa0000b0`,
acknowledges BUFC flag `0x40000` through `0xaa002114`, and publishes the
selected record pointer at state `+0x08` (`0x29664..0x296a4`). Its return is
the selected record's byte `+2` low three bits; the ISR dispatches values 1–7
through the table at `0x28846..0x28880`. It subsequently invokes deferred
record handling at `0x296a6` before clearing the BUFC pending mask
(`0x28b28..0x28b36`). Thus BUFC channel-2 threshold is the concrete stock
descriptor-delivery route; FRC `RXDONE` is not, on the current evidence, a
direct PDU-delivery condition. The meaning of record byte `+2`, its payload
framing, and which dispatched result represents a CRC-valid BLE packet remain
unrecovered.

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
Neither condition is exclusive to the BUFC descriptor class. There is a
potential bridge through the ISR's `r7` bit 10 at `0x28b10..0x28b1c`, which
calls `0x2847c(2)` and contributes its result to the third generic event-set
argument, but its relation to the descriptor return class is not yet proven.

The open port consequently binds `LL_RADIO_IRQn` to direct `FRC_IRQn` and enables only `FRC.IEN.TXDONE|RXDONE` during `radio_setup()`. The generated revB2 model places `IEN` at `+0x06C`, exposes the standard SET alias, and names those two bits (`FRC_S_register.py:684-704`, `FRC_S_field.py:1197-1254`). This is deliberately narrower than the vendor handler: it gives the existing `radio_is_done()` completion policy an identical IRQ source, without treating an arbitrary sequencer, buffer, error, or RAC state flag as successful completion. It does not establish receive production or the rest of RAIL's packet-state dispatch.

The generic OpenISA LLL, not the radio HAL, installs and enables this route: `lll_init()` uses `IRQ_CONNECT(LL_RADIO_IRQn, ..., isr_radio, NULL, 0)` then enables `LL_RADIO_IRQn` (`lll/lll.c:142-155`). Since this port maps that macro to `FRC_IRQn` (`include/hal/efr32/ll_irqs.h:14-18`), `radio_isr_set()` only swaps the callback parameter and `isr_radio()` invokes it. The FRC enable policy above confines that callback path to TXDONE/RXDONE in this port; it does not make any other FRC source a completion.

The open HAL implements the source-backed TX/abort/idle subset: `radio_tx_enable()` waits for `STATUS.STATE=OFF` then writes `TXEN`; `radio_disable()` performs separate write-only `RXDIS` then `TXDIS` commands; and `radio_is_idle()` returns whether `STATUS.STATE` is OFF. The idle-before-TX sequencing is the portable OpenISA contract (`hal/RV32M1/radio/radio.c:811-818`), and the advertising path invokes it at `lll_adv.c:698`. The field definitions are public generated revB2 source: `RAC_S_field.py:207-214` (`TXEN`), `:157-164` (`STATE`), `:257-264` (`TXDIS`), `:287-294` (`RXDIS`), `:797-804` (`SEQIF.STATETX`, bit 25), and `:707-714` (`SEQIF.STATEOFF`, bit 16) in the sibling `silabs-simplicity-sdk` checkout. Separate abort writes intentionally avoid claiming that a combined command write is hardware-equivalent. The native QEMU qtest covers `TXEN` then `TXDIS` state transitions and their matching `SEQIF` bits.

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
hand-off, so the open HAL deliberately does not reproduce them.  It retains
the proven scratch clear and force-disable-to-OFF prefix, then clears the
force-disable latch.  QEMU gives `CTRL.FORCEDISABLE` this narrow cancellation
effect, including cancellation of its synthetic TX timer; it does not infer
the omitted sequencer behavior.

## 4.2 SYNTH, FRC, and AGC fields [F]
The same public revB2 model identifies the remaining core data-path fields. SYNTH has 28-bit `FREQ` at `+0x034`, 21-bit `IFFREQ` at `+0x038`, `DIVCTRL` at `+0x03C`, 6-bit `CHCTRL.CHNO` at `+0x040`, and 18-bit `CHSP` at `+0x044`. The public BLE 1M channel config specifies base 2402 MHz, 2 MHz spacing, physical channels 0..39. Zephyr passes MHz relative to 2400 (`2`, `26`, `80` for advertising), so `radio_freq_chan_set` now validates that input and writes `CHNO = (chan - 2) / 2`. QEMU preserves these fields with their public masks. The PHY table remains responsible for `FREQ` and `CHSP`.

FRC exposes dynamic-frame-length configuration at `DFLCTRL +0x00C`, `MAXLENGTH +0x010`, frame/length/address word-count comparators at `+0x020..+0x028`, whitening control/polynomial/init at `+0x030/+0x034/+0x038`, RX control at `RXCTRL +0x04C`, and four frame configuration descriptors `FCD0..3` later in the block. `MAXLENGTH` is specifically a 12-bit `MAXLENGTH` field in bits 0..11 plus a four-bit `INILENGTH` field in bits 12..15 (`FRC_S_register.py:118-133`, `FRC_S_field.py:277-294`); the BLE tables' `0x4101` is therefore not a one-field byte maximum. The public model also gives eight-bit `DATABUFFER +0x018` and four-bit `BUFFERMODE +0x078` (`FRC_NS_register.py:161-170,779-793`, `FRC_NS_field.py:340-344,1820-1844`), plus `PKTBUFCTRL` at `+0x0CC` (mask `0x0103FFFF`) and read-only `PKTBUFSTATUS` at `+0x0D0` (six-bit count). The current public xG22 rev-B1 model identifies `SNIFFCTRL +0x084` as a read/write register with mask `0x0003FFFF` (`host_py_rm_studio_internal/full/efr32xg22/revB1/FRC_S_register.py:798-804`); stock writes it through the standard CLR then SET windows at `flash-stock.bin` offsets `0x2a6cc` and `0x2a6d0`, so QEMU preserves that exact masked latch without assigning sniff behavior. The same model identifies `RAWCTRL +0x08C` with mask `0x000021BF` (`FRC_S_register.py:848-867`); the historical stock trace writes this register at `radio-trace.log:91`, so QEMU likewise constrains the latch but does not infer raw-data behavior. `RXCTRL` has mask `0x000007FF`; it names RX packet/error-policy fields but does not itself establish packet-buffer behavior. The same public `FRC_NS_register.py:610-699,998-1087` defines `IF/IEN` at `+0x068/+0x06C` and `SEQIF/SEQIEN` at `+0x0B4/+0x0B8`, each with writable mask `0x7F7FFFFF`; QEMU retains those latches and verifies the masks. The current public rev-B1 model additionally identifies historical configuration offsets `MODEM.IF/IEN +0x208/+0x20C` (mask `0x0007FFFF`) and `MODEM.IRCAL +0x228` (mask `0x0000FFBF`) (`MODEM_S_register.py:1641-1707,1841-1854`); QEMU constrains those latches, but does not synthesize modem IRQs or calibration. That preserves software-visible pending/enable bits without assigning completion or IRQ policy. The generated `FRC_S` and `FRC_NS` models give the same offsets/masks at secure `0xA8004000` and non-secure `0xB8004000`; Zephyr's xG22 devicetree exposes `radio@b0000000` (`efr32xg22.dtsi:11-24`), so the open HAL uses the latter. RFCRC similarly has secure/non-secure bases `0xA8010000`/`0xB8010000`.

The public xG22 BLE 1M base table records `DFLCTRL = 0x00018101`, `WCNTCMP0/1 = 0/1`, whitening control/poly/init all zero, `CONVRAMADDR = 0x4000`, and `FCD1/2/3 = 0x4CFF/0x4DFF/0x4DFF` ([F] source locator: `ble_efr32xg22_configurator_out.c:225-242`). Its per-PHY table adds `MAXLENGTH = 0x4101`, `FECCTRL = 0`, `CTRL = 0x407A0`, and `FCD0 = 0x4CFF` (`...:474-481`); it also writes `SEQ.PHYINFO`, whose effect is not inferred here. QEMU retains the public masks for the decoded `MAXLENGTH`, `CTRL`, and `FCD0..3` registers and the base-table `WCNTCMP`, RAM-address, FEC, trailing-data, convolution, puncture, clock-gate, and supplemental-length latches using the generated public masks (`FRC_S_register.py:190-229,332-401,474-577,966-979,1146-1229`). It assigns none of them packet-pipeline effects. The generated field definitions (`FRC_S_field.py:197-274`, `:3357-3434`) decode `DFLCTRL` as `DFLMODE=1`, `DFLOFFSET=1`, `DFLBITS=8`, `MINLENGTH=1` (the other named fields zero). They decode each `0x4CFF` descriptor as `WORDS=255`, `BUFFER=0`, `INCLUDECRC=1`, `CALCCRC=1`, `SKIPWHITE=1`, with the remaining named fields zero; `0x4DFF` differs only by `BUFFER=1`. This establishes public configuration values, not their packet-pipeline or buffer semantics.

The portable OpenISA LLL consistently calls `radio_pkt_configure(8, max_len, flags)`: advertising (`lll_adv.c:353,356`), scanning (`lll_scan.c:160,163`), and direct test (`lll_test.c:195`) use their respective legacy or test payload maxima; connection setup uses its negotiated RX/TX octets, adding the four-byte MIC when encryption is enabled (`lll_conn.c:444-450,480-487`). These call sites establish that the API's second argument is a byte limit and that `flags` varies with PHY and encryption. They do not establish the relation of that byte limit to the separate `MAXLENGTH`/`INILENGTH` fields or the FRC descriptors, so the open EFR32 HAL does not yet program them.

The RAIL frame-type callback does not close that gap. `RAILCb_ConfigFrameTypeLength` in `rail_rx.o` is a four-byte tail-call wrapper at archive file offset `0x1552a4`; its target `sli_rail_3bff6dfc0590d5c54bb0576abaeb38dd` in `rail_rf_hal.o` begins at `0x1434c`. The target stores a relocation-resolved helper address in a `.bss` slot and returns zero. It has no peripheral-address literal or MMIO store, so this callback path is not evidence for a `MAXLENGTH`, `INILENGTH`, or descriptor policy.

The xG22 RAIL archive gives a narrower, directly located length observation. `RAIL_SetFixedLength` is a four-byte wrapper in `rail_rf.o` around `sli_rail_308157e3c5d7e1d3aaf039be96b40226` in `rail_rf_hal.o`; the latter section begins at archive file offset `0x1123c` (member starts at `0x10dd4`, section-relative offset `0x468`). Its nonzero, non-`0xffff` path clamps the API argument at `0x1000`, then writes `argument - 1` to non-secure `FRC.WCNTCMP0` at `0xB8004020`; its zero and sentinel paths also update `FRC.DFLCTRL` at `0xB800400c`. The generated `FRC_NS` model identifies those offsets. This proves that RAIL's *fixed-length* API uses the non-secure FRC window and a word-count comparator, not that OpenISA's dynamic BLE `radio_pkt_configure(8, max_len, flags)` should use that policy.

The vendor TX-FIFO setup provides separate BUFC evidence. `RAIL_SetTxFifo` reaches `sli_rail_86983bfebc3fcf81f3ab3444e4625a88` in `rfhal_bufc.o`, whose 60-byte section begins at archive file offset `0x33414` (member starts at `0x332f4`, section-relative offset `0x120`). Its caller validates a four-byte-aligned FIFO address, reduces the requested allocation to the largest supported power of two in the 64–4096-byte range, and limits the initial count to that result. The called BUFC routine first writes command `1` at `0xBA000034`, then writes the derived configuration, FIFO address, and initial count to `0xBA00000C`, `+0x004`, and `+0x008`; it follows with command `2` when the initial count is nonzero. The transform is directly recoverable from the caller: for a selected allocation `2^n`, the first word is `n - 6` (so 512 bytes encodes as `3`), not a raw byte capacity. QEMU retains these channel-0 setup latches and derives FIFO occupancy (including threshold status) from `WRITEOFFSET - READOFFSET`, so that initial byte count is visible before any port access. It applies the same 64–4096-byte encoded capacity to backing-RAM port accesses, wrapping them at the configured circular-buffer boundary; its regression writes across a 512-byte boundary. It also reads a configured BUFC data port from that guest-RAM backing address, matching RAIL's documented zero-copy `RAIL_SetTxFifo()` use (`rail.h:1425-1429`); its regression preloads RAM and verifies the port returns that byte. These exact writes establish a FIFO setup contract, but not whether FRC descriptors consume the configured buffer or how a RAC transition launches it.

`radio_pkt_tx_set` now applies exactly that limited contract. It copies the OpenISA PDU's two-byte header plus its length-byte-selected payload into an aligned 512-byte staging FIFO, then writes `CMD=1`, configuration `3`, its address, the copied byte count, and `CMD=2` in the vendor order. QEMU's deliberately synthetic TXEN snapshot now reads that configured circular guest-RAM range (`ADDR`, `READOFFSET`, `WRITEOFFSET`) when present, while retaining the direct data-port staging path for register-level tests; its focused regression uses the same clear/configure/address/count/prefetch shape as the HAL. This makes the simulator observe the HAL's real staging representation, not an otherwise-empty internal port-write buffer. It remains a simulator snapshot boundary only: neither this change nor the QEMU latch model claims that FRC consumes channel 0, that RAC starts a packet, or that any bytes reach the air.

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
choice. The currently located stock `FRC+0x054` literals at file offsets
`0x1f7dc`, `0x28d68`, and `0x298a8` are read sites only; no literal-backed
stock store or `RAIL_ConfigRxOptions` call has been established. Computed
address writes remain possible, so absence of a located store is not evidence
for either option state.

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
indices at `+0x3a` and `+0x3c`. Static accesses in this object only read slot
fields `+0`, `+2`, and `+3`, except that the release helper writes slot `+0`
to reclaim it; the initializer writes no individual eight-byte slot, and no
sequencer-image static write to those fields has been located. This supports
BUFC hardware (possibly together with the sequencer) as the descriptor
producer, while the observed CPU paths configure, select, hold, and release
entries. It does not identify the producer's record encoding or time boundary.

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
separate, with a focused regression for the documented channel-2 setup; it
does not fabricate hardware descriptor production.

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
two ticks, then verifies reset returns it to zero. This is an emulator
timebase approximation only. It is not interchangeable with the
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
pending radio-launch compare are multiplexed to the earlier wrap-safe deadline.
The counter ISR only records the expiry and wakes a pre-created TX or RX
Zephyr thread. Those persistent threads run the normal radio enable path only
when selected by the kernel scheduler; disable/reset disarms the deadline and
invalidates its generation, but does not destroy the workers. This is an
intentional scheduler integration choice, rather than a claim that it matches
the unresolved vendor PROTIMER semantics.

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
