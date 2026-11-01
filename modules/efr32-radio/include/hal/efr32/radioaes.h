/*
 * AES on the EFR32BG22's RADIOAES accelerator (crypto/radioaes.c): ECB, CMAC and CCM.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every call is synchronous and reentrant: a caller that preempts another caller's operation
 * waits for the hardware to finish it (bounded by the longest single operation) and restores the
 * DMA pointers it found. No key schedule, context or buffer is kept between calls; the key is
 * fetched by the accelerator's DMA from the caller's buffer.
 */
#ifndef EFR32_RADIOAES_H
#define EFR32_RADIOAES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** AES block length in bytes (FIPS-197). */
#define EFR32_RADIOAES_BLOCK_SIZE 16U

/*
 * Return values: 0 on success; -EINVAL for parameters this driver does not implement (the key
 * sizes are 128 and 256 bits: the BA411E instance has no 192-bit keys, see radioaes.c); -EBADMSG
 * when a CCM decryption's tag does not verify (the output is then zeroed); -EIO when the
 * accelerator's DMA reported a bus error.
 */

/** AES-ECB over @p len bytes (a multiple of 16). @p in and @p out may be equal. */
int efr32_radioaes_ecb(bool encrypt, const uint8_t *key, size_t key_len, const uint8_t *in,
		       uint8_t *out, size_t len);

/** AES-CMAC (NIST SP 800-38B) of @p len bytes; writes the full 16-byte MAC. */
int efr32_radioaes_cmac(const uint8_t *key, size_t key_len, const uint8_t *in, size_t len,
			uint8_t mac[EFR32_RADIOAES_BLOCK_SIZE]);

/**
 * AES-CCM (NIST SP 800-38C) with a 13-byte nonce (a 2-byte length field), a tag of 4, 6, ..., 16
 * bytes, up to 0xFEFF bytes of associated data and 1 to 0xFFFF bytes of payload (an empty payload
 * returns -EINVAL: see radioaes.c).
 *
 * Encrypt: @p in (plaintext) -> @p out (ciphertext), @p tag written.
 * Decrypt: @p in (ciphertext) -> @p out (plaintext), @p tag verified; on a mismatch @p out is
 * zeroed and -EBADMSG returned. @p in and @p out may be equal.
 */
int efr32_radioaes_ccm(bool encrypt, const uint8_t *key, size_t key_len, const uint8_t *nonce,
		       size_t nonce_len, const uint8_t *aad, size_t aad_len, const uint8_t *in,
		       uint8_t *out, size_t len, uint8_t *tag, size_t tag_len);

/** CCM nonce length this driver implements (bytes). */
#define EFR32_RADIOAES_CCM_NONCE_SIZE 13U

#ifdef __cplusplus
}
#endif

#endif /* EFR32_RADIOAES_H */
