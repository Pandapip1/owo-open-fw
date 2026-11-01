/*
 * The product's hardware watchdog: WDOG0, configured once at boot, never reconfigured, locked
 * (docs/watchdog.md). Raw register access through the CMSIS device header; Zephyr's
 * watchdog driver is not used (it cannot select the warning, the energy-mode bits or the lock).
 *
 *  - Period: CONFIG_OWO_WATCHDOG_PERSEL, (2^(PERSEL + 3) + 1) cycles of the devicetree's
 *    WDOG0 clock (RM SS27.3.1). The clock is the LFRCO on the suit and the LFXO on the kit,
 *    both 32768 Hz.
 *  - Warning at CONFIG_OWO_WATCHDOG_WARNSEL quarters of the period (RM SS27.3.4): a
 *    zero-latency interrupt that puts the output stage in its safe state with raw register
 *    writes (owo_suit_force_safe_from_isr) and then waits for the reset.
 *  - Runs in EM2 and EM3 (EM2RUN, EM3RUN; RM SS27.3.3), and while the core is halted by a
 *    debugger unless CONFIG_OWO_WATCHDOG_FREEZE_IN_DEBUG (DEBUGRUN; RM SS27.3.2).
 *  - WDOG0_LOCK and CMU_WDOGLOCK are written after the configuration: EN, CFG and the clock
 *    selection are then read-only for any write but the unlock key (RM SS27.5.8, SS8.5.4).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/clock_control_silabs.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <em_device.h>

#define WDOG_NODE DT_NODELABEL(wdog0)
#define WDOG_CLK_NODE DT_NODELABEL(wdog0clk)

static WDOG_TypeDef *const wdog = (WDOG_TypeDef *)DT_REG_ADDR(WDOG_NODE);
static const struct silabs_clock_control_cmu_config wdog_clock = SILABS_DT_CLOCK_CFG(WDOG_NODE);

/* RM SS27.3.1: TTIMEOUT = [2^(PERSEL + 3) + 1] / f; SS27.3.4: TWARNING = TTIMEOUT x WARNSEL / 4. */
#define WDOG_PERSEL_BIAS 3
#define WDOG_WARN_QUARTERS 4
#define WDOG_PERIOD_CYCLES ((1ULL << (CONFIG_OWO_WATCHDOG_PERSEL + WDOG_PERSEL_BIAS)) + 1)
/* The WDOG0 clock branch is a fixed-factor clock of its source oscillator (xg22.dtsi). */
#define WDOG_HZ DT_PROP(DT_CLOCKS_CTLR(WDOG_CLK_NODE), clock_frequency)
#define USEC_PER_SEC_ULL 1000000ULL

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(WDOG_NODE), "the product needs wdog0");
BUILD_ASSERT(DT_IRQN(WDOG_NODE) == WDOG0_IRQn, "devicetree and CMSIS disagree on WDOG0's IRQ");

extern void owo_suit_force_safe_from_isr(void);

/* Microseconds from a clear to the warning (rounded down) and to the reset (rounded up). */
uint32_t owo_wdt_warning_us(void)
{
	return (uint32_t)(WDOG_PERIOD_CYCLES * CONFIG_OWO_WATCHDOG_WARNSEL * USEC_PER_SEC_ULL /
			  (WDOG_WARN_QUARTERS * WDOG_HZ));
}

uint32_t owo_wdt_period_us(void)
{
	return (uint32_t)DIV_ROUND_UP(WDOG_PERIOD_CYCLES * USEC_PER_SEC_ULL, WDOG_HZ);
}

/*
 * Restart the count. CMD is an LFSYNC register (RM SS27.4): a write stays pending for a few
 * watchdog-clock cycles, and a second write while SYNCBUSY.CMD is set is a BusFault
 * (RM SS4.2.4.4.4). A pending clear restarts the count when it completes, so skipping this
 * one loses nothing. Returns 1 when skipped for that reason, 0 when written.
 */
int owo_wdt_feed(void)
{
	if (wdog->SYNCBUSY & WDOG_SYNCBUSY_CMD) {
		return 1;
	}
	wdog->CMD = WDOG_CMD_CLEAR;
	return 0;
}

/*
 * The warning: a feed is overdue by (WARNSEL/4 x period) - (longest legitimate feed interval).
 * Zero-latency, direct: it runs even inside irq_lock() sections (BASEPRI), never enters the
 * kernel, and never returns. The output stage goes to its safe state first; the reset that
 * follows within the rest of the period then re-runs the C reset safe-off and POST.
 */
ISR_DIRECT_DECLARE(owo_wdt_warning_isr)
{
	owo_suit_force_safe_from_isr();
	for (;;) {
		/* Wait for the watchdog reset. Nothing below this priority may run again. */
	}
	return 0;
}

static int owo_wdt_init(void)
{
	const struct device *cmu = DEVICE_DT_GET(DT_CLOCKS_CTLR(WDOG_NODE));
	CMU_TypeDef *const cmu_regs = (CMU_TypeDef *)DT_REG_ADDR(DT_CLOCKS_CTLR(WDOG_NODE));
	uint32_t rate = 0;
	int err;

	err = clock_control_on(cmu, (clock_control_subsys_t)&wdog_clock);
	if (err < 0 && err != -EALREADY) {
		return err;
	}
	if (clock_control_get_rate(cmu, (clock_control_subsys_t)&wdog_clock, &rate) < 0 ||
	    rate != WDOG_HZ) {
		/* The period would not be what the measurements chose: do not run unsupervised. */
		k_panic();
	}
	/* After any reset WDOG0 is disabled and unlocked (EMU_RSTCTRL: "the entire device is reset"). */
	wdog->CFG = (CONFIG_OWO_WATCHDOG_PERSEL << _WDOG_CFG_PERSEL_SHIFT) |
		    (CONFIG_OWO_WATCHDOG_WARNSEL << _WDOG_CFG_WARNSEL_SHIFT) | WDOG_CFG_EM2RUN |
		    WDOG_CFG_EM3RUN |
		    (IS_ENABLED(CONFIG_OWO_WATCHDOG_FREEZE_IN_DEBUG) ? 0 : WDOG_CFG_DEBUGRUN);
	wdog->IF_CLR = _WDOG_IF_MASK;
	wdog->IEN = WDOG_IEN_WARN;
	IRQ_DIRECT_CONNECT(DT_IRQN(WDOG_NODE), 0, owo_wdt_warning_isr, IRQ_ZERO_LATENCY);
	irq_enable(DT_IRQN(WDOG_NODE));
	wdog->EN = WDOG_EN_EN;
	wdog->LOCK = WDOG_LOCK_LOCKKEY_LOCK;
	/* Any key but the unlock key locks the WDOG clock selection (RM SS8.5.4). */
	cmu_regs->WDOGLOCK = ~CMU_WDOGLOCK_LOCKKEY_UNLOCK & _CMU_WDOGLOCK_LOCKKEY_MASK;
	return 0;
}

/* After PRE_KERNEL_1, where the clock control driver selects the WDOG0 clock branch. */
SYS_INIT(owo_wdt_init, PRE_KERNEL_2, 0);
