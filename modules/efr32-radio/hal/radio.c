/*
 * Radio HAL of the EFR32BG22 lower link layer: the API contract of Zephyr's OpenISA LLL
 * (subsys/bluetooth/controller/ll_sw/openisa/hal/RV32M1/radio/radio.h, 78 functions).
 * SPDX-License-Identifier: Apache-2.0
 *
 * *** UNVALIDATED, UNFINISHED: public generated register layouts are now known,
 * but the required radio/sequencer behavior is not fully characterized. ***
 * Every function below is classified by what it is blocked on:
 *   software      needs no register access, simply not written yet
 *   regs-*        needs register knowledge listed in docs/radio-tables.md (T1..T9)
 * Until those exist the functions only keep software state; anything that would put a packet on the
 * air asserts through EFR32_RADIO_TODO() instead of writing guessed registers.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <soc.h>
#include <psa/crypto.h>

#include "hal/ccm.h"
#include "hal/cntr.h"
#include "hal/efr32/cntr_efr32.h"
#include "hal/efr32/ticker.h"
#include "abort_lifecycle.h"
#include "completion_dispatch.h"
#include "hal/efr32/ll_irqs.h"
#include "hal/radio.h"
#include "hal/debug.h"
#include "packet_stage_lifecycle.h"
#include "rx_pdu_delivery.h"
#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
#include "rx_descriptor.h"
#endif
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
#include "qemu_tifs_state.h"
#endif
#include "timer_state.h"

#include "util/mem.h"
#include "common/rpa.h"

#include "lll/pdu_vendor.h"
#include "ll_sw/pdu.h"

/* Marks a function that cannot be implemented without the radio register map: logs once. */
#define EFR32_RADIO_TODO(tag) \
	do { \
		static bool efr32_once_; \
		if (!efr32_once_) { \
			efr32_once_ = true; \
			printk("efr32-radio: %s needs %s (docs/radio-tables.md)\n", __func__, tag); \
		} \
	} while (0)

/* Same, for the functions that would start air activity: refuse to run on stubs. */
#define EFR32_RADIO_AIR_TODO(tag) \
	do { \
		EFR32_RADIO_TODO(tag); \
		LL_ASSERT_MSG(!IS_ENABLED(CONFIG_EFR32_RADIO_STUB_REGISTERS), \
			      "radio register map unknown"); \
	} while (0)

/*
 * OpenISA's abort paths install a cleanup callback and then call
 * radio_disable().  The RV32M1 HAL completes that sequence with a timer IRQ;
 * RAC's RXDIS/TXDIS commands do not produce either FRC completion latch, so
 * retain an explicit, non-PDU completion reason for the same controller
 * lifecycle.
 */
static struct efr32_radio_abort_lifecycle efr32_radio_abort;

#if defined(CONFIG_BT_CTLR_PRIVACY)
#define EFR32_RADIO_AR_NO_MATCH 0xffU
/*
 * ull_filter_lll_irks_get() supplies AAR-order (big-endian) IRKs. Retain an
 * LE snapshot because bt_rpa_irk_matches() implements BLE ah in that order,
 * and because the caller does not promise the source array's lifetime.
 */
static struct {
	bool armed;
	uint8_t count;
	uint8_t match;
	uint8_t irks[CONFIG_BT_CTLR_RL_SIZE][16];
} efr32_radio_ar = {
	.match = EFR32_RADIO_AR_NO_MATCH,
};

/*
 * This is deliberately reached only from the normalized-PDU delivery seam.
 * In particular, raw BUFC channel-1 bytes and descriptor data are not valid
 * inputs: their framing and appended-info policy remain unproven.
 */
static void efr32_radio_ar_execute(const uint8_t *pdu, uint16_t bytes)
{
	const struct pdu_adv *adv = (const struct pdu_adv *)pdu;
	bt_addr_t rpa;
	uint8_t i;

	if (!efr32_radio_ar.armed) {
		return;
	}

	efr32_radio_ar.armed = false;
	efr32_radio_ar.match = EFR32_RADIO_AR_NO_MATCH;
	/* Two PDU header octets plus the advertiser address are required. */
	if (bytes < 8U || adv->len < sizeof(rpa.val) ||
	    bytes < (uint16_t)(PDU_AC_LL_HEADER_SIZE + adv->len) || !adv->tx_addr) {
		return;
	}

	memcpy(rpa.val, adv->payload, sizeof(rpa.val));
	if (!BT_ADDR_IS_RPA(&rpa)) {
		return;
	}

	for (i = 0U; i < efr32_radio_ar.count; i++) {
		if (bt_rpa_irk_matches(efr32_radio_ar.irks[i], &rpa)) {
			efr32_radio_ar.match = i;
			break;
		}
	}
}
#endif /* CONFIG_BT_CTLR_PRIVACY */
/*
 * This state is intentionally distinct from PROTIMER.  It mirrors only
 * OpenISA software bookkeeping; it never makes a timer expire or starts a
 * radio operation.  See timer_state.c for the reference line citations.
 */
static struct efr32_radio_timer_state efr32_radio_timer;
static atomic_t efr32_radio_launch_generation;
static atomic_t efr32_radio_launch_active;
/* Generation mailboxes pair the coalescing event latch with its expiry. */
static atomic_t efr32_radio_tx_pending_generation;
static atomic_t efr32_radio_rx_pending_generation;
K_DEADLINE_EVENT_DEFINE(efr32_radio_tx_event);
K_DEADLINE_EVENT_DEFINE(efr32_radio_rx_event);
K_DEADLINE_EVENT_DEFINE(efr32_radio_completion_event);

/* The FRC vector publishes this mailbox; only the persistent job calls LLL. */
static struct {
	radio_isr_cb_t callback;
	void *param;
	bool abort;
} efr32_radio_completion_work;
static struct efr32_radio_completion_dispatch efr32_radio_completion_dispatch;

#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
/*
 * This is a simulator-only replacement for the unproven PROTIMER/RAC TIFS
 * sequencer.  The timer callback only wakes this pre-created job; all RAC
 * commands and timeout handling remain scheduler-owned.
 */
static struct efr32_qemu_tifs_state efr32_qemu_tifs;
/* The LLL records the action after the operation it is now preparing. */
static enum efr32_qemu_tifs_next_action efr32_qemu_tifs_next_action;
static bool efr32_qemu_tifs_transition_requested;
/* RXDIS has claimed a recorded turn-around; wait for the callback to stage TX. */
static bool efr32_qemu_tifs_rx_to_tx_staging;
/* Coalesces an IRQ publication while the persistent TIFS job is running. */
K_DEADLINE_EVENT_DEFINE(efr32_qemu_tifs_event);
K_MUTEX_DEFINE(efr32_qemu_tifs_op_lock);

static uint32_t efr32_qemu_tifs_deadline(uint32_t delay_us)
{
	/* cntr_init() selects the QEMU scheduler's 32.768 kHz BURTC clock. */
	uint32_t ticks = (uint32_t)(((uint64_t)delay_us * 32768U + 999999U) /
				      1000000U);

	return cntr_cnt_get() + (ticks ? ticks : 1U);
}

static void efr32_qemu_tifs_schedule(uint32_t delay_us, uint32_t generation)
{
	cntr_qemu_tifs_cmp_set(efr32_qemu_tifs_deadline(delay_us), generation);
}

static void efr32_qemu_tifs_cancel(void)
{
	/* Serialize cancellation with the worker's final claim and RXEN side effect. */
	k_mutex_lock(&efr32_qemu_tifs_op_lock, K_FOREVER);
	unsigned int key = irq_lock();

	efr32_qemu_tifs_state_cancel(&efr32_qemu_tifs);
	efr32_qemu_tifs_rx_to_tx_staging = false;
	irq_unlock(key);

	/* A stale BURTC wake is harmless: the worker rechecks phase/generation. */
	cntr_qemu_tifs_cmp_cancel();
	k_mutex_unlock(&efr32_qemu_tifs_op_lock);
}

static void efr32_qemu_tifs_arm(uint32_t hcto_us)
{
	unsigned int key;
	uint32_t generation;
	bool stale;

	/* Stop before publishing the new generation, then verify after arm. */
	cntr_qemu_tifs_cmp_cancel();
	key = irq_lock();
	generation = efr32_qemu_tifs_state_arm(&efr32_qemu_tifs, hcto_us);
	irq_unlock(key);

	efr32_qemu_tifs_schedule(efr32_radio_timer.tifs_us, generation);

	/* If reset/teardown raced the start, do not resurrect the cancelled job. */
	key = irq_lock();
	stale = efr32_qemu_tifs.phase != EFR32_QEMU_TIFS_WAIT_RX ||
		efr32_qemu_tifs.generation != generation;
	irq_unlock(key);
	if (stale) {
		cntr_qemu_tifs_cmp_cancel();
	}
}

static void efr32_qemu_tifs_begin_rx_to_tx(void);
static void efr32_qemu_tifs_arm_staged_rx_to_tx(void);
static bool efr32_qemu_tifs_tx_staged(void);
static void efr32_qemu_tifs_start_staged_tx(void);

static void efr32_qemu_tifs_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		enum efr32_qemu_tifs_phase phase;
		uint32_t generation;
		uint32_t hcto_us;
		unsigned int key;

		/* Atomically consume a TIFS publication or become its idle consumer. */
		k_deadline_event_wait(&efr32_qemu_tifs_event);
		key = irq_lock();
		phase = efr32_qemu_tifs.phase;
		generation = efr32_qemu_tifs.generation;
		hcto_us = efr32_qemu_tifs.hcto_us;
		irq_unlock(key);

		if (phase == EFR32_QEMU_TIFS_WAIT_RX) {
			/*
			 * Re-check at the final cancellation point before RXEN.  A reset,
			 * disable, or timer stop may have invalidated this dequeued work
			 * while the worker was waiting to run.
			 */
			/* Cancellation cannot interleave between claim and RXEN. */
			k_mutex_lock(&efr32_qemu_tifs_op_lock, K_FOREVER);
			key = irq_lock();
			bool claimed = efr32_qemu_tifs_state_claim_rx(
				&efr32_qemu_tifs, generation);
			irq_unlock(key);
			if (!claimed) {
				k_mutex_unlock(&efr32_qemu_tifs_op_lock);
				continue;
			}

			/* radio_pkt_rx_set() was completed before HCTO was configured. */
			if (radio_is_idle()) {
				radio_rx_enable();
			} else {
				/* Do not hold the cancellation lock while waiting on RAC. */
				key = irq_lock();
				efr32_qemu_tifs_state_cancel(&efr32_qemu_tifs);
				irq_unlock(key);
			}
			k_mutex_unlock(&efr32_qemu_tifs_op_lock);

			/* Arm the same persistent job for the bounded no-response path. */
			key = irq_lock();
			if (efr32_qemu_tifs.phase == EFR32_QEMU_TIFS_WAIT_HCTO &&
			    efr32_qemu_tifs.generation == generation) {
				irq_unlock(key);
				efr32_qemu_tifs_schedule(hcto_us, generation);
			} else {
				irq_unlock(key);
			}
		} else if (phase == EFR32_QEMU_TIFS_WAIT_TX) {
			/* The LLL callback must have staged a fresh response by TIFS. */
			k_mutex_lock(&efr32_qemu_tifs_op_lock, K_FOREVER);
			key = irq_lock();
			bool claimed = efr32_qemu_tifs_state_claim_tx(
				&efr32_qemu_tifs, generation);
			irq_unlock(key);
			if (claimed && radio_is_idle() && efr32_qemu_tifs_tx_staged()) {
				efr32_qemu_tifs_start_staged_tx();
			}
			k_mutex_unlock(&efr32_qemu_tifs_op_lock);
		} else if (phase == EFR32_QEMU_TIFS_WAIT_HCTO) {
			key = irq_lock();
			if (efr32_qemu_tifs_state_begin_timeout(&efr32_qemu_tifs,
							 generation)) {
				irq_unlock(key);
				/* A completed response owns cleanup; only abort silence. */
				if (!radio_is_done()) {
					radio_disable();
				}
			} else {
				irq_unlock(key);
			}
		}
	}
}

K_THREAD_DEFINE(efr32_qemu_tifs_job_thread,
		CONFIG_EFR32_RADIO_COMPLETION_STACK_SIZE,
		efr32_qemu_tifs_job, NULL, NULL, NULL,
		CONFIG_EFR32_RADIO_SCHEDULER_PRIO, 0, 0);
#endif

/* Called from the shared BURTC CC0 alarm: publish work only, never RAC MMIO. */
void efr32_radio_qemu_tifs_wake(uint32_t generation)
{
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	bool valid;
	unsigned int key = irq_lock();

	valid = efr32_qemu_tifs.phase != EFR32_QEMU_TIFS_IDLE &&
		efr32_qemu_tifs.generation == generation;
	irq_unlock(key);
	if (valid) {
		/*
		 * BURTC expiry is this job's release event. K_DEADLINE(0) gives the
		 * now-eligible worker an immediate start-by key at its static priority;
		 * it does not advance the RXEN/TXEN action before the compare expiry.
		 */
		k_deadline_event_post(&efr32_qemu_tifs_event, K_DEADLINE(0));
	}
#else
	ARG_UNUSED(generation);
#endif
}

static void efr32_radio_completion_job(void *a, void *b, void *c);

