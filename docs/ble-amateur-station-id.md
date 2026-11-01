# BLE Amateur Station Identification (BLE-ASI)

Version 1.0 (draft), 2026-11-01. Author: Gavin John, KD3APL.

## 1. Purpose

This document defines a Bluetooth Low Energy advertising data format for amateur radio station
identification. It applies to amateur stations that use Bluetooth LE PHYs and framing on amateur
frequencies.

The station's callsign is sent as plain ASCII inside a Service Data AD structure, in a dedicated
broadcast packet. Any receiver can find and read it with publicly documented tools.

This document defines a data format only. Whether a given use meets a particular administration's
identification rules (for example 47 CFR 97.119 in the United States) is for the operator to
determine. Section 7 gives notes.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are to be interpreted as described in
RFC 2119.

## 2. Service UUID

    47cf0097-0119-4de0-b1e0-ca1151600073

The UUID is a well-formed RFC 9562 version 4 UUID: the version nibble is `4` and the variant
nibble is `b`. It was chosen to be readable rather than random.

| Group | Reads as |
|---|---|
| `47cf0097` `0119` | 47 CFR 97.119, "Station identification" |
| `4de0` | version 4, then `de`: CW for "this is", as in "DE KD3APL" |
| `b1e0` | variant `b`, then `1e0`: read together as "BLE" |
| `ca11516` `00073` | "CALLSIG(N)" … "73" |

It is not derived from the Bluetooth Base UUID, so it cannot collide with a Bluetooth SIG-assigned
16-bit or 32-bit UUID.

## 3. AD structure

The identification is one AD structure of type **Service Data – 128-bit UUID (0x21)**, as defined
in the Bluetooth Core Specification Supplement (CSS), Part A, §1.11.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 1 | AD length | 18 + N (covers the type, UUID, header and callsign) |
| 1 | 1 | AD type | `0x21` |
| 2 | 16 | Service UUID | `73 00 60 51 11 CA E0 B1 E0 4D 19 01 97 00 CF 47` (the UUID in Section 2, little-endian, as all Bluetooth UUIDs are sent) |
| 18 | 1 | Header | bits 7–4: format version (`1`); bits 3–0: flags (Section 4) |
| 19 | N | Callsign | ASCII, 1 ≤ N ≤ 12 (Section 5) |

The structure is 19 + N octets. With N = 12 it is 31 octets, the whole legacy advertising data
field.

## 4. Header

- **Format version** (bits 7–4): `1` for this document. A receiver MUST ignore structures with a
  version it does not implement.
- **Flags** (bits 3–0):
  - bit 0, **FINAL**: set in the identification sent at the end of a communication; clear
    otherwise.
  - bits 3–1: reserved. Senders MUST set them to 0; receivers MUST ignore them.

For a version 1 identification the header octet is therefore `0x10` (periodic) or `0x11`
(final).

## 5. Callsign

- The callsign MUST be the station's callsign as issued, in ASCII uppercase letters `A`–`Z` and
  digits `0`–`9`. It MAY carry indicators separated by `/` (for example `KD3APL/M` or
  `W1AW/KH6`).
- The callsign has no terminator or padding: its length is the AD length minus 18.
- Senders MUST NOT encrypt, compress or otherwise encode the callsign. Bluetooth whitening and CRC
  are permitted; both are publicly specified, and whitening is removed by every receiver before
  the payload is read.

## 6. Transmission

- **Dedicated identification packet.** The station MUST send the identification as an
  `ADV_NONCONN_IND` PDU (non-connectable, non-scannable; Core Vol 6 Part B §2.3.1.3) whose AdvData
  contains only this AD structure. It goes out on each primary advertising channel of the
  frequency plan in use. The application's own advertising data, including its device name, is
  unaffected.
- **Not embedded.** The structure MUST NOT be carried in other PDUs (advertising, scan response,
  extended or periodic advertising data) as a substitute for the dedicated packet. A receiver
  therefore needs to watch only for non-connectable, non-scannable advertising.
- **Address.** The PDU's AdvA MAY be the station's usual device address, or an address used only
  for identification packets. A separate address stops receivers that cache names by address from
  mixing the identification with the application's advertising.
- **Timing** follows the operator's licence conditions. In the United States, identification
  is required at least every 10 minutes during a communication and at its end. Set the FINAL flag
  on the end-of-communication identification.

## 7. Notes for US operators (informative)

47 CFR 97.119(b)(3) allows identification by a RTTY or data emission using a specified digital
code when all or part of the communication is sent by such an emission. 47 CFR 97.309(a) lists
specified digital codes, including ASCII.

This format carries the callsign as unencrypted ASCII. The framing around it (Bluetooth LE GFSK,
whitening, CRC) is publicly documented in the Bluetooth Core Specification.

Whether that combination is acceptable identification under 97.119(b)(3) is the operator's
determination. Until it is confirmed, a station can also identify in CW (A1A) under
97.119(b)(1), at 20 words per minute or slower when keyed automatically.

## 8. Receiver behaviour

A receiver identifies a BLE-ASI structure by AD type `0x21` and the UUID in Section 2. It then
checks the format version and reads the callsign as the remaining octets. Callsign characters
outside Section 5's set SHOULD be treated as a malformed identification.

## 9. Example

Station KD3APL, periodic identification:

    AdvData (25 octets):
    18 21 73 00 60 51 11 CA E0 B1 E0 4D 19 01 97 00 CF 47 10 4B 44 33 41 50 4C
    |  |  |------------------ UUID (LE) -------------------| |  |-- "KD3APL" --|
    |  AD type: Service Data, 128-bit UUID                   header: v1, no flags
    AD length: 24

The full `ADV_NONCONN_IND` PDU is a 2-octet header, the 6-octet AdvA and these 25 octets. On the
LE 1M PHY that is 41 octets including the preamble, access address and CRC, or 328 µs per channel.

## 10. Implementations

- owo-open-fw, `modules/efr32-radio` amateur operation (Silicon Labs EFR32BG22): optional data
  identification, CONFIG_EFR32_RADIO_AMATEUR_ID_DATA. CW remains that implementation's default
  identification.
