/*
 * AES-ECB, AES-CMAC and AES-CCM on the EFR32BG22's RADIOAES accelerator.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sources for every register, bit and descriptor field used here (docs/crypto.md):
 *
 * [RM]  EFR32xG22 Reference Manual rev. 1.2: 3.2 memory map (RADIOAES at 0x44000000);
 *       8.5 CMU_CLKEN0.RADIOAES (bit 2, "Enable Bus Clock") and 8.5.25 CMU_RADIOCLKCTRL.EN
 *       ("Enables radio bridge clock"); 11.4 the accelerator DMA interface (FETCHADDR,
 *       FETCHLEN, PUSHADDR, PUSHLEN, IF, IF_CLR, CTRL, CMD, STATUS) and 11.2 the modes (ECB,
 *       CMAC, CCM among them). The RM documents this DMA register block for CRYPTOACC; RADIOAES
 *       has the identical layout ([H]).
 * [H]   hal_silabs simplicity_sdk/platform/Device/SiliconLabs/EFR32BG22/Include/efr32bg22_aes.h
 *       (Zlib): AES_TypeDef and its bit fields, RADIOAES_BASE in efr32bg22*.h.
 * [Z]   hal_silabs simplicity_sdk/platform/security/sl_component/sl_protocol_crypto/src/
 *       sli_protocol_crypto_radioaes.c and sli_radioaes_management.c (Zlib, Silicon Labs): the
 *       scatter-gather descriptor, the descriptor tag (engine select, config/data, last, data
 *       type, config offset, invalid bytes), the BA411E configuration offsets and mode words,
 *       the descriptor sequences for ECB, CMAC and CCM, and the clocks they enable.
 * [SP]  NIST SP 800-38B (CMAC) and SP 800-38C (CCM: B0 flags, A.2.1/A.2.2 formatting).
 *
 * Nothing here comes from a proprietary binary or an unlicensed document.
 *
 * Concurrency. The callers are threads of different priorities (the Bluetooth host's long work
 * queue and RX thread, and the controller's ULL and LLL meta-IRQ threads), and a higher-priority
 * caller may preempt a lower one anywhere. Each call therefore (1) waits until the DMA is idle,
 * which finishes any operation a preempted caller had started (the accelerator writes its result
 * to that caller's buffers), (2) saves FETCHADDR/PUSHADDR, which a preempted caller may have
 * written but not yet started, (3) runs its own descriptor chain to completion and (4) restores
 * the two pointers. Preemption on one core nests, so this holds at any depth; CTRL is the same
 * value for every caller. A caller is delayed by at most one in-flight operation of a lower
 * priority (bounded by the longest operation, docs/crypto.md), never by a lock holder that has
 * been preempted. Not for use from interrupt handlers (none do).
 */
#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/sys/util.h>
#include <soc.h>

#include "hal/efr32/radioaes.h"

/* Scatter-gather descriptor [Z: sli_radioaes_dma_descr_t]. The DMA reads it, so every field is
 * stored before the start command (see radioaes_run()).
 */
struct radioaes_desc {
	volatile uint32_t address;
	volatile uint32_t next;
	volatile uint32_t length;
	volatile uint32_t tag;
};

/* next: bit 0 stops the chain after this descriptor [Z: DMA_AXI_DESCR_NEXT_STOP]; descriptors
 * are therefore word aligned.
 */
#define DESC_NEXT_STOP 0x00000001U
#define DESC_NEXT(d) ((uint32_t)(uintptr_t)(d))

/* length: the byte count and the same flags as the FETCHLEN/PUSHLEN registers [RM 11.4.2.2,
 * 11.4.2.5; Z: DMA_AXI_DESCR_CONST_ADDR/REALIGN/DISCARD = BLOCK_S_*].
 */
#define DESC_LEN_REALIGN AES_FETCHLEN_REALIGN
#define DESC_LEN_DISCARD AES_PUSHLEN_DISCARD
#define DESC_LEN_MAX     _AES_FETCHLEN_LENGTH_MASK

/* tag [Z: DMA_SG_ENGINESELECT_*, DMA_SG_TAG_*]. */
#define TAG_ENGINE_BA411E         0x00000001U /* data flow through the BA411E AES engine */
#define TAG_IS_DATA               0x00000000U
#define TAG_IS_CONFIG             0x00000010U /* to the engine's parameters at an offset */
#define TAG_IS_LAST               0x00000020U /* last data of the operation */
#define TAG_DATA_PAYLOAD          0x00000000U /* encrypted/decrypted and authenticated */
#define TAG_DATA_HEADER           0x00000040U /* authenticated only (CCM associated data) */
#define TAG_CFG_OFFSET(off)       (((uint32_t)(off) & 0xFFU) << 8)
#define TAG_INVALID_BYTES(nbytes) (((uint32_t)(nbytes) & 0x1FU) << 8)

