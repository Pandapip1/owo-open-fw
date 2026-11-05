/*
 * RADIOAES driver tests: known answers from the Bluetooth Core Specification v6.1 (Vol 3 Part H
 * Appendix D: AES-CMAC RFC 4493 vectors, f4, f5, f6, g2, h6, ah; Vol 6 Part C 1: LE encryption
 * sample data, the session key and four encrypted PDUs; NIST SP 800-38B D.3 AES-256 CMAC), through
 * the driver and through PSA Crypto, CMAC also multi-part; nested preemption between two callers; execution time and stack per operation; P-256
 * (software, p256-m) timed for comparison.
 *
 * Results: printk, and crypto_results (read over SWD; docs/crypto.md).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/devicetree.h>
#include <soc.h>
#include <psa/crypto.h>

#include "hal/efr32/radioaes.h"

#define CPU_HZ DT_PROP(DT_PATH(cpus, cpu_0), clock_frequency)

enum op {
	OP_ECB,          /* direct, one block */
	OP_CMAC_16,      /* direct, h6-sized message */
	OP_CMAC_65,      /* direct, f6-sized message (the longest SMP message) */
	OP_CCM_ENC_1,    /* direct, LL_START_ENC_RSP: one payload octet */
	OP_CCM_ENC_27,   /* direct, a full 27-octet PDU */
	OP_CCM_DEC_27,   /* direct */
	OP_CCM_ENC_251,  /* direct, the longest PDU with data length extension */
	OP_CCM_DEC_251,  /* direct */
	OP_PSA_ECB,      /* import, cipher_encrypt, destroy: as bt_encrypt_le() */
	OP_PSA_CMAC_65,  /* import, mac_compute, destroy: as bt_crypto_aes_cmac() */
	OP_PSA_CCM_27,   /* import, aead_encrypt, destroy */
	OP_PSA_CMAC_MP_65,  /* import, sign_setup, 3 updates (16 + 33 + 16), finish, destroy */
	OP_PSA_CMAC_MP_256, /* the same over 256 octets in 16-octet updates (a GATT database) */
	OP_P256_KEYGEN,  /* generate + export public: as the host's generate_pub_key() */
	OP_P256_ECDH,    /* import private + raw agreement: as generate_dh_key() */
	OP_COUNT,
};

static const char *const op_name[OP_COUNT] = {
	"ecb", "cmac16", "cmac65", "ccm_enc1", "ccm_enc27", "ccm_dec27", "ccm_enc251",
	"ccm_dec251", "psa_ecb", "psa_cmac65", "psa_ccm27", "psa_cmacmp65", "psa_cmacmp256",
	"p256_keygen", "p256_ecdh",
};

struct op_stats {
	uint32_t n;
	uint32_t dwt_min;
	uint32_t dwt_max;
	uint32_t sys_min;
	uint32_t sys_max;
	uint32_t stack;
	uint32_t hw_min; /* start command to DMA idle (EFR32_RADIOAES_PROBE), CPU cycles */
	uint32_t hw_max;
};

struct crypto_results {
	uint32_t magic;         /* 0xC0FFEE00 + 1 when done */
	uint32_t cpu_hz;
	uint32_t sys_hz;
	uint32_t software_psa;  /* 1: prj-software.conf */
	uint32_t pass;
	uint32_t fail;
	uint32_t first_fail;    /* line number */
	uint32_t preempt_low_ops;
	uint32_t preempt_high_ops;
	uint32_t preempt_errors;
	uint32_t preempt_nested; /* high ops released while the low thread was inside a call */
	uint32_t status_idle;   /* RADIOAES STATUS read while idle, before any operation */
	uint32_t status_after;  /* ... and after the tests */
	uint32_t hw_cfg[3];     /* INCL_IPS_HW_CFG, BA411E_HW_CFG_1, BA411E_HW_CFG_2 */
	struct op_stats op[OP_COUNT];
};

struct crypto_results crypto_results;

static void check(bool ok, int line)
{
	if (ok) {
		crypto_results.pass++;
	} else {
		if (crypto_results.fail == 0U) {
			crypto_results.first_fail = line;
		}
		crypto_results.fail++;
		printk("FAIL line %d\n", line);
	}
}
#define CHECK(x) check((x), __LINE__)

static size_t hex(uint8_t *out, const char *s)
{
	size_t n = 0;
	int hi = -1;

	for (; *s != '\0'; s++) {
		int v;

		if (*s >= '0' && *s <= '9') {
			v = *s - '0';
		} else if (*s >= 'a' && *s <= 'f') {
			v = *s - 'a' + 10;
		} else if (*s >= 'A' && *s <= 'F') {
			v = *s - 'A' + 10;
		} else {
			continue;
		}
		if (hi < 0) {
			hi = v;
		} else {
			out[n++] = (uint8_t)((hi << 4) | v);
			hi = -1;
		}
	}
	return n;
}

/* ---- timing ---- */

static bool dwt_ok;

static void dwt_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	uint32_t a = DWT->CYCCNT;

	k_busy_wait(10);
	dwt_ok = DWT->CYCCNT != a;
}

struct t0 {
	uint32_t dwt;
	uint32_t sys;
};

static inline struct t0 t_start(void)
{
	struct t0 t = { dwt_ok ? DWT->CYCCNT : 0U, k_cycle_get_32() };

	return t;
}

