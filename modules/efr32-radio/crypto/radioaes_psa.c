/*
 * PSA Crypto transparent driver on RADIOAES (hal/efr32/radioaes.h): one-shot AES-ECB (no
 * padding), AES-CMAC (full or truncated; one-shot and multi-part) and AES-CCM. Anything else returns
 * PSA_ERROR_NOT_SUPPORTED, which the dispatch passes on to TF-PSA-Crypto's software
 * implementation when the image builds one (efr32_radioaes_psa_config.h).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <string.h>

#include <psa/crypto.h>
#include <zephyr/sys/util.h>

#include "efr32_radioaes_psa.h"
#include "hal/efr32/radioaes.h"

static psa_status_t status_from_errno(int err)
{
	switch (err) {
	case 0:
		return PSA_SUCCESS;
	case -EINVAL:
		return PSA_ERROR_NOT_SUPPORTED;
	case -EBADMSG:
		return PSA_ERROR_INVALID_SIGNATURE;
	default:
		return PSA_ERROR_HARDWARE_FAILURE;
	}
}

static bool is_aes_key(const psa_key_attributes_t *attributes, size_t key_buffer_size)
{
	return psa_get_key_type(attributes) == PSA_KEY_TYPE_AES &&
	       PSA_BITS_TO_BYTES(psa_get_key_bits(attributes)) == key_buffer_size;
}

static psa_status_t ecb(bool encrypt, const psa_key_attributes_t *attributes,
			const uint8_t *key_buffer, size_t key_buffer_size, psa_algorithm_t alg,
			const uint8_t *input, size_t input_length, uint8_t *output,
			size_t output_size, size_t *output_length)
{
	if (alg != PSA_ALG_ECB_NO_PADDING || !is_aes_key(attributes, key_buffer_size)) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	if ((input_length % EFR32_RADIOAES_BLOCK_SIZE) != 0U) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	if (output_size < input_length) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}
	*output_length = 0U;
	if (input_length == 0U) {
		return PSA_SUCCESS;
	}

	psa_status_t status = status_from_errno(efr32_radioaes_ecb(
		encrypt, key_buffer, key_buffer_size, input, output, input_length));

	if (status == PSA_SUCCESS) {
		*output_length = input_length;
	}
	return status;
}

psa_status_t efr32_radioaes_cipher_encrypt(const psa_key_attributes_t *attributes,
					   const uint8_t *key_buffer, size_t key_buffer_size,
					   psa_algorithm_t alg, const uint8_t *iv, size_t iv_length,
					   const uint8_t *input, size_t input_length,
					   uint8_t *output, size_t output_size,
					   size_t *output_length)
{
	ARG_UNUSED(iv);

	if (iv_length != 0U) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	return ecb(true, attributes, key_buffer, key_buffer_size, alg, input, input_length,
		   output, output_size, output_length);
}

psa_status_t efr32_radioaes_cipher_decrypt(const psa_key_attributes_t *attributes,
					   const uint8_t *key_buffer, size_t key_buffer_size,
					   psa_algorithm_t alg, const uint8_t *input,
					   size_t input_length, uint8_t *output, size_t output_size,
					   size_t *output_length)
{
	return ecb(false, attributes, key_buffer, key_buffer_size, alg, input, input_length,
		   output, output_size, output_length);
}

psa_status_t efr32_radioaes_mac_compute(const psa_key_attributes_t *attributes,
					const uint8_t *key_buffer, size_t key_buffer_size,
					psa_algorithm_t alg, const uint8_t *input,
					size_t input_length, uint8_t *mac, size_t mac_size,
					size_t *mac_length)
{
	uint8_t full[EFR32_RADIOAES_BLOCK_SIZE];
	size_t length;
	psa_status_t status;

	if (PSA_ALG_FULL_LENGTH_MAC(alg) != PSA_ALG_CMAC ||
	    !is_aes_key(attributes, key_buffer_size)) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	length = PSA_MAC_LENGTH(PSA_KEY_TYPE_AES, psa_get_key_bits(attributes), alg);
	if (mac_size < length) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}

	status = status_from_errno(
		efr32_radioaes_cmac(key_buffer, key_buffer_size, input, input_length, full));
	if (status == PSA_SUCCESS) {
		memcpy(mac, full, length);
		*mac_length = length;
	}
	memset(full, 0, sizeof(full));
	return status;
}

/*
 * Multi-part AES-CMAC (psa_mac_sign_setup/update/finish, the GATT database hash). The engine's
 * CMAC mode takes the whole message in one descriptor chain, so the operation runs NIST SP
 * 800-38B 6.2 itself over one-block ECB calls: C_i = CIPH_K(C_(i-1) xor M_i), the last block
 * held back until finish, then masked with K1 or K2 (6.1) and padded.
 */