/* BA411E parameter offsets and configuration (mode) word [Z: AES_OFFSET_*, AES_MODEID_*]. */
#define BA411E_OFFSET_CFG 0U
#define BA411E_OFFSET_KEY 8U

#define BA411E_MODE_ECB     0x00000100U
#define BA411E_MODE_CCM     0x00002000U
#define BA411E_MODE_CMAC    0x00010000U
#define BA411E_MODE_NO_CX   0x00000000U /* no context load or save */
#define BA411E_MODE_AES128  0x00000000U
#define BA411E_MODE_AES256  0x00000004U
#define BA411E_MODE_ENCRYPT 0x00000000U
#define BA411E_MODE_DECRYPT 0x00000001U

#define AES128_KEY_SIZE 16U
#define AES256_KEY_SIZE 32U

/* CCM [SP 800-38C A.2.1, A.2.2]: B0 is flags || nonce || Q; with a 13-byte nonce, Q (the payload
 * length) has q = 15 - 13 = 2 bytes; associated data shorter than 2^16 - 2^8 is prefixed by its
 * length in 2 bytes.
 */
#define CCM_B0_FLAG_ADATA     0x40U
#define CCM_B0_TAG_SHIFT      3U
#define CCM_Q_SIZE            (15U - EFR32_RADIOAES_CCM_NONCE_SIZE)
#define CCM_AAD_LEN_SIZE      2U
#define CCM_AAD_MAX           0xFEFFU
#define CCM_PAYLOAD_MAX       0xFFFFU
#define CCM_TAG_MIN           4U
#define CCM_TAG_MAX           EFR32_RADIOAES_BLOCK_SIZE

#define STATUS_BUSY (AES_STATUS_FETCHERBSY | AES_STATUS_PUSHERBSY | AES_STATUS_SOFTRSTBSY)
#define IF_ERROR    (AES_IF_FETCHERERROR | AES_IF_PUSHERERROR)

static int key_mode(size_t key_len, uint32_t *mode)
{
	/* The BA411E instance takes 128- and 256-bit keys only [Z: keybits 192 returns
	 * SL_STATUS_NOT_SUPPORTED in every mode].
	 */
	switch (key_len) {
	case AES128_KEY_SIZE:
		*mode = BA411E_MODE_AES128;
		return 0;
	case AES256_KEY_SIZE:
		*mode = BA411E_MODE_AES256;
		return 0;
	default:
		return -EINVAL;
	}
}

static void desc_config(struct radioaes_desc *d, const void *src, size_t len, uint32_t offset,
			const struct radioaes_desc *next)
{
	d->address = (uint32_t)(uintptr_t)src;
	d->next = DESC_NEXT(next);
	d->length = len | DESC_LEN_REALIGN;
	d->tag = TAG_ENGINE_BA411E | TAG_IS_CONFIG | TAG_CFG_OFFSET(offset);
}

#if defined(CONFIG_EFR32_RADIOAES_PROBE)
/* Test probe: CPU cycles (DWT) from the start command to the DMA idle, last operation. */
uint32_t efr32_radioaes_probe_cycles;
#endif

