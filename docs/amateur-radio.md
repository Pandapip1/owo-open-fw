# Amateur-radio operation (47 CFR Part 97) of the EFR32 radio

Tags: **[F]** measured on silicon (BGM220 explorer kit, BGM220PC22HNA, J-Link OB 000440200548),
**[Q]** measured in QEMU (this tree's `vendor/qemu`), **[R]** rule text, quoted from the eCFR
(Title 47 Part 97, current as of 2026-11-01; latest amendment shown 91 FR 1430, 2026-01-14),
**[S]** Bluetooth Core Specification v6.1, **[D]** Silicon Labs public documentation
(EFR32xG22 reference manual rev 1.2, BGM220P datasheet rev 1.3, and the public register model
and radio configurator in the Simplicity SDK), **[I]** inferred, **[P]** placeholder.

This is a general capability of the open radio module (`modules/efr32-radio`), for licensed
amateur stations. **The OWO product never carries it**: the product needs link-layer encryption,
which amateur rules forbid, and its CMake refuses `CONFIG_EFR32_RADIO_AMATEUR`
(`CMakeLists.txt`). Its images are byte-identical with and without this work (section 9).

Nothing here is legal advice, and nothing here has been measured for compliance: the control
operator remains responsible for the station (47 CFR 97.103). What is and is not measured is in
sections 7 and 8.

## 1. Use

```
CONFIG_EFR32_RADIO_AMATEUR=y        # compiles it in; the radio still starts under Part 15
```

```c
#include "hal/efr32/amateur.h"

struct efr32_amateur_config cfg = {
	.callsign = "KD3APL",      /* the licensee's; "/indicator" allowed (97.119(c)) */
	.admin = NULL,             /* NULL: &efr32_amateur_us_part97 */
	.plan_khz = NULL,          /* NULL: default plan (section 3); else 40 carriers, kHz */
};
int err = efr32_amateur_enable(&cfg);   /* with no connection; advertising/scanning may run */
...
err = efr32_amateur_disable();          /* identifies (end of communication), then Part 15 */
```

`efr32_amateur_enable()` refuses (and the radio stays under Part 15): a missing, empty or
malformed call sign (`-EINVAL`; US format: prefix K, N, W, KA-KZ, NA-NZ, WA-WZ or AA-AL, one
numeral, one to three letters), any carrier outside the bands with the emission's guard or
outside the radio's measured synthesizer range (`-ERANGE`, `efr32_amateur_last_error_channel()`
names the channel), an existing connection (`-EBUSY`). `efr32_amateur_end_of_communication()`
identifies at once instead of after the idle timeout. Kconfig: `EFR32_RADIO_AMATEUR_CW_WPM`
(5-20, default 20), `EFR32_RADIO_AMATEUR_ID_LEAD_S` (default 30), and the test-only
`EFR32_RADIO_AMATEUR_SYNTH_PROBE` (section 7).

Code: `hal/amateur_rules.c` (rules, hardware independent), `hal/amateur.c` (the switch, the
identification job, the encryption policy), `hal/radio.c` (carrier, OOK frame, CCM refusal),
`include/hal/efr32/amateur.h`; Zephyr: `BT_CTLR_LE_ENC_POLICY` (vendor/zephyr, section 4).

## 2. The rules this implements

Table-driven (`struct efr32_amateur_admin`): another administration is another table of bands,
interval, speed limit, power ceiling and call-sign format. Only US Part 97 is provided.

| rule [R] | text (excerpt) | firmware |
|---|---|---|
| 97.301(a), Region 2 | "13 cm \| 2300-2310 ... (d), (p)"; "2390-2450 ... (d), (e), (p)" (Technician and up) | the band table |
| 97.303(d), (e), (p) | secondary to radiolocation (d), to fixed and mobile of other nations (p)(1); 2305-2310 MHz under FCC fixed, mobile and radiolocation (p)(2); receivers in 2400-2450 accept ISM interference (e) | documented; the operator's responsibility |
| 97.305(c) | "13 cm Entire band MCW, phone, image, RTTY, data, SS, test, pulse" | BLE packets are data (F1D) |
| 97.305(a) | "an amateur station may transmit a CW emission on any frequency authorized to the control operator" | the CW identification |
| 97.307(a), (b) | not "more bandwidth than necessary"; "Emissions resulting from modulation must be confined to the band or segment" | the guard (section 3) |
| 97.307(c) | "All spurious emissions ... must be reduced to the greatest extent practicable" | **unmeasured** (section 8) |
| 97.309(a), (b) | specified codes (ASCII, ...), and unspecified codes "must not be transmitted for the purpose of obscuring the meaning" | BLE's framing and whitening are publicly documented [S]; no encryption |
| 97.113(a)(4) | "messages encoded for the purpose of obscuring their meaning" are prohibited | encryption refused (section 4) |
| 97.119(a) | "transmit its assigned call sign on its transmitting channel at the end of each communication, and at least every 10 minutes during a communication" | the identification job (section 5) |
| 97.119(b)(1) | "By a CW emission. When keyed by an automatic device used only for identification, the speed must not exceed 20 words per minute" | Morse by OOK, 20 WPM |
| 97.3(c)(8), 97.311 | SS: "bandwidth-expansion modulation emissions"; 97.311 (a)-(c): FCC-regulated areas, no obscuring, no harmful interference, Regional Director may require records | section 6 |
| 97.313(a), (b) | "minimum transmitter power necessary"; "not ... exceeding 1.5 kW PEP" | section 6 |