#define CMAC_RB 0x87U /* SP 800-38B 5.3, b = 128 */

static void cmac_double(uint8_t b[EFR32_RADIOAES_BLOCK_SIZE])
{
	uint8_t carry = (b[0] & 0x80U) != 0U ? CMAC_RB : 0U;

	for (size_t i = 0; i < EFR32_RADIOAES_BLOCK_SIZE - 1U; i++) {
		b[i] = (uint8_t)((b[i] << 1) | (b[i + 1U] >> 7));
	}
	b[EFR32_RADIOAES_BLOCK_SIZE - 1U] = (uint8_t)(b[EFR32_RADIOAES_BLOCK_SIZE - 1U] << 1) ^ carry;
}

static psa_status_t cmac_chain(efr32_radioaes_mac_operation_t *operation, const uint8_t *m)
{
	for (size_t i = 0; i < EFR32_RADIOAES_BLOCK_SIZE; i++) {
		operation->chain[i] ^= m[i];
	}
	return status_from_errno(efr32_radioaes_ecb(true, operation->key, operation->key_len,
						    operation->chain, operation->chain,
						    EFR32_RADIOAES_BLOCK_SIZE));
}

static psa_status_t cmac_final(efr32_radioaes_mac_operation_t *operation,
			       uint8_t mac[EFR32_RADIOAES_BLOCK_SIZE])
{
	uint8_t subkey[EFR32_RADIOAES_BLOCK_SIZE] = {0};
	psa_status_t status;

	/* L = CIPH_K(0^128); K1 for a complete last block, else K2 and 10* padding. */
	status = status_from_errno(efr32_radioaes_ecb(true, operation->key, operation->key_len,
						      subkey, subkey, sizeof(subkey)));
	if (status == PSA_SUCCESS) {
		cmac_double(subkey);
		if (operation->block_len < EFR32_RADIOAES_BLOCK_SIZE) {
			cmac_double(subkey);
			operation->block[operation->block_len] = 0x80U;
			memset(&operation->block[operation->block_len + 1U], 0,
			       EFR32_RADIOAES_BLOCK_SIZE - 1U - operation->block_len);
		}
		for (size_t i = 0; i < EFR32_RADIOAES_BLOCK_SIZE; i++) {
			operation->block[i] ^= subkey[i];
		}
		status = cmac_chain(operation, operation->block);
	}
	if (status == PSA_SUCCESS) {
		memcpy(mac, operation->chain, EFR32_RADIOAES_BLOCK_SIZE);
	}
	memset(subkey, 0, sizeof(subkey));
	return status;
}

static psa_status_t mac_setup(efr32_radioaes_mac_operation_t *operation,
			      const psa_key_attributes_t *attributes, const uint8_t *key_buffer,
			      size_t key_buffer_size, psa_algorithm_t alg)
{
	/* The engine has no 192-bit keys; one-shot CMAC declines them the same way. */
	if (PSA_ALG_FULL_LENGTH_MAC(alg) != PSA_ALG_CMAC ||
	    !is_aes_key(attributes, key_buffer_size) ||
	    (key_buffer_size != 16U && key_buffer_size != sizeof(operation->key))) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	memset(operation, 0, sizeof(*operation));
	memcpy(operation->key, key_buffer, key_buffer_size);
	operation->key_len = (uint8_t)key_buffer_size;
	return PSA_SUCCESS;
}

psa_status_t efr32_radioaes_mac_sign_setup(efr32_radioaes_mac_operation_t *operation,
					   const psa_key_attributes_t *attributes,
					   const uint8_t *key_buffer, size_t key_buffer_size,
					   psa_algorithm_t alg)
{
	return mac_setup(operation, attributes, key_buffer, key_buffer_size, alg);
}

psa_status_t efr32_radioaes_mac_verify_setup(efr32_radioaes_mac_operation_t *operation,
					     const psa_key_attributes_t *attributes,
					     const uint8_t *key_buffer, size_t key_buffer_size,
					     psa_algorithm_t alg)
{
	return mac_setup(operation, attributes, key_buffer, key_buffer_size, alg);
}

psa_status_t efr32_radioaes_mac_update(efr32_radioaes_mac_operation_t *operation,
				       const uint8_t *input, size_t input_length)
{
	while (input_length > 0U) {
		size_t n;

		if (operation->block_len == EFR32_RADIOAES_BLOCK_SIZE) {
			psa_status_t status = cmac_chain(operation, operation->block);

			if (status != PSA_SUCCESS) {
				return status;
			}
			operation->block_len = 0U;
		}
		n = MIN(EFR32_RADIOAES_BLOCK_SIZE - operation->block_len, input_length);
		memcpy(&operation->block[operation->block_len], input, n);
		operation->block_len += (uint8_t)n;
		input += n;
		input_length -= n;
	}
	return PSA_SUCCESS;
}