static int radioaes_run(const struct radioaes_desc *fetch, const struct radioaes_desc *push)
{
	uint32_t saved_fetch;
	uint32_t saved_push;
	uint32_t errors;

	/* (1) Finish a preempted caller's operation [RM 11.4.2.11: FETCHERBSY/PUSHERBSY are high
	 * as long as the fetcher/pusher is busy; Z: sli_radioaes_acquire() waits the same way
	 * before a preempting operation].
	 */
	while (RADIOAES->STATUS & STATUS_BUSY) {
	}
	/* (2) [Z: sli_radioaes_save_state()]. */
	saved_fetch = RADIOAES->FETCHADDR;
	saved_push = RADIOAES->PUSHADDR;

	/* (3) Descriptors and inputs are ordinary stores; complete them before the DMA starts. */
	__DSB();
	RADIOAES->CTRL = AES_CTRL_FETCHERSCATTERGATHER | AES_CTRL_PUSHERSCATTERGATHER;
	RADIOAES->FETCHADDR = (uint32_t)(uintptr_t)fetch;
	RADIOAES->PUSHADDR = (uint32_t)(uintptr_t)push;
#if defined(CONFIG_EFR32_RADIOAES_PROBE)
	uint32_t probe_start = DWT->CYCCNT;
#endif
	RADIOAES->CMD = AES_CMD_STARTPUSHER | AES_CMD_STARTFETCHER;
	while (RADIOAES->STATUS & (AES_STATUS_FETCHERBSY | AES_STATUS_PUSHERBSY)) {
	}
#if defined(CONFIG_EFR32_RADIOAES_PROBE)
	efr32_radioaes_probe_cycles = DWT->CYCCNT - probe_start;
#endif
	/* The pusher has written the outputs; order their reads after the busy flag. */
	__DSB();

	/* A bus error on either side [RM 11.4.2.7: "Triggered when an error response is received"]
	 * means a descriptor named an address the DMA may not access: a defect, reported to
	 * whichever caller sees it.
	 */
	errors = RADIOAES->IF & IF_ERROR;
	if (errors != 0U) {
		RADIOAES->IF_CLR = errors;
	}

	/* (4) [Z: sli_radioaes_restore_state()]. */
	RADIOAES->FETCHADDR = saved_fetch;
	RADIOAES->PUSHADDR = saved_push;

	return errors != 0U ? -EIO : 0;
}

int efr32_radioaes_ecb(bool encrypt, const uint8_t *key, size_t key_len, const uint8_t *in,
		       uint8_t *out, size_t len)
{
	struct radioaes_desc f_key, f_cfg, f_data, p_data;
	uint32_t config;
	int err;

	if (len == 0U || (len % EFR32_RADIOAES_BLOCK_SIZE) != 0U || len > DESC_LEN_MAX) {
		return -EINVAL;
	}
	err = key_mode(key_len, &config);
	if (err != 0) {
		return err;
	}
	config |= BA411E_MODE_ECB | BA411E_MODE_NO_CX |
		  (encrypt ? BA411E_MODE_ENCRYPT : BA411E_MODE_DECRYPT);

	/* [Z: sli_aes_crypt_ecb_radio()]: key, configuration, data in; data out. */
	desc_config(&f_key, key, key_len, BA411E_OFFSET_KEY, &f_cfg);
	desc_config(&f_cfg, &config, sizeof(config), BA411E_OFFSET_CFG, &f_data);
	f_data.address = (uint32_t)(uintptr_t)in;
	f_data.next = DESC_NEXT_STOP;
	f_data.length = len;
	f_data.tag = TAG_ENGINE_BA411E | TAG_IS_DATA | TAG_IS_LAST | TAG_DATA_PAYLOAD;

	p_data.address = (uint32_t)(uintptr_t)out;
	p_data.next = DESC_NEXT_STOP;
	p_data.length = len;
	p_data.tag = TAG_ENGINE_BA411E | TAG_IS_LAST;

	return radioaes_run(&f_key, &p_data);
}

int efr32_radioaes_cmac(const uint8_t *key, size_t key_len, const uint8_t *in, size_t len,
			uint8_t mac[EFR32_RADIOAES_BLOCK_SIZE])
{
	struct radioaes_desc f_key, f_cfg, f_body, f_last, p_mac;
	uint8_t last[EFR32_RADIOAES_BLOCK_SIZE];
	size_t tail;
	size_t body;
	uint32_t config;
	int err;

	if (len > DESC_LEN_MAX) {
		return -EINVAL;
	}
	err = key_mode(key_len, &config);
	if (err != 0) {
		return err;
	}
	config |= BA411E_MODE_CMAC | BA411E_MODE_NO_CX | BA411E_MODE_ENCRYPT;

	/* [Z: sli_aes_cmac_radio()] feeds the message rounded up to whole blocks with the number
	 * of padding bytes in the last descriptor's tag (all 16 for an empty message), and the
	 * engine applies SP 800-38B's padding and subkey choice. The final block is copied here so
	 * the DMA never reads past the caller's buffer.
	 */
	tail = len % EFR32_RADIOAES_BLOCK_SIZE;
	if (tail == 0U && len > 0U) {
		tail = EFR32_RADIOAES_BLOCK_SIZE;
	}
	body = len - tail;
	memset(last, 0, sizeof(last));
	if (tail > 0U) {
		memcpy(last, in + body, tail);
	}

	desc_config(&f_key, key, key_len, BA411E_OFFSET_KEY, &f_cfg);
	desc_config(&f_cfg, &config, sizeof(config), BA411E_OFFSET_CFG,
		    body > 0U ? &f_body : &f_last);
	f_body.address = (uint32_t)(uintptr_t)in;
	f_body.next = DESC_NEXT(&f_last);
	f_body.length = body;
	f_body.tag = TAG_ENGINE_BA411E | TAG_IS_DATA | TAG_DATA_PAYLOAD;
	f_last.address = (uint32_t)(uintptr_t)last;
	f_last.next = DESC_NEXT_STOP;
	f_last.length = sizeof(last);
	f_last.tag = TAG_ENGINE_BA411E | TAG_IS_DATA | TAG_IS_LAST | TAG_DATA_PAYLOAD |
		     TAG_INVALID_BYTES(EFR32_RADIOAES_BLOCK_SIZE - tail);

	p_mac.address = (uint32_t)(uintptr_t)mac;
	p_mac.next = DESC_NEXT_STOP;
	p_mac.length = EFR32_RADIOAES_BLOCK_SIZE;
	p_mac.tag = TAG_ENGINE_BA411E | TAG_IS_LAST;

	return radioaes_run(&f_key, &p_mac);
}