#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
static void efr32_radio_qemu_rx_descriptor_isr(void *arg);
static void efr32_radio_qemu_rx_descriptor_quiesce(void);
static void efr32_radio_qemu_rx_descriptor_setup(void);
static inline bool efr32_qemu_rx_descriptor_pending(void);
K_SEM_DEFINE(efr32_qemu_rx_descriptor_sem, 0, 1);
static struct {
	uint16_t descriptor_read_offset;
	bool descriptor_crc_valid;
} efr32_qemu_rx_descriptor;
#endif

static void efr32_radio_tx_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		atomic_val_t generation;

		k_deadline_event_wait(&efr32_radio_tx_event);
		generation = atomic_get(&efr32_radio_tx_pending_generation);
		if (atomic_cas(&efr32_radio_launch_active,
			    generation, 0)) {
			radio_tx_enable();
		}
	}
}

static void efr32_radio_rx_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		atomic_val_t generation;

		k_deadline_event_wait(&efr32_radio_rx_event);
		generation = atomic_get(&efr32_radio_rx_pending_generation);
		if (atomic_cas(&efr32_radio_launch_active,
			    generation, 0)) {
			radio_rx_enable();
		}
	}
}

K_THREAD_DEFINE(efr32_radio_tx_job_thread,
		CONFIG_EFR32_RADIO_LAUNCH_STACK_SIZE,
		efr32_radio_tx_job, NULL, NULL, NULL,
		CONFIG_EFR32_RADIO_SCHEDULER_PRIO, 0, 0);
K_THREAD_DEFINE(efr32_radio_rx_job_thread,
		CONFIG_EFR32_RADIO_LAUNCH_STACK_SIZE,
		efr32_radio_rx_job, NULL, NULL, NULL,
		CONFIG_EFR32_RADIO_SCHEDULER_PRIO, 0, 0);
K_THREAD_DEFINE(efr32_radio_completion_job_thread,
		CONFIG_EFR32_RADIO_COMPLETION_STACK_SIZE,
		efr32_radio_completion_job, NULL, NULL, NULL,
		CONFIG_EFR32_RADIO_SCHEDULER_PRIO, 0, 0);

/* ISR-safe: publish only a generation and wake the pre-created job. */
void efr32_radio_launch_wake(uint8_t trx, uint32_t generation)
{
	if ((uint32_t)atomic_get(&efr32_radio_launch_active) != generation) {
		return;
	}
	if (trx) {
		atomic_set(&efr32_radio_tx_pending_generation, generation);
		/* Compare expiry is the release event; do not defer eligible launch work. */
		k_deadline_event_post(&efr32_radio_tx_event, K_DEADLINE(0));
	} else {
		atomic_set(&efr32_radio_rx_pending_generation, generation);
		/* The EDF key orders equal-priority ready jobs; it cannot start early. */
		k_deadline_event_post(&efr32_radio_rx_event, K_DEADLINE(0));
	}
}

/* Teardown never owns the persistent job threads. It only makes any queued
 * mailbox wake stale and releases the shared BURTC deadline. */
static void efr32_radio_launch_disarm(void)
{
	atomic_set(&efr32_radio_launch_active, 0);
	cntr_radio_cmp_cancel();
}
/*
 * OpenISA uses sleep/wake as a reference-counted clock-availability
 * contract.  xG22 radio power sequencing has not been characterized, so
 * this intentionally represents immediate software availability only; it is
 * not a claim that RAC idle, an EN bit, or any other MMIO state is powered
 * down.  radio_setup() establishes the initial caller-held reference.
 */
static atomic_t efr32_radio_power_ref = ATOMIC_INIT(1);
static uint8_t MALIGN(4) pkt_empty[PDU_EM_LL_SIZE_MAX];
/* Largest data PDU the controller can be configured to receive, plus MIC; or an advertising PDU. */
static uint8_t MALIGN(4) pkt_scratch[MAX(PDU_DC_LL_HEADER_SIZE + PDU_DC_PAYLOAD_SIZE_MAX + 4,
					 PDU_AC_LL_SIZE_MAX)];
/*
 * RAIL encodes a 512-byte BUFC TX FIFO as control value three. Staging keeps
 * that declared allocation real even when OpenISA hands us a short PDU.
 */
static uint8_t MALIGN(4) efr32_tx_fifo[512];
/* RAIL configures the RX BUFC channel as a 512-byte power-of-two FIFO too. */
static uint8_t MALIGN(4) efr32_rx_fifo[512];
#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
/*
 * The synthetic descriptor producer is a QEMU-only test seam.  Keep its
 * backing ring in storage owned by this image rather than reusing the stock
 * firmware's observed address, which may overlap unrelated Zephyr RAM.
 */
static uint8_t MALIGN(4) efr32_qemu_rx_descriptor_ring_storage[
	EFR32_RADIO_QEMU_RX_DESCRIPTOR_RING_BYTES];
#endif

/*
 * Packet buffers have two distinct ownership domains on this part:
 *
 * - the controller owns the PDU passed to radio_pkt_{rx,tx}_set(); and
 * - BUFC owns efr32_{rx,tx}_fifo while the peripheral is active.
 *
 * Keep the relationship explicit now, before a later FRC handoff teaches us
 * how to transfer an RX frame out of BUFC.  In particular, RX must not be
 * copied speculatively: the FRC framing and BUFC read-pointer semantics are
 * not yet evidenced.  The stored RX target is the eventual completion target,
 * not a claim that BUFC already writes directly to it.
 */
static struct {
	void *rx_target;
	const void *tx_source;
	uint16_t tx_bytes;
	/*
	 * The OpenISA LLL supplies these immediately before packet setup.  They
	 * describe the on-air PDU, not the number of bytes currently in BUFC:
	 * FRC's FIFO framing is still unknown.  Retain them so an eventual RX
	 * completion path can reject an impossible length before it copies data.
	 */
	uint8_t bits_len;
	uint8_t max_len;
	uint8_t flags;
	bool packet_configured;
	bool rx_fifo_configured;
	bool tx_fifo_configured;
	struct efr32_radio_rx_pdu_delivery rx_delivery;
	struct efr32_radio_packet_lifecycle lifecycle;
} efr32_packet_stage;

/*
 * A future normalized-PDU producer can reject a completed frame before the
 * LLL callback.  This is deliberately separate from FRC.TRAILRXDATA.CRCOK:
 * it represents a software delivery failure, not a second hardware CRC.
 */
static bool efr32_rx_delivery_invalid;

static void efr32_packet_stage_reset(void)
{
	memset(&efr32_packet_stage, 0, sizeof(efr32_packet_stage));
	efr32_packet_lifecycle_reset(&efr32_packet_stage.lifecycle);
}

#define BLE_CCM_MIC_SIZE 4U
#define BLE_CCM_NONCE_SIZE 13U
#define BLE_CCM_AAD_MASK 0xE3U

#define EFR32_SYNTH_BASE 0xB8018000UL
#define EFR32_SYNTH_CHCTRL_OFFSET 0x040U
#define EFR32_SYNTH_CHCTRL_CHNO_MASK 0x3FU

#define EFR32_RFCRC_BASE 0xB8010000UL
#define EFR32_RFCRC_EN_OFFSET 0x004U
#define EFR32_RFCRC_CTRL_OFFSET 0x008U
#define EFR32_RFCRC_INIT_OFFSET 0x018U
#define EFR32_RFCRC_POLY_OFFSET 0x020U
#define EFR32_RFCRC_BLE_CTRL 0x00000708U

#define EFR32_FRC_BASE 0xB8004000UL
#define EFR32_AGC_BASE 0xB800C000UL
#define EFR32_FRC_WHITEPOLY_OFFSET 0x034U
#define EFR32_FRC_WHITEINIT_OFFSET 0x038U
#define EFR32_FRC_RXCTRL_OFFSET 0x04CU
#define EFR32_FRC_TRAILRXDATA_OFFSET 0x054U
#define EFR32_FRC_IF_OFFSET 0x068U
#define EFR32_FRC_IEN_OFFSET 0x06CU
#define EFR32_FRC_BLE_WHITEPOLY 0x0044U
#define EFR32_FRC_BLE_WHITEINIT_MSB 0x0040U
#define EFR32_FRC_IF_TXDONE (1U << 0)
#define EFR32_FRC_IF_RXDONE (1U << 4)
#define EFR32_FRC_IF_COMPLETION (EFR32_FRC_IF_TXDONE | EFR32_FRC_IF_RXDONE)
#define EFR32_FRC_TRAILRXDATA_CRCOK (1U << 1)

#define EFR32_MODEM_BASE 0xB8014000UL
#define EFR32_MODEM_SYNC0_OFFSET 0x068U

#define EFR32_PROTIMER_BASE 0xB801C000UL

#define EFR32_AGC_FRAMERSSI_OFFSET 0x01CU
#define EFR32_AGC_IF_OFFSET 0x044U
#define EFR32_AGC_CMD_OFFSET 0x04CU
#define EFR32_AGC_CMD_RSSISTART (1U << 0)
#define EFR32_AGC_IF_RSSIVALID (1U << 0)
#define EFR32_AGC_RSSI_CODE_SHIFT 6U
#define EFR32_AGC_RSSI_CODE_MASK 0x3FFU
#define EFR32_AGC_RSSI_INVALID (-512)
#define EFR32_AGC_RSSI_OFFSET_QUARTER_DB 200

#define EFR32_RAC_BASE 0xB8020000UL
#define EFR32_RAC_SR0_OFFSET 0x040U
#define EFR32_RAC_SR1_OFFSET 0x044U
#define EFR32_RAC_SR2_OFFSET 0x048U
#define EFR32_RAC_SR0_INIT_CLEAR (1U << 6)
#define EFR32_RAC_RXENSRCEN_OFFSET 0x008U
#define EFR32_RAC_STATUS_OFFSET 0x00CU
#define EFR32_RAC_CMD_OFFSET 0x010U
#define EFR32_RAC_CTRL_OFFSET 0x014U
#define EFR32_REGISTER_SET_OFFSET 0x1000U
#define EFR32_REGISTER_CLEAR_OFFSET 0x2000U
#define EFR32_RAC_STATUS_STATE_SHIFT 24U
#define EFR32_RAC_STATUS_STATE_MASK 0xFU
#define EFR32_RAC_STATE_OFF 0U
#define EFR32_RAC_CMD_TXEN (1U << 0)
#define EFR32_RAC_CMD_TXDIS (1U << 5)
#define EFR32_RAC_CMD_RXDIS (1U << 8)
#define EFR32_RAC_RXENSRCEN_SWRXEN (1U << 1)
#define EFR32_RAC_CTRL_FORCEDISABLE (1U << 0)

#define EFR32_BUFC_BASE 0xBA000000UL
#define EFR32_BUFC_TX_BASE_OFFSET 0x00CU
#define EFR32_BUFC_RX_BASE_OFFSET 0x04CU
#define EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET 0x08CU
#define EFR32_BUFC_CFG_OFFSET 0x000U
#define EFR32_BUFC_ADDR_OFFSET 0x004U
#define EFR32_BUFC_WRITEOFFSET 0x008U
#define EFR32_BUFC_READOFFSET_OFFSET 0x00CU
#define EFR32_BUFC_WRITEDATA_OFFSET 0x018U
#define EFR32_BUFC_THRESHOLD_OFFSET 0x024U
#define EFR32_BUFC_CMD_OFFSET 0x028U
#define EFR32_BUFC_IF_OFFSET 0x114U
#define EFR32_BUFC_IEN_OFFSET 0x118U
#define EFR32_BUFC_CMD_CLEAR 1U
#define EFR32_BUFC_CMD_PREFETCH 2U
#define EFR32_BUFC_FIFO_512_CONFIG 3U
#define EFR32_BUFC_RX_THRESHOLD 0x0FFFU
#define EFR32_BUFC_DESCRIPTOR_THRESHOLD 7U
#define EFR32_BUFC_DESCRIPTOR_PENDING BIT(18)

#define EFR32_RADIO_EN_OFFSET 0x004U

static inline volatile uint32_t *radio_reg32(uintptr_t address)
{
	return (volatile uint32_t *)address;
}

/*
 * The OpenISA LLL programs a packet format and a direction-specific packet
 * target before it asks the HAL to arm RAC. Keep that ordering as a checked
 * HAL contract. A completion IRQ without a current RX target cannot be
 * treated as packet delivery, and TXEN must not consume a stale BUFC FIFO.
 *
 * This validates only controller-owned metadata. It does not imply that FRC
 * has consumed or produced bytes in either BUFC FIFO; that relationship still
 * needs evidence.
 */
static void efr32_packet_stage_arm(enum efr32_radio_packet_direction direction)
{
	LL_ASSERT(efr32_packet_stage.packet_configured);
	LL_ASSERT(efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_IDLE);

	if (direction == EFR32_RADIO_PACKET_RX) {
		LL_ASSERT(efr32_packet_stage.rx_target != NULL);
		LL_ASSERT(efr32_packet_stage.rx_fifo_configured);
	#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
		efr32_qemu_rx_descriptor.descriptor_crc_valid = false;
	#endif
	} else {
		LL_ASSERT(direction == EFR32_RADIO_PACKET_TX);
		LL_ASSERT(efr32_packet_stage.tx_source != NULL);
		LL_ASSERT(efr32_packet_stage.tx_fifo_configured);
		LL_ASSERT(efr32_packet_stage.tx_bytes >= 2U);
		LL_ASSERT(efr32_packet_stage.tx_bytes <=
			  (uint32_t)efr32_packet_stage.max_len + 2U);
	}

	/* The LLL reads completion state from its ISR callback, so clear only
	 * before the operation is armed—not from the ISR itself. */
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
	efr32_rx_delivery_invalid = false;
	efr32_packet_lifecycle_arm(&efr32_packet_stage.lifecycle, direction);
}