## 3. Carriers: the channel plan

Under Part 15 the radio uses the Bluetooth carriers, 2402 + 2k MHz [S Vol 6 Part B 1.4.1].
Under amateur rules every one of the 40 physical channels gets a carrier from a plan; the link
layer is unchanged (it still hops over channel indices; whitening stays per index).

* **Band edges.** Each carrier must keep a guard either side inside one band: 2 MHz for the
  1 Msym/s PHYs (LE 1M, LE Coded), 4 MHz when the 2M PHY is built in. These are the first
  offsets of the Bluetooth transmit spectrum mask, where the power in 1 MHz is at most -20 dBm
  [S Vol 6 Part A 3.2.2, Tables 3.3/3.4]; the 99 % occupied bandwidth of LE 1M GFSK lies well
  inside +-1 MHz [I]. So 2390-2450 allows 2392-2448 MHz, 2300-2310 allows 2302-2308 MHz. The
  actual occupied bandwidth and the power at the edge are **unmeasured** (section 8).
* **Synthesizer range.** A carrier must also lie inside the range the synthesizer was measured
  to reach (section 7): 2290-2500 MHz [F], where the datasheet specifies 2400-2483.5 MHz
  [D BGM220P datasheet Table 4.5, FRANGE]. Outside it the plan is refused, so the radio never
  transmits where its synthesizer was not seen to calibrate.
* **Default plan** (`efr32_amateur_plan_default()`): a channel whose Bluetooth carrier is valid
  keeps it (identity for 2402-2448 MHz: channels 0-21, 37, 38); every other channel (22-36, 39)
  takes the valid carriers on the 2 MHz raster that no kept channel uses, in turn: 2302, 2304,
  2306, 2308, 2392, 2394, 2396, 2398, 2400, then again. Channels may share a carrier: one link
  uses one channel at a time.
* The plan is checked once, at enable, and is fixed while amateur rules apply; `hal/radio.c`
  tunes each channel to its carrier (`efr32_radio_amateur_carrier_khz()`), and stops the radio
  should a carrier ever be out of band.

## 4. No encryption

47 CFR 97.113(a)(4) forbids messages encoded to obscure their meaning; Bluetooth link-layer
encryption (AES-CCM) does exactly that. Under amateur rules:

