/*
 * Multi-part operation context of the RADIOAES PSA driver (crypto/radioaes_psa.c), a member of
 * TF-PSA-Crypto's psa_driver_mac_context_t union (the dispatch's
 * crypto_driver_contexts_composites.h that crypto/CMakeLists.txt derives).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIOAES_PSA_CONTEXTS_H
#define EFR32_RADIOAES_PSA_CONTEXTS_H

#include <stdint.h>

/* AES-CMAC (NIST SP 800-38B 6.2) computed block by block over AES-ECB on RADIOAES. */
typedef struct {
	uint8_t key[32];   /* AES-128 or AES-256 key */
	uint8_t chain[16]; /* C_i, the chaining value after the blocks processed so far */
	uint8_t block[16]; /* message bytes not yet processed: the last block is held back */
	uint8_t key_len;
	uint8_t block_len;
} efr32_radioaes_mac_operation_t;

#endif /* EFR32_RADIOAES_PSA_CONTEXTS_H */
