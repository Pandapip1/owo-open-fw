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
#include "lll/lll_vendor.h"
#include "hal/ticker.h"
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
 * configuration (hal/efr32/swi.h), so they share its context and its gate. The LLL context runs at
 * CONFIG_BT_CTLR_LLL_PRIO with the radio's jobs, the worker at CONFIG_BT_CTLR_ULL_HIGH_PRIO
 * (hal/efr32/ll_threads.h); mayfly_prio_is_equal() reports exactly that split, like the Nordic
 * glue, so the mayfly layer only runs a callee inline when it cannot preempt or be preempted by
 * its caller.
 */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_CTLR_LOW_LAT));
/* ull_reset() must wait for the LLL reset mayfly; ZLI assumes it ran inline. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_CTLR_ZLI), "set CONFIG_BT_CTLR_ZLI=n");

struct ll_context {
	void (*body)(const void *arg);
	struct k_deadline_event event;
	const char *name;
	/* Gate set by mayfly_enable(); a run requested while closed waits for it. */
	bool enabled;
	bool deferred;
};

static struct ll_context lll_ctx = {
	.event = Z_DEADLINE_EVENT_INITIALIZER_NAMED("lll context"),
	.name = "lll thread",
	.enabled = true,
};
static struct ll_context worker_ctx = {
	.event = Z_DEADLINE_EVENT_INITIALIZER_NAMED("ull worker context"),
	.name = "ull worker thread",
	.enabled = true,
};

/*
 * Release @p ctx for one run that must start by @p deadline. The kernel checks that start only
 * when the release finds the context idle (CONFIG_SCHED_DEADLINE_MISS_POLICY). One that arrives
 * while a body runs may be served by that body: ticker_worker() reads the counter at entry,
 * mayfly_run() drains its queues and, with BT_MAYFLY_YIELD_AFTER_CALL, pends its own
 * continuation. It still orders the next run, which the kernel runs under its deadline.
 */
/* Set by efr32_ll_threads_stop(): no release may resume a stopped context. */
static atomic_t ll_stopped;

static void ll_context_post(struct ll_context *ctx, int deadline)
{
	if (atomic_get(&ll_stopped) == 0) {
		k_deadline_event_post(&ctx->event, deadline);
	}
}

/* Consume one run of @p ctx; returns the body to run, if its gate is open. */
static void (*ll_context_take(struct ll_context *ctx))(const void *arg)
{
	void (*body)(const void *arg) = NULL;
	unsigned int key = irq_lock();

	if (ctx->enabled) {
		body = ctx->body;
	} else {
		ctx->deferred = true;
	}
	irq_unlock(key);
	return body;
}

/*
 * When the worker run in progress fell due (its ticker compare or doorbell), and the earliest
 * such instant among the runs that have pended LLL work since the LLL doorbell last rang: the
 * LLL work a worker run requests serves the event that run prepares, so it is due when the run
 * was (hal/efr32/ll_deadlines.h, prepare chain).
 */
static uint32_t worker_run_due;
static uint32_t lll_due;
static bool lll_due_valid;

static void ll_context_thread(void *context, void *b, void *c)
{
	struct ll_context *ctx = context;

	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		void (*body)(const void *arg);

		/*
		 * One run per wait. A doorbell rung while the body executes stays latched in the
		 * event with its own deadline, so the next wait returns at once under that key,
		 * after any equal-priority job due sooner; the run never inherits this one's key.
		 */
		k_deadline_event_wait(&ctx->event);
		EFR32_LL_TRACE_ONCE("%s wake", ctx->name);
		if (ctx == &worker_ctx) {
			worker_run_due = (uint32_t)k_current_get()->base.prio_deadline -
					 EFR32_DL_ULL_WINDOW_CYC;
		}
		body = ll_context_take(ctx);
		if (body != NULL) {
			body(NULL);
		}
	}
}

K_THREAD_DEFINE(efr32_lll_thread, CONFIG_EFR32_RADIO_LLL_STACK_SIZE, ll_context_thread,
		&lll_ctx, NULL, NULL, EFR32_LLL_THREAD_PRIO, 0, 0);
K_THREAD_DEFINE(efr32_ull_worker_thread, CONFIG_EFR32_RADIO_ULL_STACK_SIZE, ll_context_thread,
		&worker_ctx, NULL, NULL, EFR32_ULL_THREAD_PRIO, 0, 0);

void efr32_ll_threads_bind(void (*worker)(const void *arg), void (*lll)(const void *arg))
{
	unsigned int key = irq_lock();

	worker_ctx.body = worker;
	lll_ctx.body = lll;
	irq_unlock(key);
	EFR32_LL_TRACE_ONCE("ll-bind worker=%p lll=%p", worker, lll);
}

/*
 * Doorbells: only make the context runnable. The ticker compare releases the worker itself,
 * due at the compare; a worker doorbell (a mayfly, a re-enabled gate) is due when it rings, and
 * its run gets the same window, since it may serve ticker work just as well. Either run must
 * start in time for a radio event prepared EVENT_OVERHEAD_XTAL_US + EVENT_OVERHEAD_START_US
 * ahead (EFR32_DL_ULL_WINDOW_CYC). LLL work is due when the worker run that requested it was,
 * or when its doorbell rings if a thread requested it.
 */
