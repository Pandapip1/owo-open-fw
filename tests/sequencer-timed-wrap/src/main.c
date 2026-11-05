/*
 * The open radio sequencer's timed command (hal/efr32/seq_abi.h, docs/radio-tables.md 7.82)
 * decides "due" and "past LIMIT" on TIMER0 counts, which wrap. Each case posts SWRXEN with a
 * TARGET and LIMIT around TIMER0 = 2^31 or 2^32 and checks what the sequencer reports in DONE.
 * The RAC stays forced disabled, so the command reaches no radio state.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <soc.h>
#include "hal/efr32/seq_abi.h"

extern const uint8_t efr32_radio_seq_image[];
extern const size_t efr32_radio_seq_image_size;

/* Register map and install sequence of hal/radio.c (efr32_radio_sequencer_install(), radio_reset()). */
#define RAC_BASE 0xB8020000UL
#define RAC_CTRL_OFFSET 0x014U
#define RAC_SEQCTRL_OFFSET 0x038U
#define RAC_CTRL_FORCEDISABLE BIT(0)
#define RAC_CTRL_ACTIVE_HIGH BIT(7)
#define RAC_CTRL_PA_ENABLE_HIGH BIT(8)
#define RAC_CTRL_LNA_ENABLE_HIGH BIT(9)
#define RAC_CTRL_SEQRESET BIT(24)
#define RAC_CTRL_CPUWAITDIS BIT(26)
#define RAC_SEQCTRL_STAGE2_STARTUP 0x00000011U
#define REGISTER_SET_OFFSET 0x1000U
#define SEQ_RAM_BASE 0xB0000000UL
#define SEQ_IDENTITY_ADDRESS 0xB0004FE0UL
#define SEQ_IDENTITY_OPEN_V1 0x4F535131U
#define SEQ_RX2TX_DECISION_ADDRESS 0xB0004FA4UL
#define SEQ_M33_RADIO_ALIAS 0x10000000UL
#define SEQ_MAILBOX_FLAGS_SET \
	(EFR32_SEQ_RDMAILBOX1_BASE + SEQ_M33_RADIO_ALIAS + REGISTER_SET_OFFSET + \
	 EFR32_SEQ_RDMAILBOX_FLAGS)
#define SEQ_MAILBOX_IEN (EFR32_SEQ_RDMAILBOX1_BASE + SEQ_M33_RADIO_ALIAS + EFR32_SEQ_RDMAILBOX_IEN)
/* The sequencer's identity handshake bound in radio.c. */
#define SEQ_READY_TIMEOUT_US 10000U

/* TIMER0 counts a case starts from below a boundary, and the instant's lead over the start. */
#define BEFORE 0x10000U
#define LEAD 0x400U
/* How far past LIMIT a late case starts. */
#define PAST 0x1000U
/* The longest a case may take: its lead and the window, at the slowest TIMER0 rate here. */
#define CASE_TIMEOUT_US 100000U
/* Poll step: a few system-clock (BURTC) cycles. */
#define POLL_US 100U

static volatile uint32_t *reg(uintptr_t address)
{
	return (volatile uint32_t *)address;
}

static volatile uint32_t *timed(uint32_t offset)
{
	return reg(SEQ_RAM_BASE + EFR32_SEQ_TIMED_BLOCK + offset);
}

static void timer0_start_at(uint32_t count)
{
	TIMER0->CMD_SET = TIMER_CMD_STOP;
	TIMER0->CNT = count;
	TIMER0->CMD_SET = TIMER_CMD_START;
}

static uint32_t timer0_now(void)
{
	return TIMER0->CNT;
}

static uint16_t generation;

/* Post SWRXEN due at @p target, issuable up to @p limit; returns DONE. */
static uint32_t post(uint32_t target, uint32_t limit)
{
	uint32_t op = EFR32_SEQ_TIMED_OP_SWRXEN |
		      ((uint32_t)++generation << EFR32_SEQ_TIMED_GENERATION_SHIFT);
	uint32_t done = 0U;

	*timed(EFR32_SEQ_TIMED_TARGET) = target;
	*timed(EFR32_SEQ_TIMED_LIMIT) = limit;
	*timed(EFR32_SEQ_TIMED_DONE) = 0U;
	__DSB();
	*timed(EFR32_SEQ_TIMED_OP) = op;
	__DSB();
	*reg(SEQ_MAILBOX_FLAGS_SET) = EFR32_SEQ_RDMAILBOX_TIMED_FLAG;
	for (uint32_t us = 0U; us < CASE_TIMEOUT_US && done == 0U; us += POLL_US) {
		k_busy_wait(POLL_US);
		done = *timed(EFR32_SEQ_TIMED_DONE);
	}
	zassert_not_equal(done, 0U, "the sequencer did not act on the command");
	zassert_equal(done & ~(EFR32_SEQ_TIMED_LATE | EFR32_SEQ_TIMED_CANCELLED), op,
		      "DONE echoes another command");
	return done;
}

/* Issued at or after @p target, by @p limit. */
static void expect_issued(uint32_t start, uint32_t target, uint32_t limit)
{
	timer0_start_at(start);

	uint32_t done = post(target, limit);
	uint32_t at = *timed(EFR32_SEQ_TIMED_AT);

	zassert_equal(done & (EFR32_SEQ_TIMED_LATE | EFR32_SEQ_TIMED_CANCELLED), 0U,
		      "start 0x%08x target 0x%08x limit 0x%08x: not issued (DONE 0x%08x)",
		      start, target, limit, done);
	zassert_true((int32_t)(at - target) >= 0, "issued at 0x%08x, before 0x%08x", at, target);
	zassert_true((int32_t)(limit - at) >= 0, "issued at 0x%08x, after 0x%08x", at, limit);
}