static void t_stop(enum op op, struct t0 t)
{
	uint32_t dwt = dwt_ok ? DWT->CYCCNT - t.dwt : 0U;
	uint32_t sys = k_cycle_get_32() - t.sys;
	struct op_stats *s = &crypto_results.op[op];

	if (s->n == 0U || dwt < s->dwt_min) {
		s->dwt_min = dwt;
	}
	if (s->n == 0U || sys < s->sys_min) {
		s->sys_min = sys;
	}
#if defined(CONFIG_EFR32_RADIOAES_PROBE)
	extern uint32_t efr32_radioaes_probe_cycles;

	if (s->n == 0U || efr32_radioaes_probe_cycles < s->hw_min) {
		s->hw_min = efr32_radioaes_probe_cycles;
	}
	s->hw_max = MAX(s->hw_max, efr32_radioaes_probe_cycles);
#endif
	s->dwt_max = MAX(s->dwt_max, dwt);
	s->sys_max = MAX(s->sys_max, sys);
	s->n++;
}

/* ---- vectors ---- */

/* Vol 3 Part H D.1 (RFC 4493). */
static const char *const cmac_k = "2b7e1516 28aed2a6 abf71588 09cf4f3c";
static const char *const cmac_m64 = "6bc1bee2 2e409f96 e93d7e11 7393172a ae2d8a57 1e03ac9c "
				    "9eb76fac 45af8e51 30c81c46 a35ce411 e5fbc119 1a0a52ef "
				    "f69f2445 df4f9b17 ad2b417b e66c3710";
static const struct {
	size_t len;
	const char *mac;
} cmac_d1[] = {
	{ 0, "bb1d6929 e9593728 7fa37d12 9b756746" },
	{ 16, "070a16b4 6b4d4144 f79bdd9d d04a287c" },
	{ 40, "dfa66747 de9ae630 30ca3261 1497c827" },
	{ 64, "51f0bebf 7e3b9d92 fc497417 79363cfe" },
};

/* D.2-D.6: (key, message, AES_CMAC). */
static const struct {
	const char *key;
	const char *m;
	const char *mac;
} cmac_sc[] = {
	/* f4 */
	{ "d5cb8454 d177733e ffffb2ec 712baeab",
	  "20b003d2 f297be2c 5e2c83a7 e9f9a5b9 eff49111 acf4fddb cc030148 0e359de6 "
	  "55188b3d 32f6bb9a 900afcfb eed4e72a 59cb9ac2 f19d7cfb 6b4fdd49 f47fc5fd 00",
	  "f2c916f1 07a9bd1c f1eda1be a974872d" },
	/* f5 LTK */
	{ "3c128f20 de883288 97624bdb 8dac6989",
	  "0162746c 65d5cb84 54d17773 3effffb2 ec712bae aba6e8e7 cc25a75f 6e216583 "
	  "f7ff3dc4 cf005612 3737bfce 00a71370 2dcfc101 00",
	  "69867911 69d7cd23 980522b5 94750a38" },
	/* f5 MacKey */
	{ "3c128f20 de883288 97624bdb 8dac6989",
	  "0062746c 65d5cb84 54d17773 3effffb2 ec712bae aba6e8e7 cc25a75f 6e216583 "
	  "f7ff3dc4 cf005612 3737bfce 00a71370 2dcfc101 00",
	  "2965f176 a1084a02 fd3f6a20 ce636e20" },
	/* f6 */
	{ "2965f176 a1084a02 fd3f6a20 ce636e20",
	  "d5cb8454 d177733e ffffb2ec 712baeab a6e8e7cc 25a75f6e 216583f7 ff3dc4cf "
	  "12a3343b b453bb54 08da42d2 0c2d0fc8 01010200 56123737 bfce00a7 13702dcf c1",
	  "e3c47398 9cd0e8c5 d26c0b09 da958f61" },
	/* g2 */
	{ "d5cb8454 d177733e ffffb2ec 712baeab",
	  "20b003d2 f297be2c 5e2c83a7 e9f9a5b9 eff49111 acf4fddb cc030148 0e359de6 "
	  "55188b3d 32f6bb9a 900afcfb eed4e72a 59cb9ac2 f19d7cfb 6b4fdd49 f47fc5fd "
	  "a6e8e7cc 25a75f6e 216583f7 ff3dc4cf",
	  "1536d18d e3d20df9 9b7044c1 2f9ed5ba" },
	/* h6 */
	{ "ec0234a3 57c8ad05 341010a6 0a397d9b", "6c656272",
	  "2d9ae102 e76dc91c e8d3a9e2 80b16399" },
};

/* NIST SP 800-38B Appendix D.3, AES-256 (the messages are cmac_m64's first 0, 16, 40, 64). */
static const char *const cmac256_k = "603deb10 15ca71be 2b73aef0 857d7781 1f352c07 3b6108d7 "
				     "2d9810a3 0914dff4";
static const char *const cmac256_mac[] = {
	"028962f6 1b7bf89e fc6b551f 4667d983", "28a7023f 452e8f82 bd4bf28d 8c37c35c",
	"aaf3d8f1 de5640c2 32f5b169 b9c911e6", "e1992190 549f6ed5 696a2c05 6c315410",
};