#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
static void efr32_qemu_tifs_arm_tx(void)
{
	unsigned int key;
	uint32_t generation;
	bool stale;

	/* The completion job has stopped RX before publishing this deadline. */
	cntr_qemu_tifs_cmp_cancel();
	key = irq_lock();
	generation = efr32_qemu_tifs_state_arm_tx(&efr32_qemu_tifs);
	irq_unlock(key);
	efr32_qemu_tifs_schedule(efr32_radio_timer.tifs_us, generation);

	key = irq_lock();
	stale = efr32_qemu_tifs.phase != EFR32_QEMU_TIFS_WAIT_TX ||
		efr32_qemu_tifs.generation != generation;
	irq_unlock(key);
	if (stale) {
		cntr_qemu_tifs_cmp_cancel();
	}
}

/*
 * This deliberately bypasses radio_tx_enable(): that public helper retains
 * OpenISA's legacy busy-wait contract.  The QEMU turn-around worker instead
 * has a final, cancellable idle check and must never spin from a callback or
 * interrupt context.
 */
static bool efr32_qemu_tifs_tx_staged(void)
{
	return efr32_packet_stage.lifecycle.active_direction ==
		EFR32_RADIO_PACKET_IDLE &&
		efr32_packet_stage.tx_source != NULL &&
		efr32_packet_stage.tx_fifo_configured &&
		efr32_packet_stage.tx_bytes >= 2U;
}

static void efr32_qemu_tifs_start_staged_tx(void)
{
	efr32_packet_stage_arm(EFR32_RADIO_PACKET_TX);
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_TXEN;
}

/*
 * Called by the scheduler-owned FRC completion job before it invokes the LLL
 * RX callback. The RV32M1 reference has already programmed its next command
 * by this point; QEMU reserves this turn-around and stops RX now, but does
 * not start its TIFS timer until the callback has staged the response PDU.
 */
static void efr32_qemu_tifs_begin_rx_to_tx(void)
{
	bool claimed;
	unsigned int key;

	k_mutex_lock(&efr32_qemu_tifs_op_lock, K_FOREVER);
	key = irq_lock();
	claimed = efr32_qemu_tifs_next_action_claim(
		&efr32_qemu_tifs_next_action, EFR32_QEMU_TIFS_NEXT_TX);
	/* A received PDU wins over the no-response deadline. */
	if (claimed) {
		efr32_qemu_tifs_state_cancel(&efr32_qemu_tifs);
	}
	irq_unlock(key);
	if (claimed) {
		cntr_qemu_tifs_cmp_cancel();
		/* Do not call radio_disable(): RXDIS here is not an LLL abort. */
		*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_RXDIS;
		key = irq_lock();
		efr32_qemu_tifs_rx_to_tx_staging = true;
		irq_unlock(key);
	}
	k_mutex_unlock(&efr32_qemu_tifs_op_lock);
}

/*
 * Run after the LLL completion callback. A callback may have rejected the
 * packet or torn the event down, so re-check both the reservation and the
 * staged PDU while holding the same lock used by teardown before arming TIFS.
 */
static void efr32_qemu_tifs_arm_staged_rx_to_tx(void)
{
	bool staged;
	unsigned int key;

	k_mutex_lock(&efr32_qemu_tifs_op_lock, K_FOREVER);
	key = irq_lock();
	staged = efr32_qemu_tifs_rx_to_tx_staging;
	efr32_qemu_tifs_rx_to_tx_staging = false;
	irq_unlock(key);
	if (staged && radio_is_idle() && efr32_qemu_tifs_tx_staged()) {
		efr32_qemu_tifs_arm_tx();
	}
	k_mutex_unlock(&efr32_qemu_tifs_op_lock);
}
#endif

/*
 * The public RAIL configurator uses this compact, address-independent format
 * for radio tables. Do not put SEQ.PHYINFO records here: their data words are
 * pointers into the vendor image and are not portable to this firmware.
 */
#define EFR32_RADIO_TABLE_END 0xFFFFFFFFU
#define EFR32_RADIO_TABLE_OP_WRITE 0U
#define EFR32_RADIO_TABLE_OP_AND 1U
#define EFR32_RADIO_TABLE_OP_OR 3U

/*
 * Public xG22 BLE viterbi 1M configuration, copied as table operations rather
 * than as a vendor-addressed PHYINFO record. Source: the Zlib-licensed
 * ble_efr32xg22_configurator_out.c arrays at lines 225-319 and 474-534 in
 * the sibling Simplicity SDK checkout. radio_aa_set(), radio_crc_configure(),
 * and radio_whiten_iv_set() deliberately replace its dynamic defaults later.
 */
static const uint32_t efr32_ble_1m_base_table[] = {
	0x0001400CU, 0x00018101U, /* FRC.DFLCTRL */
	0x00024020U, 0x00000000U, 0x00000001U, /* FRC.WCNTCMP0..1 */
	0x00034030U, 0x00000000U, 0x00000000U, 0x00000000U, /* FRC.WHITE* */
	0x00024040U, 0x00000000U, 0x00004000U, /* FRC RAM addresses */
	0x00014060U, 0x00000101U, /* FRC.PUNCTCTRL */
	0x000140A8U, 0x00000007U, /* FRC.AUTOCG */
	0x000440BCU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
	0x00034108U, 0x00004CFFU, 0x00004DFFU, 0x00004DFFU, /* FRC.FCD1..3 */
	0x1001C020U, 0x0007F800U, 0x3001C020U, 0x000802F5U, /* AGC.CTRL0 */
	0x1001C024U, 0x000000FFU, 0x3001C024U, 0x00001300U, /* AGC.CTRL1 */
	0x0007C028U, 0x83B380ECU, 0x51407543U, 0x48000FA0U, 0x00004030U,
		0x00000000U, 0x00000000U, 0x0000022EU, /* AGC.CTRL2..MANGAIN */
	0x0004C050U, 0x04301151U, 0xE6092D0EU, 0x08070654U, 0x0002B6D1U,
	0x000AC064U, 0x1C003004U, 0x09183040U, 0x2079640DU, 0x01FBFCEBU,
		0x03E8F67FU, 0x15724BBDU, 0x0518A311U, 0x76543210U,
		0x00000A98U, 0x00000000U, /* AGC.GAINRANGE..LBT */
	0x01010008U, 0x00000708U, /* RFCRC.CTRL */
	0x01010018U, 0x00AAAAAAU, /* RFCRC.INIT */
	0x01010020U, 0x00DA6000U, /* RFCRC.POLY */
	0x0101401CU, 0x00000010U, /* MODEM.MIXCTRL */
	0x0103402CU, 0x00008000U, 0x03000000U, 0x00000000U, /* MODEM.CTRL3..5 */
	0x01024068U, 0x71764129U, 0x00000000U, /* MODEM.SYNC0..1 */
	0x010240BCU, 0x00000000U, 0x00000000U, /* MODEM.SHAPING10..11 */
	0x010240E0U, 0x00000033U, 0x00000000U, /* MODEM.DCCOMP* */
	0x010240F4U, 0x07830464U, 0x3AC81388U, /* MODEM.DSATHD0..1 */
	0x01014124U, 0x00000000U, /* MODEM.AUTOCG */
	0x01024130U, 0x0C660664U, 0x0000010CU, /* MODEM.DSATHD2, DIRECTMODE */
	0x01014154U, 0x00000101U, /* MODEM.LRFRC */
	0x01024168U, 0x07830464U, 0x00821388U, /* MODEM.DSATHD3..4 */
	0x01014230U, 0x00000000U, /* MODEM.BLEIQDSA */
	0x01024238U, 0x00000000U, 0x00000000U, /* MODEM.SYNCPROPERTIES, DIGIGAINCTRL */
	0x01014244U, 0x00000000U, /* MODEM.PADEBUG */
	0x01094254U, 0x00000000U, 0x003C0000U, 0x00000000U, 0x00000000U,
		0x55555555U, 0x00000017U, 0x00000000U, 0x00000000U, 0x0006AAAAU,
	0x0102427CU, 0x00000000U, 0x00000000U, /* MODEM.ANTDIVCTRL, BLEIQDSAEXT2 */
	0x01018010U, 0x00000003U, /* SYNTH.CTRL */
	0x0101803CU, 0x00000001U, /* SYNTH.DIVCTRL */
	0x010280B0U, 0x02000300U, 0x01000037U, /* SYNTH.DSMCTRL* */
	0x02020098U, 0x04000C00U, 0x0000004CU, /* RAC.SYNTHREGCTRL, VCOCTRL */
	0x020100A4U, 0x00000400U, /* RAC.SYNTHCTRL */
	0x020200D0U, 0xAA400005U, 0x00000188U, /* RAC.CLKMULTEN* */
	0x020200F4U, 0x00000000U, 0x1108213DU, /* RAC.LNAMIX* */
	0x120100FCU, 0x0000003FU, 0x320100FCU, 0x00045400U, /* RAC.LNAMIXTRIM1 */
	0x02010130U, 0x02510060U, /* RAC.PGACTRL */
	0x02010168U, 0x00000400U, /* RAC.SYMMDCTRL */
	EFR32_RADIO_TABLE_END,
};

static const uint32_t efr32_ble_1m_delta_table[] = {
	/* The omitted first record is the non-portable SEQ.PHYINFO pointer. */
	0x00014010U, 0x00004101U, /* FRC.MAXLENGTH */
	0x0001403CU, 0x00000000U, /* FRC.FECCTRL */
	0x00014048U, 0x000407A0U, /* FRC.CTRL */
	0x00014050U, 0x00000000U, /* FRC.TRAILTXDATACTRL */
	0x0001405CU, 0x00000000U, /* FRC.CONVGENERATOR */
	0x00014104U, 0x00004CFFU, /* FRC.FCD0 */
	0x01034020U, 0x86000000U, 0x0001C01FU, 0x00006000U, /* MODEM.CTRL0..2 */
	0x01014038U, 0x40000000U, /* MODEM.CTRL6 */
	0x01044058U, 0x00FF04C8U, 0x00000840U, 0x00000001U, 0x00040092U,
	0x010F4080U, 0x020C008CU, 0x00000000U, 0x002203B6U, 0x00000000U,
		0x00000000U, 0x0E050200U, 0x53432F1CU, 0x61686962U, 0x04324355U,
		0x0E0E1016U, 0x080A0C0DU, 0x04050607U, 0x02020303U, 0x01010102U,
		0x00010101U, /* MODEM.TIMING..SHAPING9 */
	0x010140ECU, 0x8C2BE890U, /* MODEM.SRCCHF */
	0x010340FCU, 0x000A2090U, 0x80585B05U, 0x4A413747U, /* MODEM.DSA* */
	0x0103410CU, 0x005262F5U, 0x2949192AU, 0x40403B89U, /* MODEM.DIGMIX* */
	0x01014128U, 0x0000DDFFU, /* MODEM.CGCLKSTOP */
	0x01074138U, 0x00FA53E8U, 0x00000000U, 0x00000000U, 0x00000000U,
		0x00000000U, 0x00000000U, 0x00000000U, /* MODEM.LONGRANGE* */
	0x01014170U, 0x000003C1U, /* MODEM.VTBLETIMING */
	0x01014234U, 0x0E000000U, /* MODEM.BLEIQDSAEXT1 */
	0x01014248U, 0xC01EAA58U, /* MODEM.REALTIMCFE */
	0x01014278U, 0x00000000U, /* MODEM.COCURRMODE */
	0x01018038U, 0x00102555U, /* SYNTH.IFFREQ */
	0x0103809CU, 0x00000000U, 0x00037870U, 0x000000D0U, /* SYNTH.LPFCTRL1* */
	0x110180A8U, 0x000001F0U, 0x310180A8U, 0x01CB4205U, /* SYNTH.LPFCTRL2RX */
	0x110180ACU, 0x000001F0U, 0x310180ACU, 0x00FD3E05U, /* SYNTH.LPFCTRL2TX */
	0x020100E4U, 0x13512C6EU, /* RAC.IFADCTRIM0 */
	0x02010154U, 0x00023FC4U, /* RAC.SYTRIM1 */
	EFR32_RADIO_TABLE_END,
};