* **Link layer** (Zephyr, `BT_CTLR_LE_ENC_POLICY`, selected by the module): the controller asks
  `ll_enc_policy_permitted()`. A peer's `LL_ENC_REQ` is answered, with no `LL_ENC_RSP`, by
  `LL_REJECT_EXT_IND` (or `LL_REJECT_IND`) with Unsupported Remote Feature (0x1A), as the
  specification requires of a link layer without encryption [S Vol 6 Part B 5.1.3.1]; the host
  is not asked for a key and data resumes unencrypted. The local host's LE Enable Encryption
  and LE Long Term Key Request Reply fail with Encryption Mode Not Acceptable (0x25). Each
  refusal is reported on the console with the rule ("encryption refused, 47 CFR
  97.113(a)(4)").
* **Radio** (`hal/radio.c`): both AES-CCM paths (the LLL's and the staged replies') check the
  same policy; reaching them under amateur rules is a defect, and the radio is stopped (safe
  shutdown, reported as a broken rule).
* Pairing itself (SMP over L2CAP) is host traffic in the clear; an application under amateur
  rules should not pair. The controller still advertises the LE Encryption feature bit (a
  compile-time feature set); peers learn of the refusal from the reject.

## 5. Station identification: Morse code by on-off keying

**Why Morse.** 97.119(b) allows CW, phone, RTTY (with a specified digital code, when the
communication is RTTY or data) or image identification. A Bluetooth advertising PDU carrying the
call sign in ASCII is a data emission (F1D), not an RTTY one (third symbol B), so the firmware
does not rely on it: it sends the call sign in International Morse code [ITU-R M.1677-1] by
on-off keying the carrier, a CW emission, which 97.305(a) allows on any amateur frequency.
Applications may also put the call sign in their advertising data (the QEMU test does, as the
Complete Local Name); that is informative, not the identification.

**How.** The EFR32xG22 modem transmits "ASK / OOK" [D RM 5.1.2]; `MODEM.CTRL0.MODFORMAT` selects
it (bits 8:6, public register model `MODEM_S_field.py`; OOK/ASK = 6 in the configurator's
`calc_demodulator.py`). The Morse key pattern is sent as one fixed-length OOK frame:

* 20 WPM, PARIS timing: one unit 1.2 s / 20 = 60 ms (dash 3 units, gaps 1/3/7) [ITU-R M.1677-1].
* Symbols at 200 Bd (`MODEM.TXBR` = 24000/1: f_HFXO / (8 x 24000) [D configurator
  `calc_txbr_value`]), 12 symbols a unit, so every keying edge falls on a symbol boundary at a
  rate inside the modem's documented "below 100 Baud/s to 2 MBaud/s". Keying per symbol at the
  Morse unit itself (16.7 Bd) would need TXBRNUM above its 16-bit field.
* No preamble (`PRE.TXBASES` 0), an all-zero sync word (32 symbols, key up), no CRC, no
  whitening, fixed length (`FRC.DFLCTRL.DFLMODE` 0, `WCNTCMP0`), least significant bit first, 1 =
  key down [I]. `MODEM.MODINDEX` as the configurator sets it for OOK (20 x 2^-7, replayed in
  `tests/efr32-radio-contract`) [I].
* KD3APL is 69 units: 104 octets, 4.14 s keyed plus 160 ms of sync, 4.32 s in all.
* Emission: A1A (amplitude, one channel without a sub-carrier, telegraphy for aural reception).
  Necessary bandwidth by ITU-R RR Appendix 1, B_n = B x K = 16.7 Bd x 5 = 83 Hz: 83H3A1A. The
  keying shape (the 1M table's shaping filter at 200 Bd, and the PA ramp at the frame's ends) is
  **unmeasured**; key clicks may make the real bandwidth wider (section 8).
* The carrier is the station's last transmitting carrier ("on its transmitting channel",
  97.119(a)); for a hopping link that is one of its channels.

**When (97.119(a)).** `hal/amateur_rules.c` keeps the schedule:

* A communication starts with the first transmission after the last identification (or after
  32 s of silence) and ends 32 s after its last transmission: the largest connection supervision
  timeout [S Vol 6 Part B 4.5.2] (legacy advertising repeats at most every 10.24 s), so no
  Bluetooth exchange is silent that long and continues. `efr32_amateur_end_of_communication()`
  and `efr32_amateur_disable()` end it at once.
* The identification is due at the end of each communication, and during one so that it **ends
  at most 10 minutes** after the previous one ended: its latest start is the previous end + 600 s
  minus its own airtime. Nothing is sent if nothing was transmitted since the last one.

**Scheduling.** The identification is a job of the deadline scheduler (`k_deadline_event`,
docs/radio-tables.md 7.76-7.82), with its own budget under admission control
(`EFR32_RADIO_WCET_AMATEUR_ID_NS`, [P] 200 us, unmeasured). Its release is
`EFR32_RADIO_AMATEUR_ID_LEAD_S` (30 s) before the latest start, or the end of the
communication; earlier is allowed. Its run asks the controller's ticker for a radio slot as long
as the identification (two user-extension ticker nodes, `BT_CTLR_USER_EXT`, the way Zephyr's
flash driver borrows the radio): the ticker places it where no other event holds the radio,
aborts what holds the radio when it starts, and the identification starts in the LLL context.
Its CPU work is short; the 4.3 s on air are radio time reserved by the ticker, not CPU time. A
timer watches the latest start: **if the identification has not started by then, the station
would transmit unidentified, and the radio is stopped** (the module's safe shutdown: PA off, RAC
forced off, M0+ reset, controller stopped, reported as a late start of
`efr32_amateur_id_event`). The slot request stays in place until the frame has ended (stopping
a ticker node releases its reservation: in a first version the next advertising event took the
radio 77 ms into the frame, found by the 10-minute QEMU run), and a second timer checks that the
frame ends inside its slot; a cut frame counts as not sent and is retried in a new slot while its
latest start allows, else the radio is stopped. `efr32_amateur_disable()` returns to Part 15
inside the slot of the closing identification, so nothing is sent under amateur rules after it.

