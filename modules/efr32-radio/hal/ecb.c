/*
 * AES-128 ECB for the link layer (Zephyr's hal/ecb.h contract) on the RADIOAES accelerator
 * (crypto/radioaes.c), called directly: the controller's ECB (session key derivation in
 * ull_llcp_enc.c, bt_encrypt_le() through controller/crypto/crypto.c) takes no PSA key slot.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <zephyr/sys/byteorder.h>

#include "hal/ecb.h"
#include "hal/debug.h"
#include "hal/efr32/radioaes.h"

#define ECB_KEY_SIZE 16U

void ecb_encrypt_be(uint8_t const *const key_be, uint8_t const *const clear_text_be,
		    uint8_t *const cipher_text_be)
{
	int err = efr32_radioaes_ecb(true, key_be, ECB_KEY_SIZE, clear_text_be, cipher_text_be,
				     EFR32_RADIOAES_BLOCK_SIZE);

	LL_ASSERT(err == 0);
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