/* Public BLE viterbi 2M delta at configurator_out.c:537-598; PHYINFO omitted. */
static const uint32_t efr32_ble_2m_delta_table[] = {
	0x00014010U, 0x00004101U, /* FRC.MAXLENGTH */
	0x0001403CU, 0x00000000U, /* FRC.FECCTRL */
	0x00014048U, 0x000407A0U, /* FRC.CTRL */
	0x00014050U, 0x00000000U, /* FRC.TRAILTXDATACTRL */
	0x0001405CU, 0x00000000U, /* FRC.CONVGENERATOR */
	0x00014104U, 0x00004CFFU, /* FRC.FCD0 */
	0x01034020U, 0x86000000U, 0x0001C01FU, 0x00006000U, /* MODEM.CTRL0..2 */
	0x01014038U, 0x40000000U, /* MODEM.CTRL6 */
	0x01044058U, 0x00FF0264U, 0x00000840U, 0x00000001U, 0x00080092U,
	0x010F4080U, 0x020C008CU, 0x00000000U, 0x002203D7U, 0x00000000U,
		0x00000000U, 0x0D050200U, 0x503F2D1AU, 0x63696860U, 0x25354657U,
		0x08090D15U, 0x05050607U, 0x03040404U, 0x02030303U, 0x02020202U,
		0x01010101U, /* MODEM.TIMING..SHAPING9 */
	0x010140ECU, 0x8C2C3890U, /* MODEM.SRCCHF */
	0x010340FCU, 0x000A209CU, 0x80585B05U, 0x4A413747U, /* MODEM.DSA* */
	0x0103410CU, 0x005255B8U, 0x2949192AU, 0x40403B89U, /* MODEM.DIGMIX* */
	0x01014128U, 0x0000DDFFU, /* MODEM.CGCLKSTOP */
	0x01074138U, 0x00FA53E8U, 0x00000000U, 0x00000000U, 0x00000000U,
		0x00000000U, 0x00000000U, 0x00000000U, /* MODEM.LONGRANGE* */
	0x01014170U, 0x000003C1U, /* MODEM.VTBLETIMING */
	0x01014234U, 0x0E000000U, /* MODEM.BLEIQDSAEXT1 */
	0x01014248U, 0xC01EAA58U, /* MODEM.REALTIMCFE */
	0x01014278U, 0x00000000U, /* MODEM.COCURRMODE */
	0x01018038U, 0x00104911U, /* SYNTH.IFFREQ */
	0x0103809CU, 0x00000000U, 0x00037870U, 0x0002C0FFU, /* SYNTH.LPFCTRL1* */
	0x110180A8U, 0x000001F0U, 0x310180A8U, 0x01CB4205U, /* SYNTH.LPFCTRL2RX */
	0x110180ACU, 0x000001F0U, 0x310180ACU, 0x000D0A05U, /* SYNTH.LPFCTRL2TX */
	0x020100E4U, 0x11512C6CU, /* RAC.IFADCTRIM0 */
	0x02010154U, 0x00003FC4U, /* RAC.SYTRIM1 */
	EFR32_RADIO_TABLE_END,
};

/* Public BLE long-range 125 kb/s delta at configurator_out.c:600-661. */
static const uint32_t efr32_ble_coded_s8_delta_table[] = {
	/* The omitted first record is the non-portable SEQ.PHYINFO pointer. */
	0x00014010U, 0x00004100U, 0x0001403CU, 0x00000010U,
	0x00014048U, 0x00040720U, 0x00014050U, 0x00000400U,
	0x0001405CU, 0x00000F0DU, 0x00014104U, 0x0001C003U,
	0x01034020U, 0x46000030U, 0x0001C81FU, 0x10002000U,
	0x01014038U, 0x00000000U,
	0x01044058U, 0x00050018U, 0x00000840U, 0x00000009U, 0x000A0092U,
	0x010F4080U, 0x022C008CU, 0x00000000U, 0x00120012U, 0x1B8C805BU,
		0x00000000U, 0x0E050200U, 0x53432F1CU, 0x61686962U, 0x04324355U,
		0x0E0E1016U, 0x080A0C0DU, 0x04050607U, 0x02020303U, 0x01010102U,
		0x00010101U,
	0x010140ECU, 0x8CB1B080U,
	0x010340FCU, 0x000A209CU, 0x80206101U, 0x123556B7U,
	0x0103410CU, 0x001255B8U, 0x29041BA0U, 0x40408808U,
	0x01014128U, 0x00000000U,
	0x01074138U, 0x3BE8D44CU, 0x306F1904U, 0x1A140E08U, 0x322C2620U,
		0x32103E38U, 0x0A987654U, 0x00005F1DU,
	0x01014170U, 0x00000000U, 0x01014234U, 0x0E000800U,
	0x01014248U, 0x001F81F4U, 0x01014278U, 0x00000000U,
	0x01018038U, 0x00104911U,
	0x0103809CU, 0x000240EBU, 0x00033870U, 0x000000D0U,
	0x110180A8U, 0x000001F0U, 0x310180A8U, 0x01FD5A05U,
	0x110180ACU, 0x000001F0U, 0x310180ACU, 0x00FD3E05U,
	0x020100E4U, 0x11512C6CU, 0x02010154U, 0x00003FC4U,
	EFR32_RADIO_TABLE_END,
};

/* Public BLE long-range 500 kb/s delta at configurator_out.c:663-724. */
static const uint32_t efr32_ble_coded_s2_delta_table[] = {
	/* The omitted first record is the non-portable SEQ.PHYINFO pointer. */
	0x00014010U, 0x00004100U, 0x0001403CU, 0x00000010U,
	0x00014048U, 0x00041720U, 0x00014050U, 0x00000401U,
	0x0001405CU, 0x00000F0DU, 0x00014104U, 0x0001C003U,
	0x01034020U, 0x46000030U, 0x0001C81FU, 0x10002000U,
	0x01014038U, 0x00000000U,
	0x01044058U, 0x00050018U, 0x00000840U, 0x00000009U, 0x000A0092U,
	0x010F4080U, 0x022C008CU, 0x00000000U, 0x00120012U, 0x1B8C805BU,
		0x00000000U, 0x0E050200U, 0x53432F1CU, 0x61686962U, 0x04324355U,
		0x0E0E1016U, 0x080A0C0DU, 0x04050607U, 0x02020303U, 0x01010102U,
		0x00010101U,
	0x010140ECU, 0x8CB1B080U,
	0x010340FCU, 0x000A209CU, 0x80206101U, 0x123556B7U,
	0x0103410CU, 0x001255B8U, 0x29041BA0U, 0x40408808U,
	0x01014128U, 0x00000000U,
	0x01074138U, 0x3BE8D44CU, 0x306F1904U, 0x1C16100AU, 0x342E2822U,
		0x3210403AU, 0x0A987654U, 0xA4805F1DU,
	0x01014170U, 0x00000000U, 0x01014234U, 0x0E000800U,
	0x01014248U, 0x001F81F4U, 0x01014278U, 0x00000000U,
	0x01018038U, 0x00104911U,
	0x0103809CU, 0x000240EBU, 0x00033870U, 0x000000D0U,
	0x110180A8U, 0x000001F0U, 0x310180A8U, 0x01FD5A05U,
	0x110180ACU, 0x000001F0U, 0x310180ACU, 0x00FD3E05U,
	0x020100E4U, 0x11512C6CU, 0x02010154U, 0x00003FC4U,
	EFR32_RADIO_TABLE_END,
};

static void efr32_radio_table_apply(const uint32_t *table)
{
	for (size_t index = 0U; table[index] != EFR32_RADIO_TABLE_END;) {
		uint32_t header = table[index++];
		uint32_t operation = header >> 28;
		uint32_t block = (header >> 24) & 0xFU;
		uint32_t count = (header >> 16) & 0xFFU;
		/*
		 * The public configurator encodes secure 0xA8xx_xxxx block
		 * addresses. Zephyr's xG22 radio devicetree exposes radio RAM at
		 * 0xB0000000, and the generated *_NS models place these blocks in
		 * the non-secure 0xB8xx_xxxx window used by this application.
		 */
		uintptr_t address = 0xB8000000UL + ((uintptr_t)block << 16) +
			(header & 0xFFFFU);

		LL_ASSERT(count != 0U);
		for (uint32_t word = 0U; word < count; word++, address += sizeof(uint32_t)) {
			uint32_t value = table[index++];

			switch (operation) {
			case EFR32_RADIO_TABLE_OP_WRITE:
				*radio_reg32(address) = value;
				break;
			case EFR32_RADIO_TABLE_OP_AND:
				*radio_reg32(address) &= value;
				break;
			case EFR32_RADIO_TABLE_OP_OR:
				*radio_reg32(address) |= value;
				break;
			default:
				LL_ASSERT(false);
			}
		}
	}
}

static uint32_t bit_reverse24(uint32_t value)
{
	uint32_t reversed = 0U;

	for (uint32_t bit = 0U; bit < 24U; bit++) {
		reversed = (reversed << 1) | (value & 1U);
		value >>= 1;
	}

	return reversed;
}

static struct {
	struct ccm config;
	struct pdu_data *rx_in;
	struct pdu_data *rx_out;
	bool pending_rx;
	bool done;
	bool mic_valid;
	bool empty;
} ccm_ctx;

static void ccm_nonce_build(const struct ccm *ccm, uint8_t nonce[BLE_CCM_NONCE_SIZE])
{
	uint64_t counter = ccm->counter;

	memcpy(nonce, &counter, 5);
	nonce[4] |= ccm->direction << 7;
	memcpy(&nonce[5], ccm->iv, sizeof(ccm->iv));
}

static psa_key_id_t ccm_key_import(const struct ccm *ccm, psa_key_usage_t usage)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, BLE_CCM_MIC_SIZE);

	if (psa_crypto_init() != PSA_SUCCESS) {
		return 0;
	}
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 128);
	psa_set_key_usage_flags(&attr, usage);
	psa_set_key_algorithm(&attr, alg);
	if (psa_import_key(&attr, ccm->key, sizeof(ccm->key), &id) != PSA_SUCCESS) {
		return 0;
	}

	return id;
}

/*
 * The FRC line is level-sensitive while IF.RXDONE/TXDONE remains set.  Mask
 * only those two sources after publishing their callback so IRQ exit does not
 * starve the scheduler job.  The job leaves the status visible to the OpenISA
 * callback, then re-enables the sources after that callback has had its
 * normal radio_status_reset() opportunity.
 */
static void efr32_radio_completion_publish(radio_isr_cb_t callback, void *param,
						   bool abort)
{
	if (!efr32_radio_completion_dispatch_queue(&efr32_radio_completion_dispatch,
						      efr32_radio_completion_dispatch.generation)) {
		return;
	}

	efr32_radio_completion_work.callback = callback;
	efr32_radio_completion_work.param = param;
	efr32_radio_completion_work.abort = abort;
	/* FRC completion has occurred; make the callback worker eligible now. */
	k_deadline_event_post(&efr32_radio_completion_event, K_DEADLINE(0));
}

static void efr32_radio_completion_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		radio_isr_cb_t callback = NULL;
		void *param = NULL;
		bool abort = false;
		unsigned int key;

		k_deadline_event_wait(&efr32_radio_completion_event);
		key = irq_lock();
		if (efr32_radio_completion_dispatch_take(&efr32_radio_completion_dispatch)) {
			callback = efr32_radio_completion_work.callback;
			param = efr32_radio_completion_work.param;
			abort = efr32_radio_completion_work.abort;
		}
		irq_unlock(key);

		if (!callback) {
			continue;
		}

#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
		/*
		 * A scan prepare may have recorded a next TX before RX began.  Consume
		 * that request before the LLL clears RX completion state, but leave the
		 * callback time to stage its response PDU before the TIFS worker TXEN.
		 */
		if (!abort && efr32_packet_stage.lifecycle.completion_direction ==
			EFR32_RADIO_PACKET_RX) {
			efr32_qemu_tifs_begin_rx_to_tx();
		}
#endif
		callback(param);
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
		if (!abort) {
			efr32_qemu_tifs_arm_staged_rx_to_tx();
		}
#endif
		if (abort) {
			key = irq_lock();
			efr32_radio_abort_finish(&efr32_radio_abort);
			irq_unlock(key);
		} else {
			/* A queued RXDONE/TXDONE is still visible until the LLL consumes it. */
			*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_SET_OFFSET +
				     EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_COMPLETION;
		}
	}
}