/* AES-128: D.1 subkey generation AES_128(K, 0), D.7 ah, Vol 6 Part C 1.1 HCI_LE_Encrypt. */
static const struct {
	const char *key;
	const char *pt;
	const char *ct;
} ecb_v[] = {
	{ "2b7e1516 28aed2a6 abf71588 09cf4f3c", "00000000 00000000 00000000 00000000",
	  "7df76b0c 1ab899b3 3e42f047 b91b546f" },
	{ "ec0234a3 57c8ad05 341010a6 0a397d9b", "00000000 00000000 00000000 00708194",
	  "159d5fb7 2ebe2311 a48c1bdc c40dfbaa" },
	{ "4C68384139F574D836BCF34E9DFB01BF", "0213243546576879acbdcedfe0f10213",
	  "99ad1b5226a37e3e058e3b8e27c2c666" },
};

/* Vol 6 Part C 1 and 1.2: SK (MSO first, as AES takes it), IV (LSO first), four PDUs. */
static const char *const ll_sk = "99AD1B5226A37E3E058E3B8E27C2C666";
static const char *const ll_iv = "24 AB DC BA BE BA AF DE";
static const struct {
	uint64_t counter;
	uint8_t dir;      /* 1: Central -> Peripheral */
	uint8_t header;
	const char *clear;
	const char *enc;  /* encrypted payload || MIC, in packet order */
} ll_v[] = {
	{ 0, 1, 0x0f, "06", "9f cd a7 f4 48" },
	{ 0, 0, 0x07, "06", "a3 4c 13 a4 15" },
	{ 1, 1, 0x0e,
	  "17 00 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f 70 71 31 32 33 34 35 36 37 38 39 30",
	  "7a 70 d6 64 15 22 6d f2 6b 17 83 9a 06 04 05 59 6b d6 56 4f 79 6b 5b 9c e6 ff 32 "
	  "f7 5a 6d 33" },
	{ 1, 0, 0x06,
	  "17 00 37 36 35 34 33 32 31 30 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f 50 51",
	  "f3 88 81 e7 bd 94 c9 c3 69 b9 a6 68 46 dd 47 86 aa 8c 39 ce 54 0d 0d ae 3a dc df "
	  "89 b9 60 88" },
};

#define LL_MIC 4U
#define LL_AAD_MASK 0xE3U /* Vol 6 Part E 2: NESN, SN, MD masked */

/* hal/radio.c ccm_nonce_build(): 39-bit counter, direction bit, IV. */
static void ll_nonce(uint8_t nonce[13], uint64_t counter, uint8_t dir)
{
	uint8_t iv[8];

	hex(iv, ll_iv);
	for (int i = 0; i < 5; i++) {
		nonce[i] = (uint8_t)(counter >> (8 * i));
	}
	nonce[4] |= (uint8_t)(dir << 7);
	memcpy(&nonce[5], iv, sizeof(iv));
}

/* ---- tests ---- */

static uint8_t buf_a[272] __aligned(4);
static uint8_t buf_b[272] __aligned(4);

static void test_ecb(void)
{
	uint8_t k[16], p[32], c[32], o[32];

	for (size_t i = 0; i < ARRAY_SIZE(ecb_v); i++) {
		hex(k, ecb_v[i].key);
		hex(p, ecb_v[i].pt);
		hex(c, ecb_v[i].ct);
		CHECK(efr32_radioaes_ecb(true, k, 16, p, o, 16) == 0);
		CHECK(memcmp(o, c, 16) == 0);
		CHECK(efr32_radioaes_ecb(false, k, 16, c, o, 16) == 0);
		CHECK(memcmp(o, p, 16) == 0);
	}
	/* Two blocks in one call, unaligned buffers. */
	hex(k, ecb_v[2].key);
	hex(buf_a + 1, ecb_v[0].pt);
	hex(buf_a + 17, ecb_v[2].pt);
	CHECK(efr32_radioaes_ecb(true, k, 16, buf_a + 1, buf_b + 3, 32) == 0);
	CHECK(efr32_radioaes_ecb(true, k, 16, buf_a + 17, o, 16) == 0);
	hex(c, ecb_v[2].ct);
	CHECK(memcmp(buf_b + 19, c, 16) == 0);
	CHECK(efr32_radioaes_ecb(true, k, 16, buf_a + 1, o, 16) == 0);
	CHECK(memcmp(buf_b + 3, o, 16) == 0);
	/* Unsupported parameters. */
	CHECK(efr32_radioaes_ecb(true, k, 24, p, o, 16) == -EINVAL);
	CHECK(efr32_radioaes_ecb(true, k, 16, p, o, 15) == -EINVAL);
}

static void test_cmac(void)
{
	uint8_t k[16], m[80], mac[16], ref[16];

	hex(k, cmac_k);
	hex(m, cmac_m64);
	for (size_t i = 0; i < ARRAY_SIZE(cmac_d1); i++) {
		hex(ref, cmac_d1[i].mac);
		CHECK(efr32_radioaes_cmac(k, 16, m, cmac_d1[i].len, mac) == 0);
		CHECK(memcmp(mac, ref, 16) == 0);
		/* Unaligned source, the last block ending at the buffer's end. */
		memcpy(buf_a + sizeof(buf_a) - cmac_d1[i].len, m, cmac_d1[i].len);
		CHECK(efr32_radioaes_cmac(k, 16, buf_a + sizeof(buf_a) - cmac_d1[i].len,
					  cmac_d1[i].len, mac) == 0);
		CHECK(memcmp(mac, ref, 16) == 0);
	}
	for (size_t i = 0; i < ARRAY_SIZE(cmac_sc); i++) {
		size_t n;

		hex(k, cmac_sc[i].key);
		n = hex(m, cmac_sc[i].m);
		hex(ref, cmac_sc[i].mac);
		CHECK(efr32_radioaes_cmac(k, 16, m, n, mac) == 0);
		CHECK(memcmp(mac, ref, 16) == 0);
	}
	uint8_t k256[32];

	hex(k256, cmac256_k);
	hex(m, cmac_m64);
	for (size_t i = 0; i < ARRAY_SIZE(cmac256_mac); i++) {
		hex(ref, cmac256_mac[i]);
		CHECK(efr32_radioaes_cmac(k256, 32, m, cmac_d1[i].len, mac) == 0);
		CHECK(memcmp(mac, ref, 16) == 0);
	}
}

