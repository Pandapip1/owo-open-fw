/*
 * AES-128 ECB for the link layer (Zephyr's hal/ecb.h contract) through the PSA Crypto API, which is
 * what Zephyr's own host uses for SMP (subsys/bluetooth/crypto/bt_crypto_psa.c). The key is imported
 * per call: simple, not fast. CRYPTOACC could replace it later; this needs no hardware knowledge.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <psa/crypto.h>

#include "hal/ecb.h"
#include "hal/debug.h"

void ecb_encrypt_be(uint8_t const *const key_be, uint8_t const *const clear_text_be,
		    uint8_t *const cipher_text_be)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id;
	size_t len;

	LL_ASSERT(psa_crypto_init() == PSA_SUCCESS);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 128);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
	psa_set_key_algorithm(&attr, PSA_ALG_ECB_NO_PADDING);
	LL_ASSERT(psa_import_key(&attr, key_be, 16, &id) == PSA_SUCCESS);
	LL_ASSERT(psa_cipher_encrypt(id, PSA_ALG_ECB_NO_PADDING, clear_text_be, 16, cipher_text_be, 16,
				     &len) == PSA_SUCCESS);
	LL_ASSERT(len == 16);
	(void)psa_destroy_key(id);
}

void ecb_encrypt(uint8_t const *const key_le, uint8_t const *const clear_text_le,
		 uint8_t *const cipher_text_le, uint8_t *const cipher_text_be)
{
	uint8_t key[16];
	uint8_t clear[16];
	uint8_t out[16];

	sys_memcpy_swap(key, key_le, sizeof(key));
	sys_memcpy_swap(clear, clear_text_le, sizeof(clear));
	ecb_encrypt_be(key, clear, out);

	if (cipher_text_le) {
		sys_memcpy_swap(cipher_text_le, out, sizeof(out));
	}
	if (cipher_text_be) {
		memcpy(cipher_text_be, out, sizeof(out));
	}
}

void ecb_encrypt_nonblocking(struct ecb *e)
{
	uint8_t *key = e->in_key_le ? (sys_memcpy_swap(e->in_key_be, e->in_key_le, 16), e->in_key_be)
				     : e->in_key_be;
	uint8_t *text = e->in_clear_text_le
				? (sys_memcpy_swap(e->in_clear_text_be, e->in_clear_text_le, 16),
				   e->in_clear_text_be)
				: e->in_clear_text_be;

	ecb_encrypt_be(key, text, e->out_cipher_text_be);
	/* Completes synchronously. */
	e->fp_ecb(0, e->out_cipher_text_be, e->context);
}

int ecb_ut(void)
{
	return 0;
}