int efr32_radioaes_ccm(bool encrypt, const uint8_t *key, size_t key_len, const uint8_t *nonce,
		       size_t nonce_len, const uint8_t *aad, size_t aad_len, const uint8_t *in,
		       uint8_t *out, size_t len, uint8_t *tag, size_t tag_len)
{
	/* Fetch: configuration, key, B0 (|| AAD length), AAD, payload, received tag (decrypt).
	 * Push: discard B0 and AAD (reflected), payload, discard its padding, tag or tag
	 * difference, discard the rest of that block. [Z: aes_ccm_radio()]
	 */
	struct radioaes_desc f_cfg, f_key, f_hdr, f_aad, f_data, f_tag;
	struct radioaes_desc p_hdr, p_data, p_pad, p_tag, p_tag_pad;
	uint8_t header[EFR32_RADIOAES_BLOCK_SIZE + CCM_AAD_LEN_SIZE];
	/* Decrypt: the engine pushes the difference between the computed and the received tag,
	 * all zero when they match [Z: ver_failed]. Pre-set to non-zero so a short push cannot
	 * verify.
	 */
	uint8_t check[EFR32_RADIOAES_BLOCK_SIZE];
	size_t header_len;
	size_t header_pad;
	size_t data_pad;
	uint32_t config;
	uint8_t diff;
	int err;

	/* An empty payload is not implemented: [Z] feeds one all-padding block for it, and on
	 * silicon that gives a tag that is not SP 800-38C's (docs/crypto.md; Bluetooth never
	 * encrypts an empty PDU, hal/radio.c).
	 */
	if (nonce_len != EFR32_RADIOAES_CCM_NONCE_SIZE || aad_len > CCM_AAD_MAX || len == 0U ||
	    len > CCM_PAYLOAD_MAX || tag_len < CCM_TAG_MIN || tag_len > CCM_TAG_MAX ||
	    (tag_len % 2U) != 0U) {
		return -EINVAL;
	}
	err = key_mode(key_len, &config);
	if (err != 0) {
		return err;
	}
	config |= BA411E_MODE_CCM | BA411E_MODE_NO_CX |
		  (encrypt ? BA411E_MODE_ENCRYPT : BA411E_MODE_DECRYPT);

	header[0] = (aad_len > 0U ? CCM_B0_FLAG_ADATA : 0U) |
		    (uint8_t)(((tag_len - 2U) / 2U) << CCM_B0_TAG_SHIFT) | (uint8_t)(CCM_Q_SIZE - 1U);
	memcpy(&header[1], nonce, EFR32_RADIOAES_CCM_NONCE_SIZE);
	header[1 + EFR32_RADIOAES_CCM_NONCE_SIZE] = (uint8_t)(len >> 8);
	header[2 + EFR32_RADIOAES_CCM_NONCE_SIZE] = (uint8_t)len;
	header_len = EFR32_RADIOAES_BLOCK_SIZE;
	if (aad_len > 0U) {
		header[EFR32_RADIOAES_BLOCK_SIZE] = (uint8_t)(aad_len >> 8);
		header[EFR32_RADIOAES_BLOCK_SIZE + 1U] = (uint8_t)aad_len;
		header_len += CCM_AAD_LEN_SIZE;
	}
	header_pad = (EFR32_RADIOAES_BLOCK_SIZE - ((header_len + aad_len) % EFR32_RADIOAES_BLOCK_SIZE)) %
		     EFR32_RADIOAES_BLOCK_SIZE;
	data_pad = (EFR32_RADIOAES_BLOCK_SIZE - (len % EFR32_RADIOAES_BLOCK_SIZE)) %
		   EFR32_RADIOAES_BLOCK_SIZE;
	memset(check, 0xFF, sizeof(check));

	/* Fetcher. */
	desc_config(&f_cfg, &config, sizeof(config), BA411E_OFFSET_CFG, &f_key);
	desc_config(&f_key, key, key_len, BA411E_OFFSET_KEY, &f_hdr);

	f_hdr.address = (uint32_t)(uintptr_t)header;
	f_hdr.next = DESC_NEXT(aad_len > 0U ? &f_aad : &f_data);
	f_hdr.length = header_len | (aad_len > 0U ? 0U : DESC_LEN_REALIGN);
	f_hdr.tag = TAG_ENGINE_BA411E | TAG_IS_DATA | TAG_DATA_HEADER |
		    (aad_len > 0U ? 0U : TAG_INVALID_BYTES(header_pad));

	f_aad.address = (uint32_t)(uintptr_t)aad;
	f_aad.next = DESC_NEXT(&f_data);
	f_aad.length = aad_len | DESC_LEN_REALIGN;
	f_aad.tag = TAG_ENGINE_BA411E | TAG_IS_DATA | TAG_DATA_HEADER | TAG_INVALID_BYTES(header_pad);

	f_data.address = (uint32_t)(uintptr_t)in;
	f_data.next = encrypt ? DESC_NEXT_STOP : DESC_NEXT(&f_tag);
	f_data.length = len | DESC_LEN_REALIGN;
	f_data.tag = TAG_ENGINE_BA411E | TAG_IS_DATA | TAG_DATA_PAYLOAD |
		     (encrypt ? TAG_IS_LAST : 0U) | TAG_INVALID_BYTES(data_pad);

	f_tag.address = (uint32_t)(uintptr_t)tag;
	f_tag.next = DESC_NEXT_STOP;
	f_tag.length = tag_len | DESC_LEN_REALIGN;
	f_tag.tag = TAG_ENGINE_BA411E | TAG_IS_DATA | TAG_IS_LAST | TAG_DATA_PAYLOAD |
		    TAG_INVALID_BYTES(EFR32_RADIOAES_BLOCK_SIZE - tag_len);

	/* Pusher. */
	p_tag_pad.address = 0U;
	p_tag_pad.next = DESC_NEXT_STOP;
	p_tag_pad.length = (EFR32_RADIOAES_BLOCK_SIZE - tag_len) | DESC_LEN_DISCARD;
	p_tag_pad.tag = 0U;

	p_tag.address = (uint32_t)(uintptr_t)(encrypt ? tag : check);
	p_tag.next = tag_len < EFR32_RADIOAES_BLOCK_SIZE ? DESC_NEXT(&p_tag_pad) : DESC_NEXT_STOP;
	p_tag.length = tag_len;
	p_tag.tag = 0U;

	p_pad.address = 0U;
	p_pad.next = DESC_NEXT(&p_tag);
	p_pad.length = data_pad | DESC_LEN_DISCARD;
	p_pad.tag = 0U;

	p_data.address = (uint32_t)(uintptr_t)out;
	p_data.next = data_pad > 0U ? DESC_NEXT(&p_pad) : DESC_NEXT(&p_tag);
	p_data.length = len;
	p_data.tag = 0U;

	p_hdr.address = 0U;
	p_hdr.next = DESC_NEXT(&p_data);
	p_hdr.length = (header_len + aad_len + header_pad) | DESC_LEN_DISCARD;
	p_hdr.tag = 0U;

	err = radioaes_run(&f_cfg, &p_hdr);
	if (err != 0) {
		memset(out, 0, len);
		return err;
	}

	if (!encrypt) {
		diff = 0U;
		for (size_t i = 0; i < tag_len; i++) {
			diff |= check[i];
		}
		if (diff != 0U) {
			memset(out, 0, len);
			return -EBADMSG;
		}
	}

	return 0;
}

/* Bus clock and radio bridge clock [RM 8.5 CMU_CLKEN0.RADIOAES, 8.5.25 CMU_RADIOCLKCTRL.EN;
 * Z: sli_radioaes_acquire() sets both]. Both stay on: switching them per operation would race
 * a preempted caller's operation.
 */
static int radioaes_init(void)
{
	CMU->CLKEN0_SET = CMU_CLKEN0_RADIOAES;
	CMU->RADIOCLKCTRL_SET = CMU_RADIOCLKCTRL_EN;

	return 0;
}

SYS_INIT(radioaes_init, PRE_KERNEL_1, 0);