**Cost.** The radio is busy for the identification's 4.3 s: a connection loses its events
meanwhile and survives only with a supervision timeout above that. The controller does not yet
refuse shorter timeouts under amateur rules.

## 6. Spread spectrum, power

Bluetooth LE hops: advertising over three channels, connections over up to 37 by a published
channel-selection algorithm [S Vol 6 Part B 4.5.8]. Each packet is a GFSK data emission (F1D) on
one carrier. Whether the hopping makes the station's emission "SS" in the sense of 97.3(c)(8)
(designators with X second and third symbols, "bandwidth-expansion modulation") is a question
for the licensee; both readings are authorized in 13 cm (97.305(c) lists data and SS). If it is
SS, 97.311 applies: communications only within FCC-regulated areas or with countries that permit
SS (a): the operator's; not to obscure meaning (a): no encryption, and the hop sequence and
framing are publicly documented [S]; no harmful interference to other emissions (b): the
operator's; on a Regional Director's request, stop, restrict, or keep records (c): the
operator's. The current 97.311 has no power limit (eCFR, amended 80 FR 53753, 2015).

Power: 97.313(b) caps a station at 1.5 kW PEP (61.8 dBm); the module's maximum output is 8.2 dBm
typical [D BGM220P datasheet Table 4.5, POUTMAX], so the cap cannot be reached, and
`efr32_amateur_enable()` checks the radio's maximum against the table's ceiling. The radio
driver has one PA setting (the stage-2 preset, dBm not characterized); 97.313(a)'s "minimum
transmitter power necessary" is the operator's to judge, and a lower setting would first need
the PA characterized (section 8).

## 7. Hardware: what the synthesizer reaches

The datasheet specifies the 2.4 GHz radio for 2400-2483.5 MHz [D BGM220P Tables 4.5/4.6] and the
module's matching network and antenna are tuned there; the reference manual describes the
synthesizer only as covering "the supported frequency bands" [D RM 5.1.1]. Whether the VCO
calibrates and the PLL locks at 2300-2310 and 2390-2400 MHz is a measurement:

* **Receive-only probe** (`tests/amateur-port`, `prj-probe.conf`,
  `EFR32_RADIO_AMATEUR_SYNTH_PROBE`): a passive scanner whose advertising channels are moved,
  three at a time, across 2290-2500 MHz in 5 MHz steps, sampling while the radio is in RXSEARCH:
  the M0+'s successive-approximation VCO calibration result (`SYNTH.VCOTUNING`, 11 bits under
  VCAPSEL 16) and `SYNTH.STATUS.CAPCALERROR`. `STATUS.INLOCK` is not reported while the IF path
  is enabled (receive) on this chip (docs/radio-tables.md 7.64, 7.72). A calibration that
  saturates (0 or 0x7FF) means the VCO cannot reach the carrier. The probe transmits nothing
  (any transmission stops the radio).
* **Transmit check** (`prj-txcheck.conf`): 2 s of advertising on one carrier, then the closing
  Morse identification, sampling `SYNTH.STATUS.INLOCK` and the calibration in the TX state.

Results [F] (docs/radio-tables.md 7.85):

* The VCO calibration converged at every probed carrier, 2290-2500 MHz, with its code
  1418 (2290 MHz) to 424 (2500 MHz) of 0-2047, never at an end, and no `CAPCALERROR`.
* Transmitting (2 s of advertising with the call sign in the advertising data, then the closing
  Morse identification) at 2304, 2395 and 2445 MHz: `INLOCK` in all 8328 TX samples at each
  carrier, 4.16 s in TX, matching the 4.32 s OOK frame; identification, ticker slot and return to
  Part 15 as in QEMU; on-target checks 27/27 on silicon.
* Default plan on silicon: the laptop's Bluetooth scan reported the kit as "KD3APL" (2402 and
  2426 MHz are standard carriers).
* Periodic identification on silicon: 660 s of advertising on 2445 MHz; identification at
  570.0 s (QEMU 570.2 s) and the closing one at 664.4 s; lock throughout; no shutdown.