/* [software] lll_init() connects LL_RADIO_IRQn (FRC_IRQn) to this entry. */
void isr_radio(void *arg)
{
	uint32_t pending;
	uint32_t expected = 0U;
	enum efr32_radio_packet_direction completion_direction = EFR32_RADIO_PACKET_IDLE;
	radio_isr_cb_t abort_callback;
	void *abort_param;

	ARG_UNUSED(arg);
	/*
	 * RXDIS/TXDIS do not raise FRC.RXDONE/TXDONE.  Deliver their explicitly
	 * requested cleanup callback without pretending an aborted PDU completed.
	 * radio_isr_set() clears this flag together with a pended FRC IRQ, so this
	 * cannot be delivered to a handler installed for a later event.
	 */
	if (efr32_radio_abort_take(&efr32_radio_abort, &abort_callback, &abort_param)) {
		/*
		 * A completion can race the RXDIS/TXDIS commands after
		 * radio_disable() has cleared the pre-abort latches.  The pending
		 * software abort is terminal for that LLL event, so acknowledge such
		 * a late completion before handing control to its cleanup callback.
		 * Otherwise the shared, level-sensitive FRC IRQ can immediately
		 * re-enter the same callback and make an aborted PDU look completed.
		 */
		*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
		efr32_radio_completion_publish(abort_callback, abort_param, true);
		return;
	}

	pending = *radio_reg32(EFR32_FRC_BASE + EFR32_FRC_IF_OFFSET) &
		  EFR32_FRC_IF_COMPLETION;

	/*
	 * TXDONE and RXDONE share FRC_IRQn, but their OpenISA callbacks have
	 * different contracts. The LLL TX callbacks do not re-check radio_is_done(),
	 * so never let an RX completion advance a TX state machine (or converse).
	 * active_direction is a software routing label; it does not assign how the
	 * hardware produces either latch.
	 */
	if (efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_TX) {
		expected = EFR32_FRC_IF_TXDONE;
		completion_direction = EFR32_RADIO_PACKET_TX;
	} else if (efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_RX) {
		expected = EFR32_FRC_IF_RXDONE;
		completion_direction = EFR32_RADIO_PACKET_RX;
	}

#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	/*
	 * The opt-in QEMU seam publishes its descriptor before retaining the
	 * synthetic RXDONE latch. If BUFC is still pending, hold FRC completion
	 * masked until the scheduler worker has copied/rejected that record. This
	 * preserves the existing LLL callback ordering without assigning that
	 * ordering to real EFR32 hardware.
	 */
	if (completion_direction == EFR32_RADIO_PACKET_RX &&
	    (pending & EFR32_FRC_IF_RXDONE) &&
	    efr32_qemu_rx_descriptor_pending()) {
		*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_RXDONE;
		k_sem_give(&efr32_qemu_rx_descriptor_sem);
		return;
	}
#endif

	/* A stale or wrong-direction completion must not leave FRC_IRQn asserted. */
	if ((pending & ~expected) || (!efr32_radio_abort.callback && pending)) {
		*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			     EFR32_FRC_IF_OFFSET) = !efr32_radio_abort.callback ? pending : pending & ~expected;
	}

	if (efr32_radio_abort.callback && (pending & expected)) {
		/*
		 * Keep the completion visible while the callback samples status.  It
		 * is retired by radio_status_reset(), the LLL's explicit status
		 * consumption boundary, rather than by this IRQ entry.
		 */
		LL_ASSERT(efr32_packet_lifecycle_latch_completion(
			&efr32_packet_stage.lifecycle, completion_direction));
		/* Do not run an LLL state machine on the hardware interrupt stack. */
		*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			     EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_COMPLETION;
		efr32_radio_completion_publish(efr32_radio_abort.callback,
					       efr32_radio_abort.param, false);
	}
}

/*
 * The generic OpenISA LLL owns the FRC_IRQn connection and enable.  It can
 * replace the callback while it advances a radio event, so make the callback
 * and its parameter an interrupt-atomic pair and discard a completion that
 * belonged to the old handler.  This is the single-IRQ equivalent of the
 * RV32M1 HAL's IRQ disable, callback update, pending-event clear, and IRQ
 * restore sequence.
 */
void radio_isr_set(radio_isr_cb_t cb, void *param)
{
	unsigned int key = irq_lock();

	(void)efr32_radio_completion_dispatch_replace(&efr32_radio_completion_dispatch);
	efr32_radio_abort_replace(&efr32_radio_abort, cb, param);
	efr32_packet_lifecycle_forget_completion(&efr32_packet_stage.lifecycle);
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
	/* Replacement also cancels a deferred callback that had masked this line. */
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_COMPLETION;
	/* FRC's line can remain pended after its source was acknowledged. */
	k_irq_clear_pending(LL_RADIO_IRQn);

	irq_unlock(key);
}

/*
 * A reset/startup boundary has no successor LLL event.  Unlike radio_disable(),
 * it must not dispatch the preceding event's cleanup callback.  Quiesce the
 * software callback and both completion-pending paths as one IRQ-atomic
 * transaction, so a retained FRC completion cannot be delivered to it.
 */
static void efr32_radio_completion_quiesce(void)
{
	unsigned int key = irq_lock();

	efr32_radio_abort_cancel(&efr32_radio_abort);
	(void)efr32_radio_completion_dispatch_replace(&efr32_radio_completion_dispatch);
	efr32_radio_completion_work.callback = NULL;
	efr32_radio_completion_work.param = NULL;
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
	k_irq_clear_pending(LL_RADIO_IRQn);
	#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	efr32_radio_qemu_rx_descriptor_quiesce();
	#endif

	irq_unlock(key);
}

/* [regs-public] Enable the six blocks whose public xG22 models expose EN.EN. */
void radio_setup(void)
{
	efr32_packet_stage_reset();
	efr32_rx_delivery_invalid = false;
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	efr32_qemu_tifs_state_init(&efr32_qemu_tifs);
	efr32_qemu_tifs_next_action = EFR32_QEMU_TIFS_NEXT_NONE;
	efr32_qemu_tifs_transition_requested = false;
	efr32_qemu_tifs_rx_to_tx_staging = false;
#endif
	efr32_radio_completion_quiesce();

	/*
	 * Every address below is a rev-B2 non-secure EN register at +0x004, with
	 * the sole writable field EN.EN at bit zero. This deliberately does not
	 * claim an ordering, reset protocol, BUFC enable, or IRQ event policy.
	 */
	*radio_reg32(EFR32_FRC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_AGC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_MODEM_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_SYNTH_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;

	/*
	 * The stock setup helper also writes one at +0x004 in the BUFC and
	 * PROTIMER windows (flash-stock.bin offsets 0x26de4 and 0x26dfc).
	 * Mirror those exact non-secure-alias writes because this HAL uses both
	 * blocks below. Their field semantics and ordering remain uncharacterized.
	 */
	*radio_reg32(EFR32_BUFC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_PROTIMER_BASE + EFR32_RADIO_EN_OFFSET) = 1U;

	/*
	 * [RAIL init] The xG22 RF initializer writes RXCTRL=0x60, then clears
	 * RAC.SR0 bit 6 through its CLR alias (radio-tables.md §3.3). These
	 * are exact initialization writes; they do not assert a packet policy.
	 */
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_RXCTRL_OFFSET) = 0x60U;
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_RAC_SR0_OFFSET) = EFR32_RAC_SR0_INIT_CLEAR;

	/*
	 * The LLL connects its radio ISR to FRC_IRQn.  Enable only the two
	 * completion latches it observes, rather than treating a RAC state change
	 * or an unrelated FRC condition as an OpenISA radio completion.
	 * lll_init() enables FRC_IRQn before calling radio_setup(), so discard a
	 * retained completion before exposing either source through IEN.  This is
	 * intentionally limited to TXDONE/RXDONE: other FRC pending sources are
	 * outside the OpenISA completion contract and must retain their state.
	 */
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_COMPLETION;

	/* Match OpenISA's initially available radio-clock contract. */
	atomic_set(&efr32_radio_power_ref, 1);

#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	/* The descriptor path is an explicit simulator-only test contract. */
	efr32_radio_qemu_rx_descriptor_setup();
#endif
}

/* [regs-vendor] Retain the source-backed reset prefix; see radio-tables.md §4.1. */
void radio_reset(void)
{
	efr32_radio_launch_disarm();
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	efr32_qemu_tifs_cancel();
	efr32_qemu_tifs_next_action = EFR32_QEMU_TIFS_NEXT_NONE;
	efr32_qemu_tifs_transition_requested = false;
#endif
	/* A hardware reset invalidates both BUFC channel configurations. */
	efr32_packet_stage_reset();
	efr32_rx_delivery_invalid = false;
#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	/* Do not carry a simulator descriptor or its synthetic CRC across reset. */
	efr32_qemu_rx_descriptor.descriptor_read_offset = 0U;
	efr32_qemu_rx_descriptor.descriptor_crc_valid = false;
#endif
	efr32_radio_completion_quiesce();

	/*
	 * Stock RAIL clears the three RAC scratch registers and asserts
	 * CTRL.FORCEDISABLE before it observes STATUS.  Its following sequencer
	 * hand-off is still unknown, so preserve only this independently useful
	 * prefix and release the force-disable latch once RAC reports OFF.
	 */
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_SR0_OFFSET) = 0U;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_SR1_OFFSET) = 0U;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_SR2_OFFSET) = 0U;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CTRL_OFFSET) =
		EFR32_RAC_CTRL_FORCEDISABLE;
	while (!radio_is_idle()) {
	}
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CTRL_OFFSET) = 0U;

}

/* [regs-public] Address-independent BLE 1M table; PHYINFO remains intentionally excluded (T3). */
void radio_phy_set(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(flags);

	switch (phy) {
	case PHY_LEGACY:
	case PHY_1M:
		efr32_radio_table_apply(efr32_ble_1m_base_table);
		efr32_radio_table_apply(efr32_ble_1m_delta_table);
		break;
	case PHY_2M:
		efr32_radio_table_apply(efr32_ble_1m_base_table);
		efr32_radio_table_apply(efr32_ble_2m_delta_table);
		break;
	case PHY_CODED:
		efr32_radio_table_apply(efr32_ble_1m_base_table);
		if (flags == PHY_FLAGS_S8) {
			efr32_radio_table_apply(efr32_ble_coded_s8_delta_table);
		} else {
			LL_ASSERT(flags == PHY_FLAGS_S2);
			efr32_radio_table_apply(efr32_ble_coded_s2_delta_table);
		}
		break;
	default:
		EFR32_RADIO_TODO("regs-public: unknown PHY");
		break;
	}
}

/* [regs] PA registers (rfhal_pa): unknown (T7) */
void radio_tx_power_set(uint32_t power)
{
	ARG_UNUSED(power);
	EFR32_RADIO_TODO("PA registers (rfhal_pa)");
}

/* [regs] PA registers (rfhal_pa): unknown (T7) */
void radio_tx_power_max_set(void)
{
	EFR32_RADIO_TODO("PA registers (rfhal_pa)");
}

/* [regs-public] BLE base=2402 MHz, spacing=2 MHz; CHCTRL.CHNO is bits 0..5 (T4). */
void radio_freq_chan_set(uint32_t chan)
{
	uint32_t channel_number;

	/* Zephyr passes MHz relative to 2400: 2..80, always even. */
	LL_ASSERT((chan >= 2U) && (chan <= 80U) && ((chan & 1U) == 0U));
	channel_number = (chan - 2U) / 2U;
	*radio_reg32(EFR32_SYNTH_BASE + EFR32_SYNTH_CHCTRL_OFFSET) =
		channel_number & EFR32_SYNTH_CHCTRL_CHNO_MASK;
}

/* [regs-public] RAIL's BLE helper writes FRC.WHITEPOLY=0x44 and WHITEINIT=iv|0x40 (T5). */
void radio_whiten_iv_set(uint32_t iv)
{
	/* LLL supplies a Bluetooth physical channel number, 0 through 39. */
	LL_ASSERT(iv <= 39U);
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_WHITEPOLY_OFFSET) = EFR32_FRC_BLE_WHITEPOLY;
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_WHITEINIT_OFFSET) =
		EFR32_FRC_BLE_WHITEINIT_MSB | iv;
}

/* [regs-public] MODEM.SYNC0 is the 32-bit BLE synchronization word register (T3). */
void radio_aa_set(const uint8_t *aa)
{
	/*
	 * OpenISA callers pass the Bluetooth access address in little-endian byte
	 * order.  Assemble it explicitly rather than relying on aa being aligned
	 * for a uint32_t load (the API only promises a byte pointer).
	 */
	uint32_t sync0 = (uint32_t)aa[0] |
			 ((uint32_t)aa[1] << 8) |
			 ((uint32_t)aa[2] << 16) |
			 ((uint32_t)aa[3] << 24);

	*radio_reg32(EFR32_MODEM_BASE + EFR32_MODEM_SYNC0_OFFSET) = sync0;
}

/* [regs-partial] FRC length/header configuration remains unproven (T5;
 * docs/radio-tables.md §4.2). CRC, whitening, and access-address
 * configuration are implemented above.  Preserve the LLL's framing bounds
 * for a later evidenced RX completion path, without inferring an FRC layout.
 */
void radio_pkt_configure(uint8_t bits_len, uint8_t max_len, uint8_t flags)
{
	/*
	 * A new framing contract invalidates both direction-specific FIFO setups.
	 * In particular, do not let a later TXEN reuse bytes staged for an older
	 * maximum length or header representation.  OpenISA configures a packet
	 * while no packet operation is active; retain that ordering as an explicit
	 * controller contract rather than trying to reconfigure an active FRC.
	 */
	LL_ASSERT(efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_IDLE);
	efr32_packet_stage.rx_target = NULL;
	efr32_packet_stage.rx_fifo_configured = false;
	efr32_packet_stage.tx_source = NULL;
	efr32_packet_stage.tx_bytes = 0U;
	efr32_packet_stage.tx_fifo_configured = false;
	efr32_packet_stage.bits_len = bits_len;
	efr32_packet_stage.max_len = max_len;
	efr32_packet_stage.flags = flags;
	efr32_packet_stage.packet_configured = true;
	EFR32_RADIO_TODO("FRC/CRC/whitening/sync-word registers");
}

/*
 * [regs-partial] Configure only the source-backed BUFC RX staging FIFO.
 * FRC's delivery of a received packet to this FIFO remains unproven (T5).
 */
