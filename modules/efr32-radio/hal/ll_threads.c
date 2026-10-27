/*
 * Lower-link-layer execution contexts in scheduler-owned meta-IRQ threads, and the mayfly glue
 * that releases them. SPDX-License-Identifier: Apache-2.0
 *
 * Replaces Zephyr's OpenISA RV32M1 mayfly.c, which ran the LLL and ULL contexts in software
 * interrupts and gated them by enabling/disabling those IRQs. Here the software interrupts only
 * ring a doorbell; each context body runs in its own persistent thread, and gating is a flag the
 * thread honours. The radio module publishes readiness and deadlines but never chooses between
 * ready contexts: that policy belongs to the Zephyr scheduler.
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

#if defined(CONFIG_EFR32_RADIO_BRINGUP_TRACE)
#define EFR32_LL_TRACE_ONCE(format, ...) \
	do { \
		static bool efr32_trace_once_; \
		if (!efr32_trace_once_) { \
			efr32_trace_once_ = true; \
			printk("efr32-radio trace: " format "\n", ##__VA_ARGS__); \
		} \
	} while (0)
#else
#define EFR32_LL_TRACE_ONCE(...) do { } while (0)
#endif

/*
 * Two contexts, one per software interrupt. ULL high and low share the worker interrupt in this
 * configuration (hal/efr32/swi.h), so they share its context and its gate. All contexts share one
 * priority, which mayfly_prio_is_equal() reports: like the equal-priority interrupts they replace
 * they never preempt each other. Separate threads let the kernel order ready work by deadline.
 */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_CTLR_LOW_LAT));
/* ull_reset() must wait for the LLL reset mayfly; ZLI assumes it ran inline. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_CTLR_ZLI), "set CONFIG_BT_CTLR_ZLI=n");
BUILD_ASSERT(CONFIG_BT_CTLR_LLL_PRIO == CONFIG_BT_CTLR_ULL_HIGH_PRIO &&
	     CONFIG_BT_CTLR_ULL_HIGH_PRIO == CONFIG_BT_CTLR_ULL_LOW_PRIO,
	     "the lower link layer's contexts share one thread priority");

struct ll_context {
	void (*body)(const void *arg);
	struct k_deadline_event event;
	const char *name;
	/* Rung by the doorbell, consumed by the thread. */
	bool pending;
	/* Gate set by mayfly_enable(); a run requested while closed waits for it. */
	bool enabled;
	bool deferred;
};

static struct ll_context lll_ctx = {
	.event = Z_DEADLINE_EVENT_INITIALIZER,
	.name = "lll thread",
	.enabled = true,
};
static struct ll_context worker_ctx = {
	.event = Z_DEADLINE_EVENT_INITIALIZER,
	.name = "ull worker thread",
	.enabled = true,
};

static void ll_context_post(struct ll_context *ctx, int deadline)
{
	unsigned int key = irq_lock();

	ctx->pending = true;
	irq_unlock(key);
	k_deadline_event_post(&ctx->event, deadline);
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

static void ll_context_thread(void *context, void *b, void *c)
{
	struct ll_context *ctx = context;

	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		void (*body)(const void *arg);

		k_deadline_event_wait(&ctx->event);
		EFR32_LL_TRACE_ONCE("%s wake", ctx->name);
		efr32_radio_deadline_check(ctx == &lll_ctx ? (int)cntr_launch_deadline() :
						      (int)cntr_ticker_deadline(), ctx->name);
		/*
		 * Coalesce repeated doorbells, but run again when one arrives while the body executes.
		 * Ordering against the other context and radio jobs is exclusively scheduler policy.
		 */
		do {
			body = ll_context_take(ctx);
			if (body != NULL) {
				body(NULL);
			}
		} while (ctx->pending);
	}
}

K_THREAD_DEFINE(efr32_lll_thread, CONFIG_EFR32_RADIO_LLL_STACK_SIZE, ll_context_thread,
		&lll_ctx, NULL, NULL, EFR32_LL_THREAD_PRIO, 0, 0);
K_THREAD_DEFINE(efr32_ull_worker_thread, CONFIG_EFR32_RADIO_ULL_STACK_SIZE, ll_context_thread,
		&worker_ctx, NULL, NULL, EFR32_LL_THREAD_PRIO, 0, 0);

void efr32_ll_threads_bind(void (*worker)(const void *arg), void (*lll)(const void *arg))
{
	unsigned int key = irq_lock();

	worker_ctx.body = worker;
	lll_ctx.body = lll;
	irq_unlock(key);
	EFR32_LL_TRACE_ONCE("ll-bind worker=%p lll=%p", worker, lll);
}

/* Doorbells: only make the context runnable. */
void efr32_ll_worker_doorbell_isr(const void *arg)
{
	ARG_UNUSED(arg);
	EFR32_LL_TRACE_ONCE("worker-doorbell");
	ll_context_post(&worker_ctx, (int)cntr_ticker_deadline());
}

void efr32_ll_lll_doorbell_isr(const void *arg)
{
	ARG_UNUSED(arg);
	EFR32_LL_TRACE_ONCE("lll-doorbell");
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
	EFR32_LL_TRACE_ONCE("mayfly-pend caller=%u callee=%u", caller_id, callee_id);
	if (callee_context(callee_id) == &lll_ctx) {
		EFR32_LL_TRACE_ONCE("mayfly-pend-lll caller=%u", caller_id);
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
