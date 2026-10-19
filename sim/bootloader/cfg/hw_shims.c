/*
 * Build shims for running the stock Gecko Bootloader sources under QEMU.
 *
 * - sha_x_process(): the SDK implementation drives the CRYPTOACC hash engine.
 *   QEMU has no CRYPTOACC model, so the same block-compression contract
 *   (big-endian state in/out) is implemented in software.
 * - ECDSA: signature verification needs the CRYPTOACC PKE and is not
 *   exercised (BOOTLOADER_ENFORCE_SIGNED_UPGRADE=0); report failure.
 */
#include <stdint.h>
#include <string.h>
#include "security/sha/btl_sha256.h"
#include "mbedtls/error.h"
#include "sx_ecc_curves.h"
#include "sx_ecdsa_alg.h"

static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

int sha_x_process(SHA_Type_t algo, uint8_t *state_in, const unsigned char *blockdata,
                  uint8_t *state_out, uint32_t num_blocks)
{
  uint32_t s[8], w[64];

  if (algo != SHA256) {
    return MBEDTLS_ERR_PLATFORM_FEATURE_UNSUPPORTED;
  }
  for (int i = 0; i < 8; i++) {
    s[i] = (uint32_t)state_in[4 * i] << 24 | state_in[4 * i + 1] << 16
           | state_in[4 * i + 2] << 8 | state_in[4 * i + 3];
  }
  while (num_blocks--) {
    for (int i = 0; i < 16; i++) {
      w[i] = (uint32_t)blockdata[4 * i] << 24 | blockdata[4 * i + 1] << 16
             | blockdata[4 * i + 2] << 8 | blockdata[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
    blockdata += 64;
  }
  for (int i = 0; i < 8; i++) {
    state_out[4 * i] = s[i] >> 24; state_out[4 * i + 1] = s[i] >> 16;
    state_out[4 * i + 2] = s[i] >> 8; state_out[4 * i + 3] = s[i];
  }
  return 0;
}

const sx_ecc_curve_t sx_ecc_curve_p256;

uint32_t ecdsa_verify_signature_digest(const sx_ecc_curve_t *curve, block_t digest,
                                       block_t qa, block_t signature)
{
  (void)curve; (void)digest; (void)qa; (void)signature;
  return 1; /* failure */
}