void radio_pkt_rx_set(void *rx_packet)
{
	uintptr_t base = EFR32_BUFC_BASE + EFR32_BUFC_RX_BASE_OFFSET;

	LL_ASSERT(efr32_packet_stage.packet_configured);
	/* The current OpenISA callers use the one-octet BLE length field. */
	LL_ASSERT(efr32_packet_stage.bits_len == 8U);
	/*
	 * The controller hands this target to the HAL before arming RX.  Replacing
	 * it after RAC is armed could redirect a future, normalized completion to
	 * a buffer belonging to another LLL event, so make that ordering an
	 * explicit HAL invariant.
	 */
	LL_ASSERT(efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_IDLE);
	LL_ASSERT(rx_packet != NULL);
	LL_ASSERT(((uintptr_t)efr32_rx_fifo & 3U) == 0U);

	/*
	 * RAIL's RX FIFO helper configures channel 1 in this exact order: CLEAR,
	 * capacity encoding, backing address, and threshold control. Unlike TX,
	 * its helper does not issue PREFETCH. Keep the caller buffer untouched
	 * until the unproven FRC packet handoff can be implemented.
	 */
	*radio_reg32(base + EFR32_BUFC_CMD_OFFSET) = EFR32_BUFC_CMD_CLEAR;
	*radio_reg32(base + EFR32_BUFC_CFG_OFFSET) = EFR32_BUFC_FIFO_512_CONFIG;
	*radio_reg32(base + EFR32_BUFC_ADDR_OFFSET) = (uint32_t)(uintptr_t)efr32_rx_fifo;
	*radio_reg32(base + EFR32_BUFC_THRESHOLD_OFFSET) = EFR32_BUFC_RX_THRESHOLD;

	/* Retain the controller's target for the eventual proven RX copy-out. */
	efr32_packet_stage.rx_target = rx_packet;
	efr32_packet_stage.rx_fifo_configured = true;
	efr32_radio_rx_pdu_delivery_reset(&efr32_packet_stage.rx_delivery);
}

/*
 * This is deliberately not connected to an FRC interrupt yet.  Its input is
 * a normalized Bluetooth Link Layer PDU: header octet, length octet, and
 * exactly that many payload octets.  A future caller must first prove how to
 * obtain this representation from BUFC/FRC; it must not pass raw BUFC memory
 * here or infer a frame boundary from an FRC completion interrupt.
 *
 * Keeping the controller-facing copy and its bounds check separate from that
 * unresolved hardware handoff makes the eventual delivery path auditable.
 */
static int __maybe_unused efr32_packet_stage_rx_complete(const uint8_t *pdu,
						  uint16_t bytes)
{
	int err;

	if (!efr32_packet_stage.rx_target ||
	    !efr32_packet_stage.packet_configured ||
	    !efr32_packet_stage.rx_fifo_configured ||
	    efr32_packet_stage.lifecycle.active_direction != EFR32_RADIO_PACKET_RX) {
		efr32_rx_delivery_invalid = true;
		return -EINVAL;
	}

	err = efr32_radio_rx_pdu_deliver_once(&efr32_packet_stage.rx_delivery,
						     efr32_packet_stage.rx_target, pdu, bytes,
						     efr32_packet_stage.bits_len,
						     efr32_packet_stage.max_len);
	if (err) {
		efr32_rx_delivery_invalid = true;
		return err;
	}

#if defined(CONFIG_BT_CTLR_PRIVACY)
	/* OpenISA resolves the completed, normalized advertising PDU before LLL. */
	efr32_radio_ar_execute(pdu, bytes);
#endif

	/*
	 * Do not release this stage here: a producer runs before RXDONE, while
	 * isr_radio() still needs the active direction to attribute that latch.
	 * radio_status_reset() retires the stage after the LLL callback sampled
	 * the resulting completion and CRC state.
	 */

	return 0;
}

#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
/*
 * This is an explicitly synthetic receive boundary.  The worker is the only
 * code that reads descriptor or FIFO bytes; the BUFC IRQ merely masks the
 * source and wakes it.  That keeps packet copying and channel-1 release out
 * of interrupt context and lets the existing FRC completion job remain the
 * sole caller of the OpenISA LLL callback.
 */
static inline volatile uint8_t *efr32_qemu_rx_descriptor_ring(void)
{
	uint32_t address = *radio_reg32(EFR32_BUFC_BASE +
		EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET + EFR32_BUFC_ADDR_OFFSET);

	if (address == 0U) {
		return NULL;
	}

	return (volatile uint8_t *)(uintptr_t)address;
}

static inline uint8_t efr32_qemu_rx_descriptor_ring_byte(
	volatile uint8_t *ring, uint16_t offset)
{
	return ring[efr32_radio_rx_descriptor_ring_storage_offset(offset)];
}

static inline bool efr32_qemu_rx_descriptor_pending(void)
{
	return !!(*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_IF_OFFSET) &
		EFR32_BUFC_DESCRIPTOR_PENDING);
}

/*
 * The descriptor ring is a QEMU-only circular FIFO. Keep its logical
 * consumer pointer visible to the model as each record is consumed; without
 * this acknowledgement, the model reports a full 512-byte ring after 64
 * eight-byte records even though the scheduler has delivered them all.
 */
static inline void efr32_qemu_rx_descriptor_ack(uint16_t offset)
{
	*radio_reg32(EFR32_BUFC_BASE +
		EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET + EFR32_BUFC_READOFFSET_OFFSET) =
		offset & EFR32_RADIO_RX_DESCRIPTOR_ENDPOINT_MASK;
}

static void efr32_radio_qemu_rx_descriptor_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		volatile uint8_t *ring;
		uint32_t fifo_address;
		uint16_t write_offset;

		k_sem_take(&efr32_qemu_rx_descriptor_sem, K_FOREVER);
		ring = efr32_qemu_rx_descriptor_ring();
		fifo_address = *radio_reg32(EFR32_BUFC_BASE +
			EFR32_BUFC_RX_BASE_OFFSET + EFR32_BUFC_ADDR_OFFSET);
		if (!ring || fifo_address != (uint32_t)(uintptr_t)efr32_rx_fifo ||
		    efr32_packet_stage.lifecycle.active_direction != EFR32_RADIO_PACKET_RX) {
			/* A stale synthetic event is not a packet. */
			*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
				EFR32_BUFC_IF_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
			continue;
		}

		write_offset = *radio_reg32(EFR32_BUFC_BASE +
			EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET + EFR32_BUFC_WRITEOFFSET) &
			EFR32_RADIO_RX_DESCRIPTOR_ENDPOINT_MASK;
		while (efr32_qemu_rx_descriptor.descriptor_read_offset != write_offset) {
			uint8_t raw[EFR32_RADIO_RX_DESCRIPTOR_BYTES];
			struct efr32_radio_rx_descriptor descriptor;
			uint8_t pdu[PDU_AC_LL_SIZE_MAX];
			uint16_t pdu_bytes = 0U;
			uint16_t descriptor_offset =
				efr32_qemu_rx_descriptor.descriptor_read_offset;
			int parse_err;
			int pdu_err;

			for (uint8_t index = 0U;
			     index < EFR32_RADIO_RX_DESCRIPTOR_BYTES; index++) {
				raw[index] = efr32_qemu_rx_descriptor_ring_byte(
					ring, descriptor_offset + index);
			}
			efr32_qemu_rx_descriptor.descriptor_read_offset =
				efr32_radio_rx_descriptor_ring_advance(descriptor_offset);
			efr32_qemu_rx_descriptor_ack(
				efr32_qemu_rx_descriptor.descriptor_read_offset);

			parse_err = efr32_radio_rx_descriptor_parse(raw, &descriptor);
			if (parse_err) {
				/* Non-received classes are queue events, never PDUs. */
				continue;
			}
			/*
			 * A single OpenISA RX stage owns exactly one PDU until its
			 * completion callback calls radio_status_reset().  The synthetic
			 * QEMU producer can queue descriptor records faster than this
			 * scheduler-owned consumer runs.  Retire any surplus successful
			 * record and its source FIFO interval, but never let its expected
			 * -EALREADY overwrite the first frame's CRC-valid outcome.
			 */
			if (efr32_radio_rx_pdu_delivery_is_complete(
				    &efr32_packet_stage.rx_delivery)) {
				*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_BASE_OFFSET +
					EFR32_BUFC_READOFFSET_OFFSET) = descriptor.endpoint;
				continue;
			}

			pdu_err = efr32_radio_rx_descriptor_normalize_pdu(
				pdu, efr32_rx_fifo,
				(uint16_t)*radio_reg32(EFR32_BUFC_BASE +
					EFR32_BUFC_RX_BASE_OFFSET + EFR32_BUFC_READOFFSET_OFFSET),
				descriptor.endpoint, efr32_packet_stage.max_len, &pdu_bytes);
			if (pdu_err || efr32_packet_stage_rx_complete(pdu, pdu_bytes)) {
				efr32_rx_delivery_invalid = true;
				continue;
			}
			/*
			 * Class 6/7 is the synthetic QEMU success result. It stands in
			 * for the missing hardware trailer/CRC producer and is never
			 * enabled in a hardware configuration.
			 */
			efr32_qemu_rx_descriptor.descriptor_crc_valid = true;

			/* The endpoint is the source-backed channel-1 release point. */
			*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_BASE_OFFSET +
				EFR32_BUFC_READOFFSET_OFFSET) = descriptor.endpoint;
		}

		*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			EFR32_BUFC_IF_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
		*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_SET_OFFSET +
			EFR32_BUFC_IEN_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
		/* Only now may a retained FRC.RXDONE wake the completion ISR. */
		*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_SET_OFFSET +
			EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_COMPLETION;
	}
}

K_THREAD_DEFINE(efr32_qemu_rx_descriptor_thread,
		CONFIG_EFR32_RADIO_COMPLETION_STACK_SIZE,
		efr32_radio_qemu_rx_descriptor_job, NULL, NULL, NULL,
		CONFIG_EFR32_RADIO_SCHEDULER_PRIO, 0, 0);

/* BUFC IRQ work is scheduler-owned; this ISR only masks and wakes. */
static void efr32_radio_qemu_rx_descriptor_isr(void *arg)
{
	uint32_t pending;

	ARG_UNUSED(arg);
	pending = *radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_IF_OFFSET) &
		*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_IEN_OFFSET);
	if (!(pending & EFR32_BUFC_DESCRIPTOR_PENDING)) {
		return;
	}

	*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		EFR32_BUFC_IEN_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
	k_sem_give(&efr32_qemu_rx_descriptor_sem);
}

static void efr32_radio_qemu_rx_descriptor_setup(void)
{
	unsigned int key = irq_lock();
	uintptr_t descriptor_start =
		(uintptr_t)efr32_qemu_rx_descriptor_ring_storage;
	uintptr_t descriptor_end = descriptor_start +
		EFR32_RADIO_QEMU_RX_DESCRIPTOR_RING_BYTES;
	uintptr_t fifo_start = (uintptr_t)efr32_rx_fifo;
	uintptr_t fifo_end = fifo_start + 512U;

	/* Catch a linker/layout regression before QEMU can DMA across regions. */
	LL_ASSERT(descriptor_end <= fifo_start || fifo_end <= descriptor_start);

	efr32_qemu_rx_descriptor.descriptor_read_offset = 0U;
	efr32_qemu_rx_descriptor.descriptor_crc_valid = false;
	k_sem_reset(&efr32_qemu_rx_descriptor_sem);
	*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		EFR32_BUFC_IF_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
	*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET +
		EFR32_BUFC_CMD_OFFSET) = EFR32_BUFC_CMD_CLEAR;
	*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET +
		EFR32_BUFC_CFG_OFFSET) = EFR32_BUFC_FIFO_512_CONFIG;
	*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET +
		EFR32_BUFC_ADDR_OFFSET) =
		(uint32_t)(uintptr_t)efr32_qemu_rx_descriptor_ring_storage;
	*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET +
		EFR32_BUFC_WRITEOFFSET) = 0U;
	efr32_qemu_rx_descriptor_ack(0U);
	*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET +
		EFR32_BUFC_THRESHOLD_OFFSET) = EFR32_BUFC_DESCRIPTOR_THRESHOLD;
	*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_SET_OFFSET +
		EFR32_BUFC_IEN_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
	IRQ_CONNECT(BUFC_IRQn, CONFIG_BT_CTLR_LLL_PRIO,
			efr32_radio_qemu_rx_descriptor_isr, NULL, 0);
	irq_enable(BUFC_IRQn);
	irq_unlock(key);
}

static void efr32_radio_qemu_rx_descriptor_quiesce(void)
{
	*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		EFR32_BUFC_IEN_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
	*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		EFR32_BUFC_IF_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
	k_irq_clear_pending(BUFC_IRQn);
}
#endif /* CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR */

/*
 * [regs-partial] Configure only the source-backed BUFC TX staging FIFO.
 * FRC's consumption of that FIFO and sequencer handoff remain unproven (T5).
 */