psa_status_t efr32_radioaes_mac_sign_finish(efr32_radioaes_mac_operation_t *operation,
					    uint8_t *mac, size_t mac_size, size_t *mac_length)
{
	uint8_t full[EFR32_RADIOAES_BLOCK_SIZE];
	psa_status_t status;

	if (mac_size > sizeof(full)) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	status = cmac_final(operation, full);
	if (status == PSA_SUCCESS) {
		memcpy(mac, full, mac_size);
		*mac_length = mac_size;
	}
	memset(full, 0, sizeof(full));
	return status;
}

psa_status_t efr32_radioaes_mac_verify_finish(efr32_radioaes_mac_operation_t *operation,
					      const uint8_t *mac, size_t mac_length)
{
	uint8_t full[EFR32_RADIOAES_BLOCK_SIZE];
	uint8_t diff = 0U;
	psa_status_t status;

	if (mac_length > sizeof(full)) {
		return PSA_ERROR_INVALID_SIGNATURE;
	}
	status = cmac_final(operation, full);
	if (status == PSA_SUCCESS) {
		for (size_t i = 0; i < mac_length; i++) {
			diff |= full[i] ^ mac[i];
		}
		if (diff != 0U) {
			status = PSA_ERROR_INVALID_SIGNATURE;
		}
	}
	memset(full, 0, sizeof(full));
	return status;
}

psa_status_t efr32_radioaes_mac_abort(efr32_radioaes_mac_operation_t *operation)
{
	memset(operation, 0, sizeof(*operation));
	return PSA_SUCCESS;
}

static bool is_ccm(const psa_key_attributes_t *attributes, size_t key_buffer_size,
		   psa_algorithm_t alg, size_t *tag_length)
{
	if (PSA_ALG_AEAD_WITH_DEFAULT_LENGTH_TAG(alg) != PSA_ALG_CCM ||
	    !is_aes_key(attributes, key_buffer_size)) {
		return false;
	}
	*tag_length = PSA_AEAD_TAG_LENGTH(PSA_KEY_TYPE_AES, psa_get_key_bits(attributes), alg);
	return true;
}

psa_status_t efr32_radioaes_aead_encrypt(const psa_key_attributes_t *attributes,
					 const uint8_t *key_buffer, size_t key_buffer_size,
					 psa_algorithm_t alg, const uint8_t *nonce,
					 size_t nonce_length, const uint8_t *additional_data,
					 size_t additional_data_length, const uint8_t *plaintext,
					 size_t plaintext_length, uint8_t *ciphertext,
					 size_t ciphertext_size, size_t *ciphertext_length)
{
	size_t tag_length;
	psa_status_t status;

	if (!is_ccm(attributes, key_buffer_size, alg, &tag_length)) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	if (ciphertext_size < plaintext_length + tag_length) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}

	status = status_from_errno(efr32_radioaes_ccm(
		true, key_buffer, key_buffer_size, nonce, nonce_length, additional_data,
		additional_data_length, plaintext, ciphertext, plaintext_length,
		ciphertext + plaintext_length, tag_length));
	if (status == PSA_SUCCESS) {
		*ciphertext_length = plaintext_length + tag_length;
	}
	return status;
}

psa_status_t efr32_radioaes_aead_decrypt(const psa_key_attributes_t *attributes,
					 const uint8_t *key_buffer, size_t key_buffer_size,
					 psa_algorithm_t alg, const uint8_t *nonce,
					 size_t nonce_length, const uint8_t *additional_data,
					 size_t additional_data_length, const uint8_t *ciphertext,
					 size_t ciphertext_length, uint8_t *plaintext,
					 size_t plaintext_size, size_t *plaintext_length)
{
	size_t tag_length;
	size_t length;
	psa_status_t status;

	if (!is_ccm(attributes, key_buffer_size, alg, &tag_length)) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	if (ciphertext_length < tag_length) {
		return PSA_ERROR_INVALID_SIGNATURE;
	}
	length = ciphertext_length - tag_length;
	if (plaintext_size < length) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}

	status = status_from_errno(efr32_radioaes_ccm(
		false, key_buffer, key_buffer_size, nonce, nonce_length, additional_data,
		additional_data_length, ciphertext, plaintext, length,
		(uint8_t *)(uintptr_t)(ciphertext + length), tag_length));
	if (status == PSA_SUCCESS) {
		*plaintext_length = length;
	}
	return status;
}
