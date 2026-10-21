/*
 * Lower-link-layer execution contexts in one meta-IRQ thread, and the mayfly glue that schedules
 * them. SPDX-License-Identifier: Apache-2.0
 *
 * Replaces Zephyr's OpenISA RV32M1 mayfly.c, which ran the LLL and ULL contexts in software
 * interrupts and gated them by enabling/disabling those IRQs. Here the software interrupts only
 * ring a doorbell; the context bodies run in the thread, and gating is a flag the thread honours.
 *
 * UNVALIDATED ON HARDWARE.
 */
#include <zephyr/kernel.h>
#include <soc.h>

#include "util/memq.h"
#include "util/mayfly.h"
#include "hal/debug.h"
#include "ll_sw/lll.h"
#include "hal/efr32/swi.h"
#include "hal/efr32/ll_threads.h"
#include "hal/efr32/cntr_efr32.h"

/*
 * Two contexts, one per software interrupt. ULL high and low share the worker interrupt in this
 * configuration (hal/efr32/swi.h), so they share its context and its gate. All contexts share one
 * priority, which mayfly_prio_is_equal() reports: like the equal-priority interrupts they replace
 * they never preempt each other, so one thread runs them all, LLL first.
 */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_CTLR_LOW_LAT));
/* ull_reset() must wait for the LLL reset mayfly; ZLI assumes it ran inline. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_CTLR_ZLI), "set CONFIG_BT_CTLR_ZLI=n");
BUILD_ASSERT(CONFIG_BT_CTLR_LLL_PRIO == CONFIG_BT_CTLR_ULL_HIGH_PRIO &&
	     CONFIG_BT_CTLR_ULL_HIGH_PRIO == CONFIG_BT_CTLR_ULL_LOW_PRIO,
	     "the lower link layer's contexts share one thread priority");

struct ll_context {
	void (*body)(const void *arg);
	/* Rung by the doorbell, consumed by the thread. */
	bool pending;
	/* Gate set by mayfly_enable(); a run requested while closed waits for it. */
	bool enabled;
	bool deferred;
};

static K_DEADLINE_EVENT_DEFINE(ll_event);
static struct ll_context lll_ctx = {
	.enabled = true,
};
static struct ll_context worker_ctx = {
	.enabled = true,
};

static void ll_context_post(struct ll_context *ctx, int deadline)
{
	unsigned int key = irq_lock();

	ctx->pending = true;
	irq_unlock(key);
	k_deadline_event_post(&ll_event, deadline);
}

/* Consume one pending run of @p ctx; returns the body to run, if its gate is open. */
static void (*ll_context_take(struct ll_context *ctx))(const void *arg)
{
	void (*body)(const void *arg) = NULL;
	unsigned int key = irq_lock();

	if (ctx->pending) {
		ctx->pending = false;
		if (ctx->enabled) {
			body = ctx->body;
		} else {
			ctx->deferred = true;
		}
	}
	irq_unlock(key);
	return body;
}

static void ll_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		void (*body)(const void *arg);

		k_deadline_event_wait(&ll_event);
		/* The ticker deadline bounds the worker work this pass is about to run. */
		efr32_radio_deadline_check((int)cntr_ticker_deadline(), "ll thread");
		/*
		 * Each pass serves both contexts, LLL first. Serving the LLL until it is idle would
		 * starve the worker, whose ticker job drains the user operations the LLL queues. A
		 * doorbell rung while a body runs is served before waiting again.
		 */
		do {
			body = ll_context_take(&lll_ctx);
			if (body != NULL) {
				body(NULL);
			}
			body = ll_context_take(&worker_ctx);
			if (body != NULL) {
				body(NULL);
			}
		} while (lll_ctx.pending || worker_ctx.pending);
	}
}

K_THREAD_DEFINE(efr32_ll_thread, CONFIG_EFR32_RADIO_LL_STACK_SIZE, ll_thread, NULL, NULL, NULL,
		EFR32_LL_THREAD_PRIO, 0, 0);

void efr32_ll_threads_bind(void (*worker)(const void *arg), void (*lll)(const void *arg))
{
	unsigned int key = irq_lock();

	worker_ctx.body = worker;
	lll_ctx.body = lll;
	irq_unlock(key);
}

/* Doorbells: only make the context runnable. */
void efr32_ll_worker_doorbell_isr(const void *arg)
{
	ARG_UNUSED(arg);
	ll_context_post(&worker_ctx, (int)cntr_ticker_deadline());
}

void efr32_ll_lll_doorbell_isr(const void *arg)
{
	ARG_UNUSED(arg);
	ll_context_post(&lll_ctx, (int)cntr_launch_deadline());
}

static struct ll_context *callee_context(uint8_t callee_id)
{
	switch (callee_id) {
	case TICKER_USER_ID_LLL:
		return &lll_ctx;
	case TICKER_USER_ID_ULL_HIGH:
	case TICKER_USER_ID_ULL_LOW:
		return &worker_ctx;
	default:
		LL_ASSERT(0);
		return NULL;
	}
}

void mayfly_enable_cb(uint8_t caller_id, uint8_t callee_id, uint8_t enable)
{
	struct ll_context *ctx = callee_context(callee_id);
	bool run;
	unsigned int key;

	ARG_UNUSED(caller_id);
	key = irq_lock();
	ctx->enabled = enable != 0U;
	run = ctx->enabled && ctx->deferred;
	if (run) {
		ctx->deferred = false;
	}
	irq_unlock(key);
	if (run) {
		/* Like re-enabling a pended IRQ: the deferred run happens now. */
		mayfly_pend(caller_id, callee_id);
	}
}

uint32_t mayfly_is_enabled(uint8_t caller_id, uint8_t callee_id)
{
	ARG_UNUSED(caller_id);
	return callee_context(callee_id)->enabled ? 1U : 0U;
}

uint32_t mayfly_prio_is_equal(uint8_t caller_id, uint8_t callee_id)
{
	/* All three contexts share one priority; thread callers are lower. */
	return (caller_id == callee_id) ||
	       ((caller_id != TICKER_USER_ID_THREAD) && (callee_id != TICKER_USER_ID_THREAD));
}

void mayfly_pend(uint8_t caller_id, uint8_t callee_id)
{
	ARG_UNUSED(caller_id);
	if (callee_context(callee_id) == &lll_ctx) {
		hal_swi_lll_pend();
	} else {
		hal_swi_worker_pend();
	}
}

uint32_t mayfly_is_running(void)
{
	/* Every lower-link-layer context, radio jobs included, runs at this priority. */
	return !k_is_in_isr() && k_thread_priority_get(k_current_get()) == EFR32_LL_THREAD_PRIO;
}
