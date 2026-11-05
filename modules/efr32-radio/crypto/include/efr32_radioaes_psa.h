/*
 * PSA Crypto transparent-driver entry points on RADIOAES (crypto/radioaes_psa.c), called from the
 * TF-PSA-Crypto dispatch that crypto/CMakeLists.txt derives at configure time.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIOAES_PSA_H
#define EFR32_RADIOAES_PSA_H

#include <psa/crypto_driver_common.h>

#include "efr32_radioaes_psa_contexts.h"

psa_status_t efr32_radioaes_cipher_encrypt(const psa_key_attributes_t *attributes,
					   const uint8_t *key_buffer, size_t key_buffer_size,
					   psa_algorithm_t alg, const uint8_t *iv, size_t iv_length,
					   const uint8_t *input, size_t input_length,
					   uint8_t *output, size_t output_size,
					   size_t *output_length);

psa_status_t efr32_radioaes_cipher_decrypt(const psa_key_attributes_t *attributes,
					   const uint8_t *key_buffer, size_t key_buffer_size,
					   psa_algorithm_t alg, const uint8_t *input,
					   size_t input_length, uint8_t *output, size_t output_size,
					   size_t *output_length);

psa_status_t efr32_radioaes_mac_compute(const psa_key_attributes_t *attributes,
					const uint8_t *key_buffer, size_t key_buffer_size,
					psa_algorithm_t alg, const uint8_t *input,
					size_t input_length, uint8_t *mac, size_t mac_size,
					size_t *mac_length);

psa_status_t efr32_radioaes_mac_sign_setup(efr32_radioaes_mac_operation_t *operation,
					   const psa_key_attributes_t *attributes,
					   const uint8_t *key_buffer, size_t key_buffer_size,
					   psa_algorithm_t alg);

psa_status_t efr32_radioaes_mac_verify_setup(efr32_radioaes_mac_operation_t *operation,
					     const psa_key_attributes_t *attributes,
					     const uint8_t *key_buffer, size_t key_buffer_size,
					     psa_algorithm_t alg);

psa_status_t efr32_radioaes_mac_update(efr32_radioaes_mac_operation_t *operation,
				       const uint8_t *input, size_t input_length);

psa_status_t efr32_radioaes_mac_sign_finish(efr32_radioaes_mac_operation_t *operation,
					    uint8_t *mac, size_t mac_size, size_t *mac_length);

psa_status_t efr32_radioaes_mac_verify_finish(efr32_radioaes_mac_operation_t *operation,
					      const uint8_t *mac, size_t mac_length);

psa_status_t efr32_radioaes_mac_abort(efr32_radioaes_mac_operation_t *operation);

psa_status_t efr32_radioaes_aead_encrypt(const psa_key_attributes_t *attributes,
					 const uint8_t *key_buffer, size_t key_buffer_size,
					 psa_algorithm_t alg, const uint8_t *nonce,
					 size_t nonce_length, const uint8_t *additional_data,
					 size_t additional_data_length, const uint8_t *plaintext,
					 size_t plaintext_length, uint8_t *ciphertext,
					 size_t ciphertext_size, size_t *ciphertext_length);

psa_status_t efr32_radioaes_aead_decrypt(const psa_key_attributes_t *attributes,
					 const uint8_t *key_buffer, size_t key_buffer_size,
					 psa_algorithm_t alg, const uint8_t *nonce,
					 size_t nonce_length, const uint8_t *additional_data,
					 size_t additional_data_length, const uint8_t *ciphertext,
					 size_t ciphertext_length, uint8_t *plaintext,
					 size_t plaintext_size, size_t *plaintext_length);

#endif /* EFR32_RADIOAES_PSA_H */