static void test_ccm(void)
{
	uint8_t sk[16], nonce[13], clear[64], enc[64], out[64], tag[4];

	hex(sk, ll_sk);
	for (size_t i = 0; i < ARRAY_SIZE(ll_v); i++) {
		size_t n = hex(clear, ll_v[i].clear);
		uint8_t aad = ll_v[i].header & LL_AAD_MASK;

		hex(enc, ll_v[i].enc);
		ll_nonce(nonce, ll_v[i].counter, ll_v[i].dir);
		CHECK(efr32_radioaes_ccm(true, sk, 16, nonce, 13, &aad, 1, clear, out, n, tag,
					 LL_MIC) == 0);
		CHECK(memcmp(out, enc, n) == 0 && memcmp(tag, enc + n, LL_MIC) == 0);
		CHECK(efr32_radioaes_ccm(false, sk, 16, nonce, 13, &aad, 1, enc, out, n, enc + n,
					 LL_MIC) == 0);
		CHECK(memcmp(out, clear, n) == 0);
		/* A corrupted MIC fails and leaves no plaintext. */
		enc[n] ^= 0x01U;
		CHECK(efr32_radioaes_ccm(false, sk, 16, nonce, 13, &aad, 1, enc, out, n, enc + n,
					 LL_MIC) == -EBADMSG);
		for (size_t j = 0; j < n; j++) {
			CHECK(out[j] == 0U);
		}
		/* The wrong direction bit (the other nonce) fails too. */
		enc[n] ^= 0x01U;
		ll_nonce(nonce, ll_v[i].counter, !ll_v[i].dir);
		CHECK(efr32_radioaes_ccm(false, sk, 16, nonce, 13, &aad, 1, enc, out, n, enc + n,
					 LL_MIC) == -EBADMSG);
	}
	/* Round trips without reference data: the longest PDU (in place), a
	 * 16-octet tag with 20 octets of associated data.
	 */
	ll_nonce(nonce, 0x7FFFFFFFFFULL, 1);
	for (size_t i = 0; i < 251; i++) {
		buf_a[i] = (uint8_t)i;
	}
	uint8_t aad = 0x02;
	uint8_t tag16[16];

	CHECK(efr32_radioaes_ccm(true, sk, 16, nonce, 13, &aad, 1, buf_a, buf_b, 251, tag,
				 LL_MIC) == 0);
	CHECK(efr32_radioaes_ccm(false, sk, 16, nonce, 13, &aad, 1, buf_b, buf_b, 251, tag,
				 LL_MIC) == 0);
	CHECK(memcmp(buf_a, buf_b, 251) == 0);
	/* An empty payload is declined (radioaes.c). */
	CHECK(efr32_radioaes_ccm(true, sk, 16, nonce, 13, &aad, 1, NULL, NULL, 0, tag, LL_MIC) ==
	      -EINVAL);
	CHECK(efr32_radioaes_ccm(true, sk, 16, nonce, 13, buf_a, 20, buf_a, buf_b, 33, tag16, 16) ==
	      0);
	CHECK(efr32_radioaes_ccm(false, sk, 16, nonce, 13, buf_a, 20, buf_b, buf_b, 33, tag16,
				 16) == 0);
	CHECK(memcmp(buf_a, buf_b, 33) == 0);
	CHECK(efr32_radioaes_ccm(true, sk, 16, nonce, 12, &aad, 1, buf_a, buf_b, 1, tag, 4) ==
	      -EINVAL);
}

static psa_key_id_t import(psa_key_type_t type, psa_algorithm_t alg, psa_key_usage_t usage,
			   const uint8_t *key, size_t len)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;

	psa_set_key_type(&attr, type);
	psa_set_key_usage_flags(&attr, usage);
	psa_set_key_algorithm(&attr, alg);
	if (psa_import_key(&attr, key, len, &id) != PSA_SUCCESS) {
		return 0;
	}
	return id;
}

