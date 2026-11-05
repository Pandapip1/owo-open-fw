# Pairing and link encryption: AES on the BG22's RADIOAES accelerator

Tags: **[F]** measured on silicon (BGM220 explorer kit, BGM220PC22HNA, HCLK 76.8 MHz),
**[Q]** measured in QEMU (`build/qemu`, this tree's `vendor/qemu`), **[S]** Bluetooth Core
Specification v6.1, **[D]** public documentation, **[Z]** Silicon Labs source under the Zlib
licence in `vendor/hal_silabs`.

## 1. Why

The product requires LE Secure Connections pairing with an authenticated (MITM) passkey and an
encrypted link (`prj.conf`: `BT_SMP_SC_PAIR_ONLY`, `BT_SMP_ENFORCE_MITM`, `BT_APP_PASSKEY`, every
characteristic `*_AUTHEN`). Before this change it could not work: PSA Crypto's software AES
allocates its cipher context (ECB and CCM 280 B, AES-CMAC 316 B in two blocks [Q]) and the image
has no heap, so ECB and CMAC failed with `PSA_ERROR_INSUFFICIENT_MEMORY` (the controller's
`ecb_encrypt()` then hit its `LL_ASSERT`), and CCM was not even built (`PSA_ERROR_NOT_SUPPORTED`).

Decision (user): use the hardware accelerator, "if only to save on battery life". Every AES the
Bluetooth stack needs now runs on RADIOAES; P-256 stays in software (section 3).

## 2. What is publicly documented, and under which licence

| block | what | register-level sources | used |
|---|---|---|---|
| RADIOAES (0x44000000, NS 0x54000000) | BA411E AES engine behind a scatter-gather DMA: ECB, CBC, CTR, CCM, CMAC; 128/256-bit keys | DMA registers: RM rev 1.2 section 11.4 (documented there for CRYPTOACC, same layout) [D] and `efr32bg22_aes.h` [Z]; clocks: RM 8.5 `CMU_CLKEN0.RADIOAES`, 8.5.25 `CMU_RADIOCLKCTRL.EN` [D]; descriptor, tag and BA411E configuration format and the ECB/CMAC/CCM descriptor sequences: `sl_protocol_crypto/src/sli_protocol_crypto_radioaes.c`, `sli_radioaes_management.c` [Z] | yes |
| CRYPTOACC symmetric (0x4C020000) | BA411E + BA413 hash behind the same DMA | registers as above [D]; descriptor and engine usage only in Silicon Labs' `sx_*` library ("All Rights reserved", Silex Insight; not in hal_silabs) | no: RADIOAES has the open descriptor-level source |
| CRYPTOACC PKE (BA414EP) | ECC over GF(p), P-256 | RM 11.3 documents `CRYPTOACC_POINTER/COMMAND/PKCTRL/PKSTATUS` but not the operation codes ("See BA414EP documentation"), the data-memory layout or the curve parameters' loading; only the closed `sx_ecc_*` library drives it | no: P-256 runs in software (p256-m) |
| CRYPTOACC TRNG (BA431) | entropy | RM 11.5, used by Zephyr's `entropy_gecko_trng` [D] | yes, already (PSA external RNG) |

Nothing in this change is derived from a proprietary binary; the closed headers were only read to
confirm their licence (they are kept, as before, under `owo-bg22-proprietary-baselines`).

## 3. Design

`modules/efr32-radio/crypto/radioaes.c` (`include/hal/efr32/radioaes.h`): synchronous AES-ECB
(n blocks), AES-CMAC and AES-CCM (13-octet nonce, 4 to 16-octet tag, 1 to 0xFFFF octets of
payload). Every register, bit, tag field and mode word carries its citation in the source.

* **Users.** The controller calls it directly: `hal/ecb.c` (session key, `bt_encrypt_le()` via
  the controller) and `hal/radio.c` (link-layer CCM). No PSA key slot, key schedule or allocation
  is on the radio's deadline path. The host reaches it through PSA Crypto: a transparent driver
  (`crypto/radioaes_psa.c`) for one-shot `psa_cipher_encrypt/decrypt` (ECB), `psa_mac_compute`
  (CMAC) and `psa_aead_encrypt/decrypt` (CCM), and multi-part CMAC (`psa_mac_sign_setup`,
  `psa_mac_update`, `psa_mac_sign_finish` and the verify forms: the GATT database hash).
  TF-PSA-Crypto's dispatch is derived at configure time from `vendor/tf-psa-crypto/dispatch` with
  exact insertions (`crypto/CMakeLists.txt`), as the LLL sources are: the driver calls, its driver
  ID and its member of the MAC operation context union; `efr32_radioaes_psa_config.h` declares
  the accelerated algorithms, so no software AES is built (flash -7.0 to -7.4 KB). An application
  sources `modules/efr32-radio/Kconfig.defaults` before `Kconfig.zephyr`, because Zephyr's own
  default for `TF_PSA_CRYPTO_DISPATCH_DIR` is read before any module's Kconfig.
* **Concurrency without a lock.** Callers run at different priorities (BT long work queue, BT RX
  thread, ULL worker, LLL-priority jobs) and preempt each other anywhere. Each call waits for the
  DMA to be idle (finishing a preempted caller's operation into that caller's buffers), saves
  FETCHADDR/PUSHADDR, runs its own chain and restores them (the save/restore is [Z]'s ISR path).
  Preemption on one core nests, so this holds at any depth; a caller waits for at most one
  lower-priority hardware run. [F]: 2 s of a cooperative thread's CCM, released every 97 us,
  against a preemptible thread's CMAC: 16 377 CCM and 57 339 CMAC, 16 380 releases while the
  CMAC was inside the driver, 0 wrong results.
* **CPU during an operation.** It polls STATUS. The hardware runs are 1.7 to 6.4 us for the
  Bluetooth sizes [F] (below); entering EM1 and taking an interrupt for that would cost more than
  it saves (an interrupt here is 10 to 40 us, docs/radio-tables.md 7.76).
* **Not implemented:** an empty CCM payload (on silicon [Z]'s all-padding block gives a tag that
  is not SP 800-38C's, and decryption of its own output fails, [F]; Bluetooth never encrypts an
  empty PDU), 192-bit keys (not in this BA411E, [Z]), multi-part PSA operations other than CMAC
  (the Bluetooth host uses no other).
* **Multi-part CMAC.** The engine's CMAC mode takes a whole message in one descriptor chain ([Z]
  saves and loads engine context only for CTR), so the driver runs SP 800-38B 6.2 itself over one-block ECB calls:
  C_i = AES_K(C_(i-1) xor M_i), the last block held back until finish, then K1 or K2 (from
  L = AES_K(0)) and the padding. Its context (key, chaining value, pending block: 66 B) joins
  PSA's MAC operation union, so a `psa_mac_operation_t` grows only where one is instantiated; the
  product (no `BT_GATT_CACHING`) links none of it (RAM and flash unchanged, `prj.conf`). One
  ECB call per 16 octets: about 6.3 us each [F, section 4], so a database hash over a few hundred
  octets costs well under 0.5 ms, once per database change.
* **Link layer.** Two OpenISA-LLL contract points that the old software CCM never exercised
  (pairing did not work before): `lll_conn.c` reads a received PDU's SN/NESN before it calls
  `radio_ccm_is_done()`, and only for non-empty PDUs, so `hal/radio.c` now decrypts (or, for an
  empty PDU, copies) in the completion job before the LLL callback; and `radio_ccm_is_available()`
  means "the CCM unit can encrypt now" (the RV32M1's single CAU), not "an empty PDU was received":
  RADIOAES always can, and the old answer stalled `LL_START_ENC_RSP` forever. A MIC failure now
  completes as a MIC failure (the LLL asserts `done`).

## 4. Measurements

Known-answer tests (`tests/crypto-port`): AES-128 (Vol 3 Part H D.1 subkey block, D.7 `ah`, Vol 6
Part C 1.1 `HCI_LE_Encrypt`), AES-CMAC (D.1 RFC 4493 examples 1-4, D.2 f4, D.3 f5 MacKey and LTK,
D.4 f6, D.5 g2, D.6 h6), AES-CCM (Vol 6 Part C 1 and 1.2: `LL_START_ENC_RSP` both directions and
`LL_DATA` both directions, encrypt, decrypt, corrupted MIC, wrong direction bit), each directly and
through PSA, plus round trips (251 octets in place, 20 octets of AAD with a 16-octet tag) and
unaligned buffers ending at a buffer's end. AES-256 CMAC (NIST SP 800-38B D.3, examples 1-4)
directly and multi-part. Multi-part CMAC through PSA: D.1 with AES-128 and D.3 with AES-256, each message
fed in pieces of 1, 7, 15, 16, 17, 33 and 80 octets, verify with the right and a corrupted MAC;
D.2-D.6 in 5-octet pieces, full and truncated to 8 octets; abort and reuse of an operation.
Silicon: 330/330 pass [F] with RADIOAES and 330/330 with `prj-software.conf` (docs/radio-tables.md
7.89; multi-part CMAC through PSA, import to destroy: 65 octets in 3 updates 124.5 us, 256 octets in
16-octet updates 220.0 us, software 421 and 804 us); QEMU 330/330 [Q] with RADIOAES and 330/330 with `prj-software.conf`
(TF-PSA-Crypto's software CMAC). The GATT database hash of `tests/gatt-peripheral-port` (built
with `-DCONFIG_BT_GATT_CACHING=y -DCONFIG_PSA_CRYPTO=y`) is the same with the driver and with
software PSA [Q]: `58bc5cbe d75434ed 2837f9b0 f00edde5`.

Times at HCLK 76.8 MHz (DWT, longest of 64, interrupts locked for the direct calls) [F]:

| operation | RADIOAES hardware run | direct call | through PSA (import, op, destroy) | software PSA (TF-PSA-Crypto) |
|---|---|---|---|---|
| AES-128 ECB, 1 block | 133 cycles (1.73 us) | 484 (6.3 us) | 3 411 (44.4 us) | 18 235 (237 us) |
| AES-CMAC 16 octets | 192 (2.5 us) | 794 (10.3 us) | - | - |
| AES-CMAC 65 octets (f6) | 412 (5.37 us) | 858 (11.2 us) | 4 198 (54.7 us) | 34 485 (449 us) |
| CCM encrypt, 1 octet | 385 (5.0 us) | 1 185 (15.4 us) | - | - |
| CCM encrypt / decrypt, 27 octets | 495 / 496 (6.4 us) | 1 171 / 1 205 (15.2 / 15.7 us) | 4 235 (55.1 us) | 34 745 (452 us) |
| CCM encrypt / decrypt, 251 octets | 2 063 / 2 062 (26.9 us) | 2 740 / 2 771 (35.7 / 36.1 us) | - | - |
| P-256 key generation + export (p256-m, software) | - | - | 27 060 470 (352 ms) | same |
| P-256 ECDH (p256-m, software) | - | - | 13 418 834 (175 ms) | same |

The hardware runs fit a start-up plus a time per AES block within 1 %: ECB and CMAC 80 + 55
cycles per block (CMAC counts its subkey block), CCM 105 + 56 (each payload block twice). The QEMU
model reproduces exactly that (`vendor/qemu/hw/arm/efr32-radioaes.c`); its idle STATUS (0) and
`INCL_IPS_HW_CFG` (0x00000001, the header says 0x11) are the values read on silicon.

Energy (datasheet Table 4.3, 3 V: EM0 at 76.8 MHz 27 to 37 uA/MHz, i.e. 2.1 to 2.8 mA; the
accelerator's own current is not specified): an encrypted 27-octet PDU costs about 15 us of CPU
on RADIOAES against about 450 us in software, so an encrypted connection event that receives and
answers with data spends about 31 us instead of about 0.9 ms of active CPU (about 85 nC instead of
2.5 uC at 2.8 mA). Pairing's AES is negligible either way; its cost is the two software P-256
operations (0.53 s, about 1.5 mC), once per pairing.

Stacks, peak high-water (`CONFIG_INIT_STACKS`, QEMU, owo_suit with `sim/product-qemu.conf`,
LE Secure Connections passkey pairing then 90 s of encrypted GATT; in brackets the same run
without pairing) [Q]: LLL 436/768 (436), completion job 488/768 (464), ULL worker 528/640 (528),
BT RX thread 728/1200 (424), BT long work queue 728/1400 (680). Per operation (thread entry
included) [F]: direct ECB 356 B, CMAC 404, CCM 540; PSA ECB 484, CMAC 540, CCM 692; software
PSA ECB 648, CMAC 712, CCM 896. The product's own figures with stimulation over the link, the
silicon ones, and the stack sizes taken from them (long work queue 1400 -> 1008):
docs/radio-tables.md 7.86.

PSA key slots: the link layer no longer takes any, which leaves the long work queue and the RX
thread; at most one live slot was measured (slot counter in tf-psa-crypto, QEMU product
pairing). `MBEDTLS_PSA_KEY_SLOT_COUNT` 4 -> 2.

## 5. Deadlines (docs/radio-tables.md 7.77, 7.79)

The connection runs and paths in Kconfig were measured on unencrypted links. With
`BT_CTLR_LE_ENC` the RX->TX completion's budget and its path to RESPOND gain a decrypt, an
encrypt and one lower-priority accelerator run (`EFR32_RADIO_CCM_DECRYPT_NS`,
`EFR32_RADIO_CCM_ENCRYPT_NS`, `EFR32_RADIO_AES_BLOCKING_NS`: silicon 15.69 + 15.25 + 5.37 us for
27-octet PDUs, 36.08 + 35.68 + 5.37 with data length extension), a Central's LLL prepare an
encrypt, and the ULL window the session-key ECB (`EFR32_RADIO_ECB_NS`, 6.30 us)
(`include/hal/efr32/ll_deadlines.h`). QEMU check [Q] (`EFR32_RADIO_TIMING_PROBE`, encrypted
GATT writes): Peripheral RX->TX run 164.3 us and path to RESPOND 159.9 us against 136.6 and 133.0
unencrypted (7.79), Central 160.5 / 146.9 against 134.6 / 121.2: +26 to +28 us, inside the
+39.7 us the QEMU figures add for full 27-octet PDUs. Software CCM would have added about 0.9 ms
to a turnaround that has about 100 us.

## 6. Verification

* QEMU, `tests/radio-port` (`prj-connection.conf` + `prj-pairing.conf`) and `tests/radio-rx-port`
  (`prj-central.conf` + `prj-central-pairing.conf`), `run-qemu-connection-e2e.sh` with `PAIRING=1`:
  LE Secure Connections passkey pairing to security level 4 in 4.75 s virtual, then 3 060/3 060
  GATT write/read-back rounds over the encrypted link in a 900 s wall run (shift 3), no
  disconnection, no MIC failure. With the final tree, 300 s wall: shift 3 1 020/1 020, shift 5
  1 110/1 110; product 810/810; product `-DOWO_HW=ON` 720/720.
* QEMU, the product (`owo_suit`, `sim/product-qemu.conf`, flash image from `sim/scripts/mkflash.py` with `sim/scripts/jump16000.S`, a test stand-in for the
  bootloader at 0) paired from the Zephyr Central (`prj-central-product.conf`: passkey 426134 for
  serial 000000000000, the product's "Test" characteristic): level 4, 360/360 rounds; the same
  with `-DOWO_HW=ON` and the suit board model (`MACHINE_OPTS=board0=owo-suit`, new `board0/1`
  properties of the dual machine): level 4, 210/210 rounds.
* Silicon: `tests/crypto-port` as above. Pairing over the real radio is not possible yet (the
  silicon receiver is not tuned, 7.77).

## 7. Open

* P-256 on the BA414EP needs its operation codes and memory map, which are not public.
* `tests/radio-port/prj-hardware.conf` still reserves a 3504-byte libc arena for PSA CMAC; it is
  no longer needed, but changing the stage-3 hardware image wants a silicon run.
* ULL worker: closed in docs/radio-tables.md 7.86 (no FP frame on the link-layer threads; the
  product sizes it from the 648 B silicon peak: 688).
* Silicon connection-event times with encryption (7.79's list) once a silicon Peripheral receives.
