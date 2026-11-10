/*
 * The watchdog feed job (docs/zephyr-dual-clock-scheduler.md, "The product's jobs"): a periodic
 * job of its own, non-pausable. Every period it feeds WDOG0 if the feed condition holds
 * (src/wdt.rs, which derives its declaration).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <zephyr/kernel.h>

/*
 * Cooperative, the highest priority under the radio's meta-IRQ levels: above the Bluetooth
 * receive threads (cooperative -10 and -8) and the output supervisor, so that no Bluetooth or
 * supervision work is ahead of the containment once it is released (default, pending user
 * confirmation). It can still wait for a cooperative thread already running at its release.
 */
#define FEED_PRIORITY K_PRIO_COOP(CONFIG_NUM_METAIRQ_PRIORITIES + 1)
/*
 * docs/radio-tables.md 7.86's rule: the high-water mark, 376 B (suit board model, stimulating,
 * a withheld feed included; no FP), plus the 36 B basic interrupt frame, rounded to 8.
 */
#define FEED_STACK_SIZE 416

static K_DEADLINE_EVENT_DEFINE(owo_feed_event);
static struct k_deadline_reservation feed_reservation;
static K_SEM_DEFINE(feed_declared, 0, 1);
/* Longest release-to-start seen, cycles, for reading over a debugger. */
__used uint32_t owo_feed_start_max_cyc;

/* src/wdt.rs: one feed pass; false when the feed was withheld. */
extern bool owo_feed_pass(void);

int owo_feed_job_start(uint32_t period_us, uint32_t deadline_us, uint32_t budget_us)
{
	feed_reservation = (struct k_deadline_reservation){
		.budget = k_us_to_cyc_ceil32(budget_us),
		.min_interarrival = k_us_to_cyc_floor32(period_us),
		.deadline = k_us_to_cyc_floor32(deadline_us),
		.non_pausable = true,
	};
	k_sem_give(&feed_declared);
	return 0;
}

int owo_feed_job_change(uint32_t period_us, uint32_t deadline_us, uint32_t budget_us)
{
	struct k_deadline_reservation next = {
		.budget = k_us_to_cyc_ceil32(budget_us),
		.min_interarrival = k_us_to_cyc_floor32(period_us),
		.deadline = k_us_to_cyc_floor32(deadline_us),
		.non_pausable = true,
	};

	return k_deadline_event_change(&owo_feed_event, &next);
}

static void feed_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	k_sem_take(&feed_declared, K_FOREVER);
	/* A refusal is reported (the miss policy); without the job WDOG0 is the containment. */
	if (k_deadline_event_arm_periodic(&owo_feed_event, &feed_reservation,
					  k_cycle_get_32() + feed_reservation.min_interarrival) != 0) {
		return;
	}
	for (;;) {
		k_deadline_event_wait(&owo_feed_event);
		owo_feed_start_max_cyc = MAX(owo_feed_start_max_cyc,
					     k_cycle_get_32() - owo_feed_event.release);
		(void)owo_feed_pass();
	}
}

K_THREAD_DEFINE(owo_feed_thread, FEED_STACK_SIZE, feed_job, NULL, NULL, NULL, FEED_PRIORITY, 0,
		0);