static void test_psa(void)
{
	uint8_t k[16], p[80], c[80], o[80];
	psa_algorithm_t ccm = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, LL_MIC);
	psa_key_id_t id;
	size_t n, len;

	for (size_t i = 0; i < ARRAY_SIZE(ecb_v); i++) {
		hex(k, ecb_v[i].key);
		hex(p, ecb_v[i].pt);
		hex(c, ecb_v[i].ct);
		id = import(PSA_KEY_TYPE_AES, PSA_ALG_ECB_NO_PADDING,
			    PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT, k, 16);
		CHECK(id != 0);
		CHECK(psa_cipher_encrypt(id, PSA_ALG_ECB_NO_PADDING, p, 16, o, 16, &len) ==
		      PSA_SUCCESS && len == 16 && memcmp(o, c, 16) == 0);
		CHECK(psa_cipher_decrypt(id, PSA_ALG_ECB_NO_PADDING, c, 16, o, 16, &len) ==
		      PSA_SUCCESS && len == 16 && memcmp(o, p, 16) == 0);
		CHECK(psa_destroy_key(id) == PSA_SUCCESS);
	}
	for (size_t i = 0; i < ARRAY_SIZE(cmac_sc); i++) {
		hex(k, cmac_sc[i].key);
		n = hex(p, cmac_sc[i].m);
		hex(c, cmac_sc[i].mac);
		id = import(PSA_KEY_TYPE_AES, PSA_ALG_CMAC, PSA_KEY_USAGE_SIGN_MESSAGE, k, 16);
		CHECK(id != 0);
		CHECK(psa_mac_compute(id, PSA_ALG_CMAC, p, n, o, 16, &len) == PSA_SUCCESS &&
		      len == 16 && memcmp(o, c, 16) == 0);
		CHECK(psa_destroy_key(id) == PSA_SUCCESS);
	}
	hex(k, ll_sk);
	id = import(PSA_KEY_TYPE_AES, ccm, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT, k, 16);
	CHECK(id != 0);
	for (size_t i = 0; i < ARRAY_SIZE(ll_v); i++) {
		uint8_t nonce[13];
		uint8_t aad = ll_v[i].header & LL_AAD_MASK;

		n = hex(p, ll_v[i].clear);
		hex(c, ll_v[i].enc);
		ll_nonce(nonce, ll_v[i].counter, ll_v[i].dir);
		CHECK(psa_aead_encrypt(id, ccm, nonce, 13, &aad, 1, p, n, o, sizeof(o), &len) ==
		      PSA_SUCCESS && len == n + LL_MIC && memcmp(o, c, len) == 0);
		CHECK(psa_aead_decrypt(id, ccm, nonce, 13, &aad, 1, c, n + LL_MIC, o, sizeof(o),
				       &len) == PSA_SUCCESS && len == n && memcmp(o, p, n) == 0);
		c[n] ^= 0x80U;
		CHECK(psa_aead_decrypt(id, ccm, nonce, 13, &aad, 1, c, n + LL_MIC, o, sizeof(o),
				       &len) == PSA_ERROR_INVALID_SIGNATURE);
	}
	CHECK(psa_destroy_key(id) == PSA_SUCCESS);
}

/* Multi-part CMAC (psa_mac_sign_setup/update/finish, as the GATT database hash): @p m fed in
 * pieces of @p chunk octets.
 */
static psa_status_t mac_multipart(psa_key_id_t id, psa_algorithm_t alg, const uint8_t *m,
				  size_t n, size_t chunk, uint8_t *mac, size_t *len)
{
	psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
	psa_status_t status = psa_mac_sign_setup(&op, id, alg);

	for (size_t at = 0; status == PSA_SUCCESS && at < n; at += chunk) {
		status = psa_mac_update(&op, m + at, MIN(chunk, n - at));
	}
	if (status == PSA_SUCCESS) {
		return psa_mac_sign_finish(&op, mac, 16, len);
	}
	psa_mac_abort(&op);
	return status;
}

static psa_status_t mac_verify_multipart(psa_key_id_t id, psa_algorithm_t alg, const uint8_t *m,
					 size_t n, size_t chunk, const uint8_t *mac, size_t len)
{
	psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
	psa_status_t status = psa_mac_verify_setup(&op, id, alg);

	for (size_t at = 0; status == PSA_SUCCESS && at < n; at += chunk) {
		status = psa_mac_update(&op, m + at, MIN(chunk, n - at));
	}
	if (status == PSA_SUCCESS) {
		return psa_mac_verify_finish(&op, mac, len);
	}
	psa_mac_abort(&op);
	return status;
}