void efr32_ll_worker_doorbell_isr(const void *arg)
{
	ARG_UNUSED(arg);
	EFR32_LL_TRACE_ONCE("worker-doorbell");
	ll_context_post(&worker_ctx, (int)(k_cycle_get_32() + EFR32_DL_ULL_WINDOW_CYC));
}

void efr32_ll_lll_doorbell_isr(const void *arg)
{
	uint32_t due = k_cycle_get_32();
	unsigned int key;

	ARG_UNUSED(arg);
	EFR32_LL_TRACE_ONCE("lll-doorbell");
	key = irq_lock();
	if (lll_due_valid) {
		due = lll_due;
		lll_due_valid = false;
	}
	irq_unlock(key);
	ll_context_post(&lll_ctx, (int)(due + EFR32_DL_LLL_WINDOW_CYC));
}

void efr32_ll_worker_release_due(uint32_t due)
{
	ll_context_post(&worker_ctx, (int)(due + EFR32_DL_ULL_WINDOW_CYC));
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
	/*
	 * As in the Nordic glue (ll_sw/nordic/hal/nrf5/mayfly.c) without ZLI: contexts at one
	 * priority, and only those, run each other's mayflies inline. Thread callers are lower.
	 */
	return (caller_id == callee_id) ||
#if (CONFIG_BT_CTLR_LLL_PRIO == CONFIG_BT_CTLR_ULL_HIGH_PRIO)
	       ((caller_id == TICKER_USER_ID_LLL) && (callee_id == TICKER_USER_ID_ULL_HIGH)) ||
	       ((caller_id == TICKER_USER_ID_ULL_HIGH) && (callee_id == TICKER_USER_ID_LLL)) ||
#endif
#if (CONFIG_BT_CTLR_LLL_PRIO == CONFIG_BT_CTLR_ULL_LOW_PRIO)
	       ((caller_id == TICKER_USER_ID_LLL) && (callee_id == TICKER_USER_ID_ULL_LOW)) ||
	       ((caller_id == TICKER_USER_ID_ULL_LOW) && (callee_id == TICKER_USER_ID_LLL)) ||
#endif
	       ((caller_id == TICKER_USER_ID_ULL_HIGH) && (callee_id == TICKER_USER_ID_ULL_LOW)) ||
	       ((caller_id == TICKER_USER_ID_ULL_LOW) && (callee_id == TICKER_USER_ID_ULL_HIGH));
}

void mayfly_pend(uint8_t caller_id, uint8_t callee_id)
{
	EFR32_LL_TRACE_ONCE("mayfly-pend caller=%u callee=%u", caller_id, callee_id);
	if (callee_context(callee_id) == &lll_ctx) {
		EFR32_LL_TRACE_ONCE("mayfly-pend-lll caller=%u", caller_id);
		if (!k_is_in_isr() && k_current_get() == efr32_ull_worker_thread) {
			unsigned int key = irq_lock();

			if (!lll_due_valid || (int32_t)(worker_run_due - lll_due) < 0) {
				lll_due = worker_run_due;
				lll_due_valid = true;
			}
			irq_unlock(key);
		}
		hal_swi_lll_pend();
	} else {
		hal_swi_worker_pend();
	}
}

uint32_t mayfly_is_running(void)
{
	int prio;

	if (k_is_in_isr()) {
		return 0U;
	}
	/* The contexts, radio jobs included, are the only threads at these priorities. */
	prio = k_thread_priority_get(k_current_get());
	return (prio == EFR32_LLL_THREAD_PRIO) || (prio == EFR32_ULL_THREAD_PRIO);
}

bool efr32_ll_threads_stop(void)
{
	const k_tid_t contexts[] = { efr32_lll_thread, efr32_ull_worker_thread };
	bool current = false;
	unsigned int key = irq_lock();

	/* No doorbell, deferred run or latched publication may start another body. */
	(void)atomic_set(&ll_stopped, 1);
	irq_disable(HAL_SWI_RADIO_IRQ);
	irq_disable(HAL_SWI_WORKER_IRQ);
	lll_ctx.enabled = false;
	worker_ctx.enabled = false;
	lll_ctx.deferred = false;
	worker_ctx.deferred = false;
	k_deadline_event_reset(&lll_ctx.event);
	k_deadline_event_reset(&worker_ctx.event);
	irq_unlock(key);
	ARRAY_FOR_EACH(contexts, i) {
		if (contexts[i] == k_current_get() && !k_is_in_isr()) {
			current = true;
		} else {
			k_thread_suspend(contexts[i]);
		}
	}
	return current;
}

#if defined(CONFIG_EFR32_RADIO_ADMISSION)
/*
 * The LLL context runs at the radio jobs' priority, so its runs are admitted with them (Kconfig
 * EFR32_RADIO_WCET_LLL_NS, measured). The ULL worker is not admitted: its measured run
 * (EFR32_RADIO_WCET_ULL_NS) is wall time that includes the radio level preempting it, which
 * admission already counts as that level's own demand, so as a budget it would count that work
 * twice. Its start-by deadline (EFR32_DL_ULL_WINDOW_CYC) is checked when it starts.
 */
static int ll_threads_budgets_init(void)
{
	k_deadline_event_budget_set(&lll_ctx.event,
				    k_ns_to_cyc_ceil32(EFR32_DL_WCET_LLL_NS));
	return 0;
}

SYS_INIT(ll_threads_budgets_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
#endif