void radio_pkt_tx_set(void *tx_packet)
{
	const uint8_t *pdu = tx_packet;
	uint32_t bytes;
	uintptr_t base = EFR32_BUFC_BASE + EFR32_BUFC_TX_BASE_OFFSET;

	LL_ASSERT(efr32_packet_stage.packet_configured);
	/* All current OpenISA TX callers use the octet-sized LL length field. */
	LL_ASSERT(efr32_packet_stage.bits_len == 8U);
	/*
	 * BUFC owns the staging FIFO after TX is armed.  Reconfiguring it while
	 * RAC is active could make that operation consume a mixture of two LLL
	 * events, so retain the same setup-before-arm contract as the RX path.
	 */
	LL_ASSERT(efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_IDLE);
	LL_ASSERT(pdu != NULL);
	bytes = (uint32_t)pdu[1] + 2U;
	/* OpenISA PDUs are header byte, payload-length byte, then payload. */
	LL_ASSERT(pdu[1] <= efr32_packet_stage.max_len);
	LL_ASSERT(bytes <= sizeof(efr32_tx_fifo));
	LL_ASSERT(((uintptr_t)efr32_tx_fifo & 3U) == 0U);
	memcpy(efr32_tx_fifo, pdu, bytes);

	/*
	 * Exact RAIL SetTxFifo order for a 512-byte FIFO: CLEAR, configuration
	 * value log2(512)-6, address, initial byte count, then PREFETCH.
	 */
	*radio_reg32(base + EFR32_BUFC_CMD_OFFSET) = EFR32_BUFC_CMD_CLEAR;
	*radio_reg32(base + EFR32_BUFC_CFG_OFFSET) = EFR32_BUFC_FIFO_512_CONFIG;
	*radio_reg32(base + EFR32_BUFC_ADDR_OFFSET) = (uint32_t)(uintptr_t)efr32_tx_fifo;
	*radio_reg32(base + EFR32_BUFC_WRITEOFFSET) = bytes;
	*radio_reg32(base + EFR32_BUFC_CMD_OFFSET) = EFR32_BUFC_CMD_PREFETCH;

	/* Keep source and length for a future FRC/BUFC completion diagnostic. */
	efr32_packet_stage.tx_source = tx_packet;
	efr32_packet_stage.tx_bytes = bytes;
	efr32_packet_stage.tx_fifo_configured = true;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_tx_ready_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_tx_chain_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_rx_ready_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_rx_chain_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
}

/*
 * [regs-vendor] RAIL StartRx writes RXENSRCEN.SET.SWRXEN (bit 1). The
 * archived primitive is at librail_efr32xg22_gcc_release.a(rail_rf_hal.o),
 * sli_rail_dadf1c52f26190429dcacf7151424c68; see radio-tables.md §4.1.
 */
void radio_rx_enable(void)
{
	while (!radio_is_idle()) {
	}

	/* Validate and label a completion before arming the hardware source. */
	efr32_packet_stage_arm(EFR32_RADIO_PACKET_RX);
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_RAC_RXENSRCEN_OFFSET) = EFR32_RAC_RXENSRCEN_SWRXEN;
}

/*
 * [regs-vendor] The generic RAIL TX helper ends by writing one to RAC.CMD.
 * Its preceding packet/FIFO setup is separate and remains unimplemented;
 * see docs/radio-tables.md §4.1.
 */
void radio_tx_enable(void)
{
	/* Match the OpenISA radio contract: do not start a new TX while active. */
	while (!radio_is_idle()) {
	}

	/* Validate and label a completion before arming the hardware source. */
	efr32_packet_stage_arm(EFR32_RADIO_PACKET_TX);
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_TXEN;
}

/* [regs-public] RAC.CMD TXDIS/RXDIS are write-only command bits (T2). */
void radio_disable(void)
{
	unsigned int key;

	efr32_radio_launch_disarm();
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	efr32_qemu_tifs_cancel();
	efr32_qemu_tifs_next_action = EFR32_QEMU_TIFS_NEXT_NONE;
	efr32_qemu_tifs_transition_requested = false;
#endif

	/*
	 * Keep independent command writes: their combined hardware semantics are
	 * unverified. The retained stock trace issues RXDIS before TXDIS
	 * (docs/radio-tables.md §4.1), so preserve that observed order.
	 */
	key = irq_lock();
	efr32_packet_lifecycle_reset(&efr32_packet_stage.lifecycle);
	/* An aborted PDU cannot be delivered by a later receive completion. */
	efr32_packet_stage.rx_target = NULL;
	efr32_packet_stage.rx_fifo_configured = false;
	efr32_packet_stage.tx_source = NULL;
	efr32_packet_stage.tx_bytes = 0U;
	efr32_packet_stage.tx_fifo_configured = false;
	/* An abort is not a TXDONE or RXDONE; discard any pre-abort completion. */
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_RXDIS;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_TXDIS;
	/*
	 * OpenISA relies on radio_disable() to enter the installed abort/cleanup
	 * callback.  FRC has no disable-complete latch, so pend its already-owned
	 * IRQ as a software event and make isr_radio() distinguish it from a PDU
	 * completion above.
	 */
	if (efr32_radio_abort_request(&efr32_radio_abort)) {
		k_irq_set_pending(LL_RADIO_IRQn);
	}
	irq_unlock(key);
}

/* [regs-public] FRC.IF has named TXDONE and RXDONE completion latches. */
void radio_status_reset(void)
{
	bool completed;
	enum efr32_radio_packet_direction completed_direction;

	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
	/* The LLL has sampled completion/CRC state before asking to clear it. */
	completed_direction = efr32_packet_stage.lifecycle.completion_direction;
	completed = efr32_packet_lifecycle_status_reset(&efr32_packet_stage.lifecycle);
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	/*
	 * A TX-completion callback prepares the RX packet and timeout after it
	 * clears status. Claim the RX prearm now, before that callback records a
	 * later RX-to-TX request. The HCTO call made by the callback consumes this
	 * flag and arms the bounded BURTC RX job.
	 */
	if (completed && completed_direction == EFR32_RADIO_PACKET_TX &&
	    efr32_qemu_tifs_next_action_claim(&efr32_qemu_tifs_next_action,
					       EFR32_QEMU_TIFS_NEXT_RX)) {
		efr32_qemu_tifs_transition_requested = true;
	}
#endif
	efr32_rx_delivery_invalid = false;
#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	efr32_qemu_rx_descriptor.descriptor_crc_valid = false;
#endif
}

/*
 * [software] OpenISA requires this to remain false while it stages a packet.
 * The reference RV32M1 HAL deliberately returns false regardless of its
 * hardware state; RAC state must not be substituted for that API contract.
 */
uint32_t radio_is_ready(void)
{
	return 0;
}

/* [regs-public] FRC.IF.TXDONE/RXDONE are the completion latches (T2). */
uint32_t radio_is_done(void)
{
	return !!(*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_IF_OFFSET) &
		 EFR32_FRC_IF_COMPLETION);
}

/* [software] Unused by the OpenISA LLL; matches the reference HAL. */
uint32_t radio_has_disabled(void)
{
	return 0;
}

/* [regs-public] RAC.STATUS.STATE is read-only and OFF is state 0 (T2). */
uint32_t radio_is_idle(void)
{
	return ((*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET) >>
		 EFR32_RAC_STATUS_STATE_SHIFT) & EFR32_RAC_STATUS_STATE_MASK) ==
		EFR32_RAC_STATE_OFF;
}

/* [regs-public] RFCRC values match BLE's 24-bit CRC after bit reversal (T5). */
void radio_crc_configure(uint32_t polynomial, uint32_t iv)
{
	LL_ASSERT((polynomial & ~0x00FFFFFFU) == 0U);
	LL_ASSERT((iv & ~0x00FFFFFFU) == 0U);
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_CTRL_OFFSET) = EFR32_RFCRC_BLE_CTRL;
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_INIT_OFFSET) = bit_reverse24(iv);
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_POLY_OFFSET) = bit_reverse24(polynomial);
}

/*
 * FRC.TRAILRXDATA.CRCOK is the hardware CRC result.  Gate it with the
 * private staging result so a future rejected normalized PDU follows
 * OpenISA's completed-but-invalid receive path without writing CRCOK.
 */
uint32_t radio_crc_is_valid(void)
{
	#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	if (efr32_qemu_rx_descriptor.descriptor_crc_valid) {
		return 1U;
	}
	#endif
	return !efr32_rx_delivery_invalid &&
	       !!(*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_TRAILRXDATA_OFFSET) &
		 EFR32_FRC_TRAILRXDATA_CRCOK);
}

/* [software] software (no register access needed); not implemented yet */
void *radio_pkt_empty_get(void)
{
	return pkt_empty;
}

/* [software] software (no register access needed); not implemented yet */
void *radio_pkt_scratch_get(void)
{
	return pkt_scratch;
}

/*
 * OpenISA's radio_switch_complete_and_tx() is called in two distinct
 * contexts: before radio_tmr_start() during event setup, and from the
 * packet-complete ISR for an RX/TX turn-around (see its radio.c:231-233,
 * 350-354, and 937-945).  The latter does not issue RAC.CMD immediately.
 * It records the next sequencer command and, after the packet-end capture,
 * programs T1 for end + TIFS - warmup.  A conforming EFR32 implementation
 * therefore needs all of these state inputs before it can arm a switch:
 *
 *   - a source-backed packet-end timestamp;
 *   - the requested TIFS interval; and
 *   - PHY-specific RX/TX warm-up/chain-delay values and a proven
 *     PROTIMER-to-RAC command path.
 *
 * This port currently has only the BURTC event-start alarm.  The end-capture
 * API records a request (radio_tmr_end_capture()), but no captured timestamp
 * is available, and QEMU models no autonomous TIFS sequencer.  Do not turn
 * this into an immediate RAC command: that would change the OpenISA
 * packet-complete contract and would not be evidence-consistent.
 */
void radio_switch_complete_and_rx(uint8_t phy_rx)
{
	ARG_UNUSED(phy_rx);
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	/*
	 * This is an instruction for the operation following the one currently
	 * being prepared. For example, advertising records RX before its initial
	 * TX; an active scanner records RX before it stages its SCAN_REQ. Do not
	 * tie that instruction to a completion marker: the LLL intentionally
	 * replaces it while handling the preceding completion.
	 */
	efr32_qemu_tifs_next_action_set(&efr32_qemu_tifs_next_action,
					EFR32_QEMU_TIFS_NEXT_RX);
#endif
	EFR32_RADIO_TODO("software TIFS on top of PROTIMER compare + RAC com");
}