static void test_psa_multipart(void)
{
	static const size_t chunks[] = {1, 7, 15, 16, 17, 33, 80};
	uint8_t k[32], m[80], ref[16], o[16];
	psa_key_usage_t usage = PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE;
	psa_algorithm_t trunc8 = PSA_ALG_TRUNCATED_MAC(PSA_ALG_CMAC, 8);
	psa_key_id_t id;
	size_t len, n;

	/* RFC 4493 (D.1) with AES-128 and SP 800-38B D.3 with AES-256, every split. */
	hex(m, cmac_m64);
	for (int bits = 128; bits <= 256; bits += 128) {
		hex(k, bits == 128 ? cmac_k : cmac256_k);
		id = import(PSA_KEY_TYPE_AES, PSA_ALG_CMAC, usage, k, bits / 8);
		CHECK(id != 0);
		for (size_t i = 0; i < ARRAY_SIZE(cmac_d1); i++) {
			hex(ref, bits == 128 ? cmac_d1[i].mac : cmac256_mac[i]);
			for (size_t c = 0; c < ARRAY_SIZE(chunks); c++) {
				CHECK(mac_multipart(id, PSA_ALG_CMAC, m, cmac_d1[i].len, chunks[c], o,
						    &len) == PSA_SUCCESS &&
				      len == 16 && memcmp(o, ref, 16) == 0);
			}
			CHECK(mac_verify_multipart(id, PSA_ALG_CMAC, m, cmac_d1[i].len, 7, ref, 16) ==
			      PSA_SUCCESS);
			ref[15] ^= 0x01U;
			CHECK(mac_verify_multipart(id, PSA_ALG_CMAC, m, cmac_d1[i].len, 7, ref, 16) ==
			      PSA_ERROR_INVALID_SIGNATURE);
		}
		CHECK(psa_destroy_key(id) == PSA_SUCCESS);
	}

	/* D.2-D.6, in 5-octet pieces; a truncated MAC is the full one's prefix. */
	for (size_t i = 0; i < ARRAY_SIZE(cmac_sc); i++) {
		hex(k, cmac_sc[i].key);
		n = hex(m, cmac_sc[i].m);
		hex(ref, cmac_sc[i].mac);
		id = import(PSA_KEY_TYPE_AES, PSA_ALG_CMAC, usage, k, 16);
		CHECK(id != 0);
		CHECK(mac_multipart(id, PSA_ALG_CMAC, m, n, 5, o, &len) == PSA_SUCCESS && len == 16 &&
		      memcmp(o, ref, 16) == 0);
		CHECK(psa_destroy_key(id) == PSA_SUCCESS);
		id = import(PSA_KEY_TYPE_AES, trunc8, usage, k, 16);
		CHECK(id != 0);
		memset(o, 0, sizeof(o));
		CHECK(mac_multipart(id, trunc8, m, n, 5, o, &len) == PSA_SUCCESS && len == 8 &&
		      memcmp(o, ref, 8) == 0);
		CHECK(mac_verify_multipart(id, trunc8, m, n, 5, ref, 8) == PSA_SUCCESS);
		CHECK(psa_destroy_key(id) == PSA_SUCCESS);
	}

	/* An aborted operation leaves nothing behind: a new one on the same object is exact. */
	psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;

	hex(k, cmac_k);
	hex(m, cmac_m64);
	hex(ref, cmac_d1[2].mac);
	id = import(PSA_KEY_TYPE_AES, PSA_ALG_CMAC, usage, k, 16);
	CHECK(psa_mac_sign_setup(&op, id, PSA_ALG_CMAC) == PSA_SUCCESS);
	CHECK(psa_mac_update(&op, m + 3, 21) == PSA_SUCCESS);
	CHECK(psa_mac_abort(&op) == PSA_SUCCESS);
	CHECK(psa_mac_sign_setup(&op, id, PSA_ALG_CMAC) == PSA_SUCCESS);
	CHECK(psa_mac_update(&op, m, 40) == PSA_SUCCESS);
	CHECK(psa_mac_sign_finish(&op, o, sizeof(o), &len) == PSA_SUCCESS && len == 16 &&
	      memcmp(o, ref, 16) == 0);
	/* A finished operation takes no more input. */
	CHECK(psa_mac_update(&op, m, 1) == PSA_ERROR_BAD_STATE);
	CHECK(psa_destroy_key(id) == PSA_SUCCESS);
}

/* ---- timing ---- */

#define REPS 64

static void time_ops(void)
{
	uint8_t k[16], m[80], mac[16], nonce[13], tag[4], o[80];
	uint8_t aad = 0x02;
	size_t n65, len;
	psa_key_id_t id;
	psa_algorithm_t ccm = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, LL_MIC);
	psa_mac_operation_t mp = PSA_MAC_OPERATION_INIT;
	static uint8_t big[256];
	struct t0 t;

	hex(k, ll_sk);
	n65 = hex(m, cmac_sc[3].m);
	ll_nonce(nonce, 1, 1);
	for (int r = 0; r < REPS; r++) {
		unsigned int key = irq_lock();

		t = t_start();
		(void)efr32_radioaes_ecb(true, k, 16, m, o, 16);
		t_stop(OP_ECB, t);
		t = t_start();
		(void)efr32_radioaes_cmac(k, 16, m, 16, mac);
		t_stop(OP_CMAC_16, t);
		t = t_start();
		(void)efr32_radioaes_cmac(k, 16, m, n65, mac);
		t_stop(OP_CMAC_65, t);
		t = t_start();
		(void)efr32_radioaes_ccm(true, k, 16, nonce, 13, &aad, 1, buf_a, buf_b, 1, tag, 4);
		t_stop(OP_CCM_ENC_1, t);
		t = t_start();
		(void)efr32_radioaes_ccm(true, k, 16, nonce, 13, &aad, 1, buf_a, buf_b, 27, tag, 4);
		t_stop(OP_CCM_ENC_27, t);
		t = t_start();
		(void)efr32_radioaes_ccm(false, k, 16, nonce, 13, &aad, 1, buf_b, buf_a, 27, tag, 4);
		t_stop(OP_CCM_DEC_27, t);
		t = t_start();
		(void)efr32_radioaes_ccm(true, k, 16, nonce, 13, &aad, 1, buf_a, buf_b, 251, tag, 4);
		t_stop(OP_CCM_ENC_251, t);
		t = t_start();
		(void)efr32_radioaes_ccm(false, k, 16, nonce, 13, &aad, 1, buf_b, buf_a, 251, tag, 4);
		t_stop(OP_CCM_DEC_251, t);
		irq_unlock(key);

		t = t_start();
		id = import(PSA_KEY_TYPE_AES, PSA_ALG_ECB_NO_PADDING, PSA_KEY_USAGE_ENCRYPT, k, 16);
		(void)psa_cipher_encrypt(id, PSA_ALG_ECB_NO_PADDING, m, 16, o, 16, &len);
		(void)psa_destroy_key(id);
		t_stop(OP_PSA_ECB, t);
		t = t_start();
		id = import(PSA_KEY_TYPE_AES, PSA_ALG_CMAC, PSA_KEY_USAGE_SIGN_MESSAGE, k, 16);
		(void)psa_mac_compute(id, PSA_ALG_CMAC, m, n65, mac, 16, &len);
		(void)psa_destroy_key(id);
		t_stop(OP_PSA_CMAC_65, t);
		t = t_start();
		id = import(PSA_KEY_TYPE_AES, ccm, PSA_KEY_USAGE_ENCRYPT, k, 16);
		(void)psa_aead_encrypt(id, ccm, nonce, 13, &aad, 1, buf_a, 27, o, sizeof(o), &len);
		(void)psa_destroy_key(id);
		t_stop(OP_PSA_CCM_27, t);
		t = t_start();
		id = import(PSA_KEY_TYPE_AES, PSA_ALG_CMAC, PSA_KEY_USAGE_SIGN_MESSAGE, k, 16);
		(void)psa_mac_sign_setup(&mp, id, PSA_ALG_CMAC);
		(void)psa_mac_update(&mp, m, 16);
		(void)psa_mac_update(&mp, m + 16, 33);
		(void)psa_mac_update(&mp, m + 49, n65 - 49);
		(void)psa_mac_sign_finish(&mp, mac, 16, &len);
		(void)psa_destroy_key(id);
		t_stop(OP_PSA_CMAC_MP_65, t);
		t = t_start();
		id = import(PSA_KEY_TYPE_AES, PSA_ALG_CMAC, PSA_KEY_USAGE_SIGN_MESSAGE, k, 16);
		(void)psa_mac_sign_setup(&mp, id, PSA_ALG_CMAC);
		for (size_t i = 0; i < sizeof(big); i += 16) {
			(void)psa_mac_update(&mp, big + i, 16);
		}
		(void)psa_mac_sign_finish(&mp, mac, 16, &len);
		(void)psa_destroy_key(id);
		t_stop(OP_PSA_CMAC_MP_256, t);
	}

	/* P-256 (software), twice: keep the run short. */
	for (int r = 0; r < 2; r++) {
		psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
		psa_key_id_t priv, peer_priv;
		uint8_t pub[65], peer_pub[65], dh[32];

		psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
		psa_set_key_bits(&attr, 256);
		psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_DERIVE);
		psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
		CHECK(psa_generate_key(&attr, &peer_priv) == PSA_SUCCESS);
		CHECK(psa_export_public_key(peer_priv, peer_pub, sizeof(peer_pub), &len) ==
		      PSA_SUCCESS);
		CHECK(psa_destroy_key(peer_priv) == PSA_SUCCESS);

		t = t_start();
		CHECK(psa_generate_key(&attr, &priv) == PSA_SUCCESS);
		CHECK(psa_export_public_key(priv, pub, sizeof(pub), &len) == PSA_SUCCESS);
		t_stop(OP_P256_KEYGEN, t);
		t = t_start();
		CHECK(psa_raw_key_agreement(PSA_ALG_ECDH, priv, peer_pub, sizeof(peer_pub), dh,
					    sizeof(dh), &len) == PSA_SUCCESS);
		t_stop(OP_P256_ECDH, t);
		CHECK(psa_destroy_key(priv) == PSA_SUCCESS);
	}
}