/* Refused: the count is already past @p limit. */
static void expect_late(uint32_t start, uint32_t target, uint32_t limit)
{
	timer0_start_at(start);

	uint32_t done = post(target, limit);

	zassert_true((int32_t)(timer0_now() - limit) > 0);
	zassert_not_equal(done & EFR32_SEQ_TIMED_LATE, 0U,
			  "start 0x%08x target 0x%08x limit 0x%08x: issued past LIMIT (DONE 0x%08x)",
			  start, target, limit, done);
}

/* Due before @p boundary, issuable until after it: the check runs before the boundary. */
static void window_across(uint32_t boundary)
{
	uint32_t start = boundary - BEFORE;

	expect_issued(start, start + LEAD, boundary + LEAD);
}

/* Due before @p boundary, still waiting when TIMER0 crosses it. */
static void instant_across(uint32_t boundary)
{
	uint32_t start = boundary - LEAD;

	expect_issued(start, boundary + LEAD, boundary + BEFORE);
}

/* LIMIT before @p boundary, the count already past it. */
static void late_across(uint32_t boundary)
{
	expect_late(boundary + PAST, boundary - BEFORE, boundary - PAST);
}

#define HALF_RANGE 0x80000000U
#define FULL_RANGE 0x00000000U

ZTEST(sequencer_timed_wrap, test_window_across_2_31)
{
	window_across(HALF_RANGE);
}

ZTEST(sequencer_timed_wrap, test_instant_across_2_31)
{
	instant_across(HALF_RANGE);
}

ZTEST(sequencer_timed_wrap, test_late_across_2_31)
{
	late_across(HALF_RANGE);
}

ZTEST(sequencer_timed_wrap, test_window_across_2_32)
{
	window_across(FULL_RANGE);
}

ZTEST(sequencer_timed_wrap, test_instant_across_2_32)
{
	instant_across(FULL_RANGE);
}

ZTEST(sequencer_timed_wrap, test_late_across_2_32)
{
	late_across(FULL_RANGE);
}

static void *setup(void)
{
	volatile uint8_t *destination = (volatile uint8_t *)SEQ_RAM_BASE;
	uint32_t identity = 0U;

	CMU->RADIOCLKCTRL_SET = CMU_RADIOCLKCTRL_EN;
	/* Every radio bus clock efr32_radio_bus_clocks_on() enables: the handlers read them all. */
	CMU->CLKEN1_SET = CMU_CLKEN1_AGC | CMU_CLKEN1_MODEM | CMU_CLKEN1_RFCRC |
			  CMU_CLKEN1_FRC | CMU_CLKEN1_PROTIMER | CMU_CLKEN1_RAC |
			  CMU_CLKEN1_SYNTH | CMU_CLKEN1_BUFC | CMU_CLKEN1_RDMAILBOX1;
	CMU->CLKEN0_SET = CMU_CLKEN0_TIMER0;
	TIMER0->EN_SET = TIMER_EN_EN;
	TIMER0->TOP = UINT32_MAX;

	*reg(RAC_BASE + RAC_CTRL_OFFSET) = RAC_CTRL_ACTIVE_HIGH | RAC_CTRL_PA_ENABLE_HIGH |
					   RAC_CTRL_LNA_ENABLE_HIGH | RAC_CTRL_SEQRESET |
					   RAC_CTRL_FORCEDISABLE;
	__DSB();
	for (size_t i = 0U; i < efr32_radio_seq_image_size; i++) {
		destination[i] = efr32_radio_seq_image[i];
	}
	*reg(SEQ_IDENTITY_ADDRESS) = 0U;
	*reg(SEQ_RX2TX_DECISION_ADDRESS) = 0U;
	__DSB();
	*reg(RAC_BASE + RAC_SEQCTRL_OFFSET) = RAC_SEQCTRL_STAGE2_STARTUP;
	*reg(RAC_BASE + RAC_CTRL_OFFSET) = RAC_CTRL_ACTIVE_HIGH | RAC_CTRL_PA_ENABLE_HIGH |
					   RAC_CTRL_LNA_ENABLE_HIGH | RAC_CTRL_FORCEDISABLE |
					   RAC_CTRL_CPUWAITDIS;
	__DSB();
	for (uint32_t us = 0U; us < SEQ_READY_TIMEOUT_US && identity != SEQ_IDENTITY_OPEN_V1;
	     us += POLL_US) {
		k_busy_wait(POLL_US);
		identity = *reg(SEQ_IDENTITY_ADDRESS);
	}
	zassert_equal(identity, SEQ_IDENTITY_OPEN_V1, "the open sequencer did not start");
	/* Identity comes first; the reset path then clears the mailbox and enables its IRQ. */
	for (uint32_t us = 0U; us < SEQ_READY_TIMEOUT_US &&
	     (*reg(SEQ_MAILBOX_IEN) & EFR32_SEQ_RDMAILBOX_TIMED_FLAG) == 0U; us += POLL_US) {
		k_busy_wait(POLL_US);
	}
	zassert_not_equal(*reg(SEQ_MAILBOX_IEN) & EFR32_SEQ_RDMAILBOX_TIMED_FLAG, 0U,
			  "the sequencer did not open its mailbox");
	return NULL;
}

ZTEST_SUITE(sequencer_timed_wrap, NULL, setup, NULL, NULL, NULL);
