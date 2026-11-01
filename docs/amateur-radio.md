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

Identification method (section 5):

```
# default: Morse code (CW) only
CONFIG_EFR32_RADIO_AMATEUR_ID_DATA=y      # also send BLE-ASI data identification frames
CONFIG_EFR32_RADIO_AMATEUR_ID_CW=n        # with ID_DATA: data identification only
CONFIG_EFR32_RADIO_AMATEUR_ID_DATA_ADDRESS_STATION=y  # default: an address of its own
```

| `ID_DATA` | `ID_CW` | each identification sends |
|---|---|---|
| n (default) | y (forced) | Morse code |
| y | y (default) | the BLE-ASI frames, then Morse code, in one slot |
| y | n | the BLE-ASI frames only (**legal basis unconfirmed**, section 5.2) |

With the data identification, `efr32_amateur_enable()` also refuses `-EADDRNOTAVAIL` when the
frames are to come from the station's address and the controller has none.

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
| 97.119(b)(3) [R*] | "By a RTTY emission using a specified digital code when all or part of the communications are transmitted by a RTTY or data emission" | the optional data identification, **basis unconfirmed** (section 5.2) |
| 97.309(a)(3), (4) [R*] | specified codes include "the 7-unit, International Alphabet No. 5, code ... (commonly known as 'ASCII')"; a station using a specified code "may use any technique whose technical characteristics have been documented publicly" | BLE-ASI carries the call sign in ASCII inside publicly specified Bluetooth framing |

[R*]: from the rule text as summarised in section 5's first version and recalled, not re-fetched:
the eCFR was not reachable from the machine that wrote section 5.2. Check the exact wording
against the eCFR before relying on it.
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

## 5. Station identification

### 5.1 Morse code by on-off keying (the default)

**Why Morse.** 97.119(b) allows CW, phone, RTTY (with a specified digital code, when the
communication is RTTY or data) or image identification. A Bluetooth advertising PDU carrying the
call sign in ASCII is a data emission (F1D), not an RTTY one (third symbol B), so by default the
firmware does not rely on it: it sends the call sign in International Morse code [ITU-R
M.1677-1] by on-off keying the carrier, a CW emission, which 97.305(a) allows on any amateur
frequency. The data identification of section 5.2 is a separate, opt-in flag. Applications may
also put the call sign in their own advertising data; that is informative, not the
identification.

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

### 5.2 Identification by data emission (optional, BLE-ASI)

`CONFIG_EFR32_RADIO_AMATEUR_ID_DATA` (default n). **Legal caveat, not confirmed:** 97.119(b)(3)
allows identification "by a RTTY emission using a specified digital code when all or part of the
communications are transmitted by a RTTY or data emission" [R*], and 97.309(a)(3) specifies ASCII
[R*]. The frame below is a data emission (F1D, GFSK, about 1 MHz wide), not a narrow-band RTTY
emission (third symbol B), and the ASCII octets travel inside Bluetooth framing (preamble, access
address, header, 128-bit UUID, CRC, whitening), all publicly specified [S] (cf. 97.309(a)(4)).
Whether that is identification under 97.119(b)(3) is open; the licensee is confirming it with
the ARRL. Until then CW stays the default, and with `ID_DATA` the CW identification stays on
unless `CONFIG_EFR32_RADIO_AMATEUR_ID_CW=n` is chosen explicitly.

**Format.** BLE Amateur Station Identification v1 (`docs/ble-amateur-station-id.md`, the
user's specification; draft): a dedicated `ADV_NONCONN_IND` (non-connectable, non-scannable;
Core v6.1 Vol 6 Part B 2.3.1.3) whose AdvData is only one Service Data - 128-bit UUID AD
structure (type 0x21): the UUID 47cf0097-0119-4de0-b1e0-ca1151600073 little-endian, a header
octet (version 1 in bits 7-4, FINAL in bit 0), the call sign's ASCII octets (A-Z, 0-9, '/'; 1-12,
no terminator). KD3APL: AdvData 25 octets
(`18 21 73 00 60 51 11 CA E0 B1 E0 4D 19 01 97 00 CF 47 10 4B 44 33 41 50 4C`, BLE-ASI 9), PDU 33
octets, 328 us on LE 1M per channel. The PDU is never embedded in the application's advertising
and the application's advertising set and data are untouched (BLE-ASI 6). FINAL (header 0x11) is
set on the end-of-communication identification: the closing one (`efr32_amateur_disable()`), one
asked for by `efr32_amateur_end_of_communication()`, and one released by the communication's
32 s idle end; 0x10 on the periodic ones.