/* ---- stack per operation ---- */

#define STACK_SIZE 2048
K_THREAD_STACK_DEFINE(op_stack, STACK_SIZE);
static struct k_thread op_thread;

static void run_op(void *p1, void *p2, void *p3)
{
	enum op op = (enum op)(uintptr_t)p1;
	uint8_t k[16] = { 1 }, m[80] = { 2 }, mac[16], nonce[13] = { 3 }, tag[4], o[80];
	uint8_t aad = 0x02;
	size_t len;
	psa_key_id_t id;
	psa_algorithm_t ccm = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, LL_MIC);

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	switch (op) {
	case OP_ECB:
		(void)efr32_radioaes_ecb(true, k, 16, m, o, 16);
		break;
	case OP_CMAC_65:
		(void)efr32_radioaes_cmac(k, 16, m, 65, mac);
		break;
	case OP_CCM_ENC_27:
		(void)efr32_radioaes_ccm(true, k, 16, nonce, 13, &aad, 1, m, o, 27, tag, 4);
		break;
	case OP_CCM_DEC_27:
		(void)efr32_radioaes_ccm(false, k, 16, nonce, 13, &aad, 1, m, o, 27, tag, 4);
		break;
	case OP_PSA_ECB:
		id = import(PSA_KEY_TYPE_AES, PSA_ALG_ECB_NO_PADDING, PSA_KEY_USAGE_ENCRYPT, k, 16);
		(void)psa_cipher_encrypt(id, PSA_ALG_ECB_NO_PADDING, m, 16, o, 16, &len);
		(void)psa_destroy_key(id);
		break;
	case OP_PSA_CMAC_65:
		id = import(PSA_KEY_TYPE_AES, PSA_ALG_CMAC, PSA_KEY_USAGE_SIGN_MESSAGE, k, 16);
		(void)psa_mac_compute(id, PSA_ALG_CMAC, m, 65, mac, 16, &len);
		(void)psa_destroy_key(id);
		break;
	case OP_PSA_CCM_27:
		id = import(PSA_KEY_TYPE_AES, ccm, PSA_KEY_USAGE_ENCRYPT, k, 16);
		(void)psa_aead_encrypt(id, ccm, nonce, 13, &aad, 1, m, 27, o, sizeof(o), &len);
		(void)psa_destroy_key(id);
		break;
	default:
		break;
	}
}