* So the firmware's range is the probed one, 2290-2500 MHz; the 13 cm segments 2302-2308 and
  2392-2448 MHz (with the 2 MHz guard) are all reachable. Every transmission made outside the
  plain Bluetooth plan for this is in the main checkout's `scratch/amateur/tx-log.txt`.

## 8. Not verified, and what to measure

Unverifiable here without RF test equipment, so **not claimed**:

* output power versus carrier, in particular below 2400 MHz where the matching network is
  off-design; harmonics and spurious emissions (97.307(c)) anywhere;
* occupied bandwidth of the LE 1M emission and the power outside the band edges with the 2 MHz
  guard;
* the OOK waveform: that the carrier is keyed at all (only the TX state, the frame's duration and
  the synthesizer lock were observed), key-down power, on/off ratio, keying edges and clicks;
* receive sensitivity off the Bluetooth channels.

With an SDR (sample rate >= 4 MS/s, centred on the carrier, a fixed gain, the kit at a fixed
distance; the same capture on a Bluetooth channel as the reference):

1. CW identification at each test carrier (2304, 2395, 2445 MHz; build `prj-txcheck.conf` with
   `CONFIG_AMATEUR_PORT_SINGLE_KHZ`): IQ capture across the whole frame; check the Morse decodes
   KD3APL at 20 WPM, the key-down level, the on/off ratio, the rise/fall time and the spectrum
   around the carrier (key clicks, +-10 kHz span, 10 Hz RBW).
2. Advertising at the same carriers: spectrum +-5 MHz, RBW 100 kHz, peak hold over a few
   seconds; read the 99 % occupied bandwidth and the level at the band edge (2390, 2450, 2300,
   2310 MHz) relative to the 2402 MHz reference.
3. Relative output power versus carrier from 2300 to 2450 MHz in 5 MHz steps (the same capture
   each), to see the matching network's roll-off.
4. If the SDR reaches it: the second harmonic (4.6-4.9 GHz) relative to the carrier.

## 9. Tests

* Host (`tests/efr32-radio-contract/amateur_rules_test.c`): call signs, bands and guards, plan
  validation and the default plan, Morse (KD3APL's 69 units against ITU-R M.1677-1, word
  spacing), the OOK bit packing, the identification schedule, and the configurator's OOK
  MODINDEX replay.
* Zephyr controller unit test (`vendor/zephyr/tests/bluetooth/controller/ctrl_encrypt`, variant
  `policy`): a refused `LL_ENC_REQ` gets `LL_REJECT_EXT_IND` 0x1A with no `LL_ENC_RSP`, no key
  material drawn and no host request; with the policy permitting the procedure is unchanged.
  20/20 (policy) and 19/20 + 1 skipped (without), built for 32-bit ARM and run under qemu-arm.
* QEMU (`vendor/qemu`): the medium matches carriers on a 1 MHz raster; a carrier outside the
  synthesizer range never locks and reaches nobody; an OOK frame is not a Bluetooth packet and
  lasts its symbols at the TXBR rate (qtests 101/101 and 35/35).
* On target in QEMU (`tests/amateur-port`, `amateur-selftest`, 27 checks): Part 15 until
  switched; call signs refused (NULL, empty, foreign, malformed); the standard plan refused at
  channel 22 (2450 MHz); 2350 MHz and 2449 MHz refused; the switch; `-EALREADY`; encryption
  refused under amateur rules and permitted after; disable without an identification when
  nothing was sent.
* QEMU dual machine (`tests/amateur-port/run-qemu-amateur-e2e.sh`): advertiser and scanner on an
  odd-MHz plan (2401-2447 MHz; no Bluetooth carrier), call sign in the advertising data: the
  scanner receives KD3APL (75 times in 40 s), every advertiser carrier is a plan carrier, and
  after 8 s the advertiser stops and returns to Part 15 with one closing identification, decoded
  from the OOK frame as KD3APL at 200 Bd, 4.32 s.
* QEMU, periodic (`tests/amateur-port/run-qemu-amateur-periodic.sh`): an advertiser that never
  stops identifies every 574.3 s (latest start 600 s - 4.32 s airtime, released 30 s early),
  each frame on air for its whole 4.32 s; 6 identifications in 62 virtual minutes, no
  shutdown.
* Product: `owo_suit` and the `-DOWO_HW=ON` RAM probe build byte-identical images before and
  after; `scripts/check-hardware-ram.sh` 30916 B <= 31000 B.