**Address** (`EFR32_RADIO_AMATEUR_ID_DATA_ADDRESS`): by default a static random address of its
own, drawn from the entropy source at each `efr32_amateur_enable()` (Core v6.1 Vol 6 Part B
1.3.2.1: two top bits 1, the rest random, not all 0 or all 1), so receivers that cache names by
address do not mix the identification with the application's advertising; or the station's
address (the controller's public address, else its random identity address).

**Where.** One PDU on each of the three advertising channels, on the active plan's carriers for
them (channels 37, 38, 39 with their own whitening), in that order. The application's own
advertising events are not touched; under a connection the frames go between connection events.

**Schedule and scheduling.** Exactly those of 5.1: the same job, the same release (30 s lead,
or the end of the communication), the same latest start (the previous identification's end +
600 s - the identification's span), the same ticker slot request and watchdog (not started by its
latest start: safe shutdown), the same frame timer (cut: retried while the latest start allows,
else safe shutdown), the same closing behaviour. The job's runs are admitted with
`EFR32_RADIO_WCET_AMATEUR_ID_NS` (measured, 7.85); the frames themselves:

* the first PDU is set up and launched in the LLL context of the slot's start (as `lll_adv.c`'s
  prepare: reset, PHY, length, access address, CRC, channel, whitening, TXEN), under the LLL
  context's budget;
* each next PDU (and, with `ID_CW`, the Morse frame) is launched from the previous PDU's radio
  completion (as `lll_adv.c`'s `isr_done()`), whose start-by deadline is the advertising PDU
  spacing, 10 ms after the previous TXEN [S Vol 6 Part B 4.4.2.1] and whose budget is a
  transmit's measured completion plus the identification's measured callback
  (`EFR32_RADIO_WCET_AMATEUR_ID_COMPLETION_NS`);
* so the identification's span, from its first TXEN to the end of its last frame, is at most
  (parts - 1) x 10 ms + TX ready + the last part's airtime (`efr32_amateur_id_span_us()`): data
  only 2 x 10 ms + 0.09 ms + 0.33 ms = 20.42 ms, data and CW 3 x 10 ms + 0.09 ms + 4.32 s =
  4.35 s. The ticker slot reserves that span plus the controller's event overheads; the latest
  start subtracts it. Measured spans are far below (7.85): 1.71 ms and 4.33 s.

The identification's own transmissions do not count as the communication's (the radio's TX
notification ignores them).

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
* Data identification (5.2) on silicon, in-band standard carriers only (channels 37 and 38 on
  2402 and 2426 MHz, channel 39 and channels 22-36 moved to 2402 MHz): 12 identifications per
  run, every 5 s, all completed, no shutdown. Each PDU went out whole (FRC word count 33 of 33 at
  each completion). The laptop's passive BlueZ scan received frames from the identification's
  own address in four of eight runs (none in the others, unexplained) and decoded the BLE-ASI
  structure (service data under the BLE-ASI UUID: `11 4B 44 33 41 50 4C`, FINAL and KD3APL).
  Run figures in 7.85.
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
* receive sensitivity off the Bluetooth channels;
* how often a stock receiver picks up the data identification: BlueZ reports a device once, so
  the laptop's scan shows that frames were decoded, not how many (7.85).

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

Data identification (5.2):

* Host: the BLE-ASI structure for KD3APL equals the specification's section 9 example octet for
  octet (25 octets), FINAL is header 0x11 and nothing else changes; 19 + N octets, 31 for a
  12-character call sign; call signs outside A-Z, 0-9, '/' or 1-12 octets refused; the
  `ADV_NONCONN_IND` (header 0x42 with TxAdd, length 31, AdvA, the structure; 328 us on LE 1M); a
  receiver finds it by type and UUID (also behind another AD structure) and refuses another
  version, another UUID, a bad character, a truncated structure or another PDU type; the static
  random address rule; the span of every method combination (CW, data, both; none or unknown
  refused; a part that does not fit its spacing refused).
* QEMU dual machine (`run-qemu-amateur-data-e2e.sh`, `prj-dataid.conf`: data only, the
  application advertising "owo-app"), odd-MHz plan:
  * `MODE=short`: the receiver (its own BLE-ASI parser, written from the specification) gets
    KD3APL from an `ADV_NONCONN_IND` with FINAL on the closing identification; 76 application
    frames carry only "owo-app" and never the call sign; the identification's address differs
    from the application's; three identification PDUs (33 octets, header 0x42) for one
    identification; plan carriers only; no CW frame; Part 15 after it. With
    `EXPECT_CW=1` and `ID_CW=y`: the same plus one Morse frame per identification.
  * `MODE=connection` (`prj-peripheral.conf`, `prj-central.conf`, the hardware configuration
    with `EFR32_RADIO_TEST_QEMU_COUNT_LL_MISSES`): a Peripheral and a Central, both under amateur
    rules, connected for 29 virtual minutes: the Peripheral identified at 570.3, 1140.3 and
    1710.2 s, the Central at 570.1, 1140.1 and 1710.1 s, all during the connection; the Central,
    scanning between its connection events, received the Peripheral's BLE-ASI frame each time
    (FINAL clear); the connection held; 0 scheduler misses counted.
* QEMU single machine (`run-qemu-amateur-periodic.sh` with the data-only image, never stops): 10
  identifications in 95 virtual minutes, every 569.98 s (latest start 600 s - 20.42 ms span,
  released 30 s early), no shutdown.
* CW-only regression: `run-qemu-amateur-e2e.sh` unchanged in outcome (KD3APL decoded from the OOK
  frame, 4.32 s); the on-target checks 27/27 with each method.
* Product: `owo_suit` and the product's `bg22_ek4108a` image byte-identical before and after
  (the `-DOWO_HW=ON` image was not built for this change).