static void measure_stack(enum op op)
{
	size_t unused;

	/* Zero, so the high-water mark is this operation's alone. */
	memset(K_THREAD_STACK_BUFFER(op_stack), 0xAA, STACK_SIZE);
	k_thread_create(&op_thread, op_stack, STACK_SIZE, run_op, (void *)(uintptr_t)op, NULL,
			NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	k_thread_join(&op_thread, K_FOREVER);
	if (k_thread_stack_space_get(&op_thread, &unused) == 0) {
		crypto_results.op[op].stack = STACK_SIZE - unused;
	}
}

/* ---- nested preemption ---- */

K_THREAD_STACK_DEFINE(high_stack, 1024);
static struct k_thread high_thread;
static K_SEM_DEFINE(high_go, 0, 1);
static volatile bool low_inside;
static volatile bool preempt_stop;

static void high_entry(void *p1, void *p2, void *p3)
{
	uint8_t sk[16], nonce[13], clear[64], enc[64], out[64], tag[4];
	size_t n;
	uint8_t aad = ll_v[2].header & LL_AAD_MASK;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	hex(sk, ll_sk);
	n = hex(clear, ll_v[2].clear);
	hex(enc, ll_v[2].enc);
	ll_nonce(nonce, ll_v[2].counter, ll_v[2].dir);
	while (!preempt_stop) {
		k_sem_take(&high_go, K_FOREVER);
		if (low_inside) {
			crypto_results.preempt_nested++;
		}
		if (efr32_radioaes_ccm(true, sk, 16, nonce, 13, &aad, 1, clear, out, n, tag, 4) != 0 ||
		    memcmp(out, enc, n) != 0 || memcmp(tag, enc + n, 4) != 0) {
			crypto_results.preempt_errors++;
		}
		crypto_results.preempt_high_ops++;
	}
}

static void high_kick(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_sem_give(&high_go);
}

static K_TIMER_DEFINE(kick_timer, high_kick, NULL);

static void test_preemption(void)
{
	uint8_t k[16], m[80], ref[16], mac[16];
	size_t n = hex(m, cmac_sc[3].m);
	int64_t end;

	hex(k, cmac_sc[3].key);
	hex(ref, cmac_sc[3].mac);
	k_thread_create(&high_thread, high_stack, K_THREAD_STACK_SIZEOF(high_stack), high_entry,
			NULL, NULL, NULL, K_PRIO_COOP(0), 0, K_NO_WAIT);
	/* An odd period so the releases fall at varying points of the low thread's calls. */
	k_timer_start(&kick_timer, K_USEC(97), K_USEC(97));
	end = k_uptime_get() + 2000;
	while (k_uptime_get() < end) {
		low_inside = true;
		int err = efr32_radioaes_cmac(k, 16, m, n, mac);

		low_inside = false;
		if (err != 0 || memcmp(mac, ref, 16) != 0) {
			crypto_results.preempt_errors++;
		}
		crypto_results.preempt_low_ops++;
	}
	k_timer_stop(&kick_timer);
	preempt_stop = true;
	k_sem_give(&high_go);
	k_thread_join(&high_thread, K_FOREVER);
	CHECK(crypto_results.preempt_errors == 0U);
	CHECK(crypto_results.preempt_nested > 0U);
}

int main(void)
{
	crypto_results.cpu_hz = CPU_HZ;
	crypto_results.sys_hz = sys_clock_hw_cycles_per_sec();
	crypto_results.software_psa = IS_ENABLED(CONFIG_EFR32_RADIOAES_PSA) ? 0U : 1U;
	crypto_results.status_idle = RADIOAES->STATUS;
	crypto_results.hw_cfg[0] = RADIOAES->INCL_IPS_HW_CFG;
	crypto_results.hw_cfg[1] = RADIOAES->BA411E_HW_CFG_1;
	crypto_results.hw_cfg[2] = RADIOAES->BA411E_HW_CFG_2;
	dwt_init();
	CHECK(psa_crypto_init() == PSA_SUCCESS);

	test_ecb();
	test_cmac();
	test_ccm();
	test_psa();
	test_psa_multipart();
	test_preemption();
	time_ops();
	for (enum op op = OP_ECB; op <= OP_PSA_CCM_27; op++) {
		measure_stack(op);
	}

	crypto_results.status_after = RADIOAES->STATUS;
	printk("crypto-port: %s PSA, cpu %u Hz, sys %u Hz, dwt %d, STATUS idle 0x%08x, "
	       "HW_CFG 0x%08x 0x%08x 0x%08x\n",
	       crypto_results.software_psa ? "software" : "RADIOAES", crypto_results.cpu_hz,
	       crypto_results.sys_hz, dwt_ok, crypto_results.status_idle,
	       crypto_results.hw_cfg[0], crypto_results.hw_cfg[1], crypto_results.hw_cfg[2]);
	for (int i = 0; i < OP_COUNT; i++) {
		struct op_stats *s = &crypto_results.op[i];

		printk("op %-12s n %3u dwt %7u..%7u hw %5u..%5u sys %7u..%7u stack %4u\n",
		       op_name[i], s->n, s->dwt_min, s->dwt_max, s->hw_min, s->hw_max, s->sys_min,
		       s->sys_max, s->stack);
	}
	printk("preemption: low %u high %u nested %u errors %u\n", crypto_results.preempt_low_ops,
	       crypto_results.preempt_high_ops, crypto_results.preempt_nested,
	       crypto_results.preempt_errors);
	printk("crypto-port: %s (%u passed, %u failed, first at line %u)\n",
	       crypto_results.fail == 0U ? "PASS" : "FAIL", crypto_results.pass,
	       crypto_results.fail, crypto_results.first_fail);
	crypto_results.magic = 0xC0FFEE01U;
	return 0;
}
