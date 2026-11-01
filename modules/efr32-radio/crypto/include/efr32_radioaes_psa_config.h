/*
 * TF-PSA-Crypto driver configuration (CONFIG_TF_PSA_CRYPTO_DRIVER_CONFIG_FILE, included at the end
 * of Zephyr's config-tf-psa-crypto.h): the RADIOAES transparent driver is present and accelerates
 * AES-ECB, AES-CMAC and AES-CCM, so TF-PSA-Crypto builds no software AES for them
 * (drivers/builtin/include/mbedtls/private/crypto_adjust_config_enable_builtins.h). An AES mode
 * outside these keeps TF-PSA-Crypto's software block cipher, and the driver declines it.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIOAES_PSA_CONFIG_H
#define EFR32_RADIOAES_PSA_CONFIG_H

#define PSA_CRYPTO_DRIVER_EFR32_RADIOAES

#define MBEDTLS_PSA_ACCEL_KEY_TYPE_AES
#define MBEDTLS_PSA_ACCEL_ALG_ECB_NO_PADDING
#define MBEDTLS_PSA_ACCEL_ALG_CMAC
#define MBEDTLS_PSA_ACCEL_ALG_CCM

#endif /* EFR32_RADIOAES_PSA_CONFIG_H */
