/*
 * TEST ONLY (CONFIG_OWO_SIM_INJECT): drain the simulation mailbox every millisecond on the
 * system work queue. The mailbox and its dispatch are in src/sim_inject.rs.
 */
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#define OWO_SIM_POLL_MS 1

extern void owo_sim_poll(void);

static void owo_sim_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(owo_sim_work, owo_sim_work_fn);

static void owo_sim_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	owo_sim_poll();
	(void)k_work_schedule(&owo_sim_work, K_MSEC(OWO_SIM_POLL_MS));
}

static int owo_sim_init(void)
{
	(void)k_work_schedule(&owo_sim_work, K_MSEC(OWO_SIM_POLL_MS));
	return 0;
}

SYS_INIT(owo_sim_init, APPLICATION, 99);