/* See radio_switch_complete_and_rx() for the bounded implementation contract. */
void radio_switch_complete_and_tx(uint8_t phy_rx, uint8_t flags_rx, uint8_t phy_tx,
				  uint8_t flags_tx)
{
	ARG_UNUSED(phy_rx);
	ARG_UNUSED(flags_rx);
	ARG_UNUSED(phy_tx);
	ARG_UNUSED(flags_tx);
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	/*
	 * A scanner or advertiser may record this before RX begins. A real RXDONE
	 * claims it before invoking the LLL callback, gives that callback time to
	 * stage its response PDU, then lets the bounded QEMU TIFS job issue TXEN.
	 */
	efr32_qemu_tifs_next_action_set(&efr32_qemu_tifs_next_action,
					EFR32_QEMU_TIFS_NEXT_TX);
#endif
	EFR32_RADIO_TODO("software TIFS on top of PROTIMER compare + RAC com");
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
void radio_switch_complete_and_disable(void)
{
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	/*
	 * This clears the sequencer command that has not been consumed yet. It
	 * must not cancel an RXDONE-claimed reservation: lll_adv uses this call
	 * immediately before it stages the SCAN_RSP that owns that reservation.
	 */
	efr32_qemu_tifs_next_action_clear(&efr32_qemu_tifs_next_action);
#endif
	EFR32_RADIO_TODO("software TIFS on top of PROTIMER compare + RAC com");
}

/* [regs-public] AGC.CMD.RSSISTART requests a live RSSI measurement (T9). */
void radio_rssi_measure(void)
{
	*radio_reg32(EFR32_AGC_BASE + EFR32_AGC_CMD_OFFSET) =
		EFR32_AGC_CMD_RSSISTART;
}

/*
 * RAIL reads FRAMERSSI rather than the live RSSI register for a received
 * packet. The encoded 10-bit value is signed quarter-dB with RAIL's
 * source-backed -200 quarter-dB offset. OpenISA stores RSSI as a positive
 * magnitude, so negate the resulting signed dB value at the API boundary.
 */
uint32_t radio_rssi_get(void)
{
	int32_t code = (*radio_reg32(EFR32_AGC_BASE + EFR32_AGC_FRAMERSSI_OFFSET) >>
			EFR32_AGC_RSSI_CODE_SHIFT) & EFR32_AGC_RSSI_CODE_MASK;
	int32_t rssi_quarter_db;
	int32_t rssi_db;

	if (code & 0x200) {
		code -= 0x400;
	}
	if (code == EFR32_AGC_RSSI_INVALID) {
		return 127U;
	}

	rssi_quarter_db = code - EFR32_AGC_RSSI_OFFSET_QUARTER_DB;
	if (rssi_quarter_db < -511) {
		rssi_quarter_db = -511;
	}
	rssi_db = rssi_quarter_db / 4;

	return rssi_db < 0 ? (uint32_t)-rssi_db : 0U;
}

/* [regs-public] AGC.IF.RSSIVALID is the public RSSI-valid status latch (T9). */
void radio_rssi_status_reset(void)
{
	*radio_reg32(EFR32_AGC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_AGC_IF_OFFSET) = EFR32_AGC_IF_RSSIVALID;
}

/* [regs-public] AGC.IF.RSSIVALID records availability of a live RSSI result (T9). */
uint32_t radio_rssi_is_ready(void)
{
	return !!(*radio_reg32(EFR32_AGC_BASE + EFR32_AGC_IF_OFFSET) &
		 EFR32_AGC_IF_RSSIVALID);
}

/* [software] OpenISA's reference HAL intentionally leaves software filtering disabled. */
void radio_filter_configure(uint8_t bitmask_enable, uint8_t bitmask_addr_type,
			    uint8_t *bdaddr)
{
	ARG_UNUSED(bitmask_enable);
	ARG_UNUSED(bitmask_addr_type);
	ARG_UNUSED(bdaddr);
}

/* [software] No filter hardware or software matcher is enabled by this LLL path. */
void radio_filter_disable(void)
{
}

/* [software] No match latch exists while filtering is disabled. */
void radio_filter_status_reset(void)
{
}

/* [software] Matches the OpenISA reference's permanently no-match filter path. */
uint32_t radio_filter_has_match(void)
{
	return 0;
}

/* [software] No filter match index exists while filtering is disabled. */
uint32_t radio_filter_match_get(void)
{
	return 0;
}

/* [software] OpenISA's reference does not configure a bit counter here. */
void radio_bc_configure(uint32_t n)
{
	ARG_UNUSED(n);
}

/* [software] No bit-counter status is produced by this port. */
void radio_bc_status_reset(void)
{
}

/* [software] Matches the OpenISA reference's permanently no-match bit counter. */
uint32_t radio_bc_has_match(void)
{
	return 0;
}

static uint32_t efr32_radio_launch_schedule(uint8_t trx, uint32_t tick, uint32_t remainder)
{
	uint32_t generation;

	if (!(remainder / 1000000UL) || (remainder & BIT(31))) {
		tick--;
		remainder += HAL_TICKER_CNTR_CLK_UNIT_FSEC / 1000UL;
	}
	remainder /= 1000000UL;
	do {
		generation = (uint32_t)atomic_inc(&efr32_radio_launch_generation) + 1U;
	} while (!generation);
	atomic_set(&efr32_radio_launch_active, 0);
	atomic_set(&efr32_radio_launch_active, generation);
	efr32_radio_timer_state_start_set(&efr32_radio_timer, tick);
	cntr_radio_cmp_set(trx, tick, generation);
	return remainder;
}

/* [software] OpenISA capture-request bookkeeping; PROTIMER capture remains unknown (T8). */
void radio_tmr_status_reset(void)
{
	efr32_radio_timer_state_status_reset(&efr32_radio_timer);
}

/* [software] Retain TIFS requested by the LLL; programming it remains unknown (T2, T8). */
void radio_tmr_tifs_set(uint32_t tifs)
{
	efr32_radio_timer_state_tifs_set(&efr32_radio_timer, tifs);
}

/* [software] Schedule the normal TX/RX enable path through the shared BURTC job bridge. */
uint32_t radio_tmr_start(uint8_t trx, uint32_t ticks_start, uint32_t remainder)
{
	return efr32_radio_launch_schedule(trx, ticks_start, remainder);
}

/* [software] Schedule a whole-tick TX/RX launch through the shared BURTC job bridge. */
uint32_t radio_tmr_start_tick(uint8_t trx, uint32_t tick)
{
	return efr32_radio_launch_schedule(trx, tick, 1000000UL);
}

/* [software] Return the retained controller tick; no hardware read is implied. */
void radio_tmr_start_us(uint8_t trx, uint32_t us)
{
	ARG_UNUSED(trx);
	ARG_UNUSED(us);
	EFR32_RADIO_AIR_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [software] Request a later AA capture; capture itself remains unknown (T8). */
uint32_t radio_tmr_start_now(uint8_t trx)
{
	ARG_UNUSED(trx);
	EFR32_RADIO_AIR_TODO("regs");
	return 0;
}

/* [software] Keep LLL-supplied AA timestamp independently of hardware capture (T8). */
uint32_t radio_tmr_start_get(void)
{
	return efr32_radio_timer.start_tick;
}

/* [software] Restore LLL-supplied AA timestamp; no hardware read is implied. */
void radio_tmr_stop(void)
{
	efr32_radio_launch_disarm();
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	efr32_qemu_tifs_cancel();
	efr32_qemu_tifs_next_action = EFR32_QEMU_TIFS_NEXT_NONE;
	efr32_qemu_tifs_transition_requested = false;
#endif
}

/*
 * OpenISA arms GENFSK.T2_CMP with an EVENT_TMR-relative value here
 * (vendor/zephyr/.../radio.c:1149-1173).  Its callers compute that value from
 * radio_tmr_tifs_base_get() and PHY/address delays (for example
 * lll_adv.c:552-557 and lll_peripheral.c:197-211).  Those inputs are not a
 * BURTC timestamp: this port's start path coarsens the fractional launch
 * remainder, and its packet-end/TIFS base remains unresolved.
 *
 * Keep the request as software bookkeeping, but do not arm the shared BURTC
 * compare or synthesize a timeout IRQ.  A correct scheduler-owned timeout
 * still needs an event epoch conversion, cancellation/generation arbitration
 * with the launch compare, and a bounded timeout completion job.  QEMU also
 * intentionally models no PROTIMER compare flag or autonomous RAC timeout.
 */
void radio_tmr_hcto_configure(uint32_t hcto)
{
	efr32_radio_timer_state_hcto_set(&efr32_radio_timer, hcto);
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	if (!efr32_qemu_tifs_transition_requested) {
		/* Initial event setup has no post-TXDONE transition marker. */
		return;
	}
	/*
	 * This arm is intentionally based on the TX-completion marker rather than
	 * on a guessed hardware timestamp. It is a QEMU-only scheduler contract;
	 * hardware builds retain the bookkeeping-only behavior above.
	 */
	efr32_qemu_tifs_transition_requested = false;
	efr32_radio_timer.hcto_requested = true;
	efr32_qemu_tifs_arm(hcto);
#endif
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_aa_capture(void)
{
	efr32_radio_timer_state_aa_capture_request(&efr32_radio_timer);
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_aa_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_aa_save(uint32_t aa)
{
	efr32_radio_timer_state_aa_save(&efr32_radio_timer, aa);
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_aa_restore(void)
{
	return efr32_radio_timer.saved_aa;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_ready_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_end_capture(void)
{
	efr32_radio_timer_state_end_capture_request(&efr32_radio_timer);
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_end_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
uint32_t radio_tmr_tifs_base_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_sample(void)
{
	EFR32_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_sample_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_pa_setup(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_lna_setup(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_lna_on(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_lna_off(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_pa_lna_enable(uint32_t trx_us)
{
	ARG_UNUSED(trx_us);
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_pa_lna_disable(void)
{
	/* No front-end module on the target board. */
}

/* [software] Synchronous BLE AES-CCM through PSA. */
void *radio_ccm_rx_pkt_set(struct ccm *ccm, uint8_t phy, void *pkt)
{
	ARG_UNUSED(phy);
	memcpy(&ccm_ctx.config, ccm, sizeof(*ccm));
	ccm_ctx.rx_in = (struct pdu_data *)pkt_scratch;
	ccm_ctx.rx_out = pkt;
	ccm_ctx.pending_rx = true;
	ccm_ctx.done = false;
	ccm_ctx.mic_valid = false;
	ccm_ctx.empty = false;

	return pkt_scratch;
}

/* [software] Encrypts synchronously; returned storage remains valid until the next radio packet. */
void *radio_ccm_tx_pkt_set(struct ccm *ccm, void *pkt)
{
	struct pdu_data *in = pkt;
	uint8_t nonce[BLE_CCM_NONCE_SIZE];
	uint8_t aad = *(uint8_t *)pkt & BLE_CCM_AAD_MASK;
	psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, BLE_CCM_MIC_SIZE);
	psa_key_id_t id;
	size_t out_len;
	psa_status_t status;

	ccm_ctx.pending_rx = false;
	ccm_ctx.done = false;
	ccm_ctx.mic_valid = false;
	ccm_ctx.empty = false;
	if (in->len == 0U) {
		ccm_ctx.done = true;
		return pkt;
	}

	id = ccm_key_import(ccm, PSA_KEY_USAGE_ENCRYPT);
	if (id == 0) {
		return NULL;
	}
	ccm_nonce_build(ccm, nonce);
	status = psa_aead_encrypt(id, alg, nonce, sizeof(nonce), &aad, sizeof(aad),
				  (uint8_t *)pkt + 2, in->len, pkt_scratch + 2,
				  sizeof(pkt_scratch) - 2, &out_len);
	(void)psa_destroy_key(id);
	if (status != PSA_SUCCESS || out_len != in->len + BLE_CCM_MIC_SIZE) {
		return NULL;
	}

	pkt_scratch[0] = *(uint8_t *)pkt;
	pkt_scratch[1] = in->len + BLE_CCM_MIC_SIZE;
	ccm_ctx.done = true;
	return pkt_scratch;
}

/* [software] Completes a pending receive decrypt synchronously. */
uint32_t radio_ccm_is_done(void)
{
	uint8_t nonce[BLE_CCM_NONCE_SIZE];
	uint8_t aad;
	psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, BLE_CCM_MIC_SIZE);
	psa_key_id_t id;
	size_t out_len;
	psa_status_t status;

	if (!ccm_ctx.pending_rx) {
		return ccm_ctx.done;
	}
	ccm_ctx.pending_rx = false;
	if (ccm_ctx.rx_in->len == 0U) {
		memcpy(ccm_ctx.rx_out, ccm_ctx.rx_in, 2);
		ccm_ctx.empty = true;
		ccm_ctx.mic_valid = true;
		ccm_ctx.done = true;
		return 1;
	}
	if (ccm_ctx.rx_in->len <= BLE_CCM_MIC_SIZE) {
		return 0;
	}

	id = ccm_key_import(&ccm_ctx.config, PSA_KEY_USAGE_DECRYPT);
	if (id == 0) {
		return 0;
	}
	ccm_nonce_build(&ccm_ctx.config, nonce);
	aad = *(uint8_t *)ccm_ctx.rx_in & BLE_CCM_AAD_MASK;
	status = psa_aead_decrypt(id, alg, nonce, sizeof(nonce), &aad, sizeof(aad),
				  (uint8_t *)ccm_ctx.rx_in + 2, ccm_ctx.rx_in->len,
				  (uint8_t *)ccm_ctx.rx_out + 2,
				  ccm_ctx.rx_in->len - BLE_CCM_MIC_SIZE, &out_len);
	(void)psa_destroy_key(id);
	if (status != PSA_SUCCESS || out_len != ccm_ctx.rx_in->len - BLE_CCM_MIC_SIZE) {
		return 0;
	}

	*(uint8_t *)ccm_ctx.rx_out = *(uint8_t *)ccm_ctx.rx_in;
	ccm_ctx.rx_out->len = out_len;
	ccm_ctx.mic_valid = true;
	ccm_ctx.done = true;
	return 1;
}

/* [software] */
uint32_t radio_ccm_mic_is_valid(void)
{
	return ccm_ctx.mic_valid;
}

/* [software] OpenISA uses this to report an empty received PDU. */
uint32_t radio_ccm_is_available(void)
{
	return ccm_ctx.empty;
}

/* [software] Snapshot AAR-order IRKs for the next normalized advertising PDU. */
void radio_ar_configure(uint32_t nirk, void *irk)
{
#if defined(CONFIG_BT_CTLR_PRIVACY)
	uint32_t count = MIN(nirk, (uint32_t)CONFIG_BT_CTLR_RL_SIZE);
	uint32_t i;

	LL_ASSERT(irk != NULL || count == 0U);
	for (i = 0U; i < count; i++) {
		sys_memcpy_swap(efr32_radio_ar.irks[i],
				(uint8_t *)irk + (i * sizeof(efr32_radio_ar.irks[i])),
				sizeof(efr32_radio_ar.irks[i]));
	}
	efr32_radio_ar.count = (uint8_t)count;
	efr32_radio_ar.match = EFR32_RADIO_AR_NO_MATCH;
	efr32_radio_ar.armed = true;
#else
	ARG_UNUSED(nirk);
	ARG_UNUSED(irk);
#endif
}

/* [software] Return the index in the last configuration snapshot. */
uint32_t radio_ar_match_get(void)
{
#if defined(CONFIG_BT_CTLR_PRIVACY)
	return efr32_radio_ar.match;
#else
	return 0U;
#endif
}

/* [software] Disarm matching and discard the last result. */
void radio_ar_status_reset(void)
{
#if defined(CONFIG_BT_CTLR_PRIVACY)
	efr32_radio_ar.armed = false;
	efr32_radio_ar.match = EFR32_RADIO_AR_NO_MATCH;
#endif
}

/* [software] A match exists only after normalized-PDU delivery ran ah(). */
uint32_t radio_ar_has_match(void)
{
#if defined(CONFIG_BT_CTLR_PRIVACY)
	return efr32_radio_ar.match != EFR32_RADIO_AR_NO_MATCH;
#else
	return 0U;
#endif
}

/* [software] Immediate reference-counted availability; hardware sleep is unknown (T1, T2). */
uint32_t radio_sleep(void)
{
	atomic_val_t previous;

	/* Do not underflow: callers use -EALREADY to diagnose unmatched sleep. */
	do {
		previous = atomic_get(&efr32_radio_power_ref);
		if (previous == 0) {
			return (uint32_t)-EALREADY;
		}
	} while (!atomic_cas(&efr32_radio_power_ref, previous, previous - 1));

	return 0;
}

/* [software] Immediate reference-counted availability; hardware wake is unknown (T1, T2). */
uint32_t radio_wake(void)
{
	atomic_inc(&efr32_radio_power_ref);
	return 0;
}

/* [software] This is deliberately not inferred from RAC STATUS.STATE. */
uint32_t radio_is_off(void)
{
	return atomic_get(&efr32_radio_power_ref) == 0;
}
