/*
 * Amateur-radio operation of the EFR32 radio (CONFIG_EFR32_RADIO_AMATEUR): the switch between
 * Part 15 and amateur rules, the channel plan, the encryption refusal, and the station
 * identification job. Rules: hal/amateur_rules.c; radio side: hal/radio.c; documentation and
 * rule text: docs/amateur-radio.md.
 *
 * Identification (47 CFR 97.119(a), (b)(1)): the call sign in Morse code, on-off keying the
 * carrier on the station's last transmitting channel, at 20 WPM or slower. It is a job of the
 * deadline scheduler (k_deadline_event): released early (the lead), placed by the controller's
 * ticker into a reserved radio slot (a user-extension ticker node, as Zephyr's own flash driver
 * borrows the radio), and watched by a timer at its latest start. If it has not started then,
 * the obligation cannot be met and the radio stops transmitting: the safe shutdown.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/kernel/deadline_miss.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <soc.h>

#include "util/memq.h"
#include "util/mayfly.h"
#include "hal/ticker.h"
#include "ticker/ticker.h"
#include "ll_sw/lll.h"
#include "lll/lll_vendor.h"
#include "ll.h"
#include "hal/efr32/amateur.h"
#include "hal/efr32/ll_threads.h"
#include "amateur_radio.h"

/* ull_conn_internal.h needs most of the controller's headers; only this is used. */
struct ll_conn;
struct ll_conn *ll_connected_get(uint16_t handle);

/*
 * The end of a communication: no transmission for the longest time a Bluetooth exchange can be
 * silent and continue, the largest connection supervision timeout, 32 s (Core v6.1 Vol 6 Part B
 * 4.5.2; legacy advertising repeats at most every 10.24 s, 4.4.2.2.2).
 */
#define EFR32_AMATEUR_END_IDLE_MS 32000U
/* The sync word ahead of the frame: 1M table MODEM.CTRL1.SYNCBITS = 31, 32 symbols keyed off. */
#define EFR32_AMATEUR_CW_SYNC_SYMBOLS 32U
#define EFR32_AMATEUR_BITS_PER_OCTET 8U
/* Morse units of the longest call sign: 12 characters of at most 5 dashes (0: 19 units + 3). */
#define EFR32_AMATEUR_CW_UNITS_MAX (EFR32_AMATEUR_CALLSIGN_MAX * 22U)
#define EFR32_AMATEUR_CW_OCTETS_MAX 512U

/*
 * Test only (EFR32_RADIO_AMATEUR_SYNTH_PROBE): receive-only carriers for measuring where the
 * synthesizer calibrates and locks, 2290-2500 MHz: the 13 cm band with 10 MHz below and the
 * 2.4 GHz ISM band with 16.5 MHz above.
 */
#define EFR32_AMATEUR_PROBE_MIN_KHZ 2290000U
#define EFR32_AMATEUR_PROBE_MAX_KHZ 2500000U

/*
 * The job's start-by deadline when no rule asks for an earlier one (the bookkeeping after an
 * identification; an identification requested at the end of a communication): the lead.
 */
#define EFR32_AMATEUR_LEAD_MS (CONFIG_EFR32_RADIO_AMATEUR_ID_LEAD_S * MSEC_PER_SEC)

#define EFR32_AMATEUR_ID_THREAD_PRIO (K_HIGHEST_THREAD_PRIO + CONFIG_NUM_METAIRQ_PRIORITIES)

/* Two user-extension ticker nodes: the radio-slot request and the delayed start in it. */
#define EFR32_AMATEUR_TICKER_SLOT (TICKER_ID_USER_BASE)
#define EFR32_AMATEUR_TICKER_START (TICKER_ID_USER_BASE + 1U)
BUILD_ASSERT(CONFIG_BT_CTLR_USER_TICKER_ID_RANGE >= 2, "amateur identification needs two ticker nodes");

BUILD_ASSERT(CONFIG_EFR32_RADIO_AMATEUR_CW_WPM <= 20,
	     "47 CFR 97.119(b)(1): an automatic CW identification must not exceed 20 WPM");

static struct k_spinlock efr32_amateur_lock;
static atomic_t efr32_amateur_on;
static const struct efr32_amateur_admin *efr32_amateur_admin;
static char efr32_amateur_call[EFR32_AMATEUR_CALLSIGN_MAX + 1U];
/* Carrier of each RF channel (2402 + 2 x rf MHz under Part 15), kHz. */
static uint32_t efr32_amateur_plan_rf[EFR32_AMATEUR_CHANNELS];
static int efr32_amateur_error_channel = -1;
static uint32_t efr32_amateur_tuned_khz;
static uint32_t efr32_amateur_tx_khz;

static struct efr32_amateur_id_state efr32_amateur_id;
static uint8_t efr32_amateur_cw[EFR32_AMATEUR_CW_OCTETS_MAX];
static size_t efr32_amateur_cw_octets;
static uint32_t efr32_amateur_cw_airtime_us;
static uint32_t efr32_amateur_ids;
/* The identification has been handed to the radio; it completed (for the job). */
static bool efr32_amateur_id_started;
static bool efr32_amateur_id_completed;
static bool efr32_amateur_slot_requested;
static bool efr32_amateur_id_now;
/* efr32_amateur_disable() waits for the last identification. */
static bool efr32_amateur_closing;
static atomic_t efr32_amateur_enc_refusals;

K_DEADLINE_EVENT_DEFINE(efr32_amateur_id_event);
static K_SEM_DEFINE(efr32_amateur_id_sem, 0, 1);
static void efr32_amateur_release_expiry(struct k_timer *timer);
static void efr32_amateur_watchdog_expiry(struct k_timer *timer);
static K_TIMER_DEFINE(efr32_amateur_release_timer, efr32_amateur_release_expiry, NULL);
static K_TIMER_DEFINE(efr32_amateur_watchdog_timer, efr32_amateur_watchdog_expiry, NULL);

static const char efr32_amateur_id_rule[] =
	"amateur station identification, 47 CFR 97.119(a)";
static const char efr32_amateur_enc_rule[] =
	"encryption under amateur rules, 47 CFR 97.113(a)(4)";

/* RF channel k (Core v6.1 Vol 6 Part B 1.4.1) to the physical channel index the plan uses. */
#define EFR32_RF_ADV37 0U
#define EFR32_RF_ADV38 12U
#define EFR32_RF_ADV39 39U
static uint8_t efr32_amateur_rf_to_index(uint32_t rf)
{
	if (rf == EFR32_RF_ADV37) {
		return EFR32_AMATEUR_ADV_CHANNEL_FIRST;
	}
	if (rf == EFR32_RF_ADV38) {
		return EFR32_AMATEUR_ADV_CHANNEL_FIRST + 1U;
	}
	if (rf == EFR32_RF_ADV39) {
		return EFR32_AMATEUR_ADV_CHANNEL_FIRST + 2U;
	}
	return (uint8_t)(rf < EFR32_RF_ADV38 ? rf - 1U : rf - 2U);
}

static uint64_t efr32_amateur_now_ms(void)
{
	return (uint64_t)k_uptime_get();
}

/* Arm the release and the watchdog for the next identification (lock held). */
static void efr32_amateur_schedule(uint64_t now)
{
	struct efr32_amateur_id_due due;
	uint32_t airtime_ms = DIV_ROUND_UP(efr32_amateur_cw_airtime_us, USEC_PER_MSEC);

	if (efr32_amateur_slot_requested ||
	    !efr32_amateur_id_next(&efr32_amateur_id, efr32_amateur_admin,
				   EFR32_AMATEUR_END_IDLE_MS,
				   CONFIG_EFR32_RADIO_AMATEUR_ID_LEAD_S * MSEC_PER_SEC, airtime_ms,
				   &due)) {
		k_timer_stop(&efr32_amateur_release_timer);
		if (!efr32_amateur_slot_requested) {
			k_timer_stop(&efr32_amateur_watchdog_timer);
		}
		return;
	}
	k_timer_start(&efr32_amateur_release_timer,
		      K_MSEC(due.release_ms > now ? due.release_ms - now : 0U), K_NO_WAIT);
	k_timer_start(&efr32_amateur_watchdog_timer,
		      K_MSEC(due.latest_start_ms > now ? due.latest_start_ms - now : 0U),
		      K_NO_WAIT);
}

/* Post the job with a start-by deadline no further ahead than the lead (32-bit cycles). */
static void efr32_amateur_post(uint32_t start_within_ms)
{
	uint32_t ms = MIN(start_within_ms, CONFIG_EFR32_RADIO_AMATEUR_ID_LEAD_S * MSEC_PER_SEC);

	k_deadline_event_post(&efr32_amateur_id_event,
			      (int)(k_cycle_get_32() + k_ms_to_cyc_ceil32(ms)));
}

static void efr32_amateur_release_expiry(struct k_timer *timer)
{
	struct efr32_amateur_id_due due;
	k_spinlock_key_t key = k_spin_lock(&efr32_amateur_lock);
	uint64_t now = efr32_amateur_now_ms();
	uint32_t airtime_ms = DIV_ROUND_UP(efr32_amateur_cw_airtime_us, USEC_PER_MSEC);
	bool due_now = false;

	ARG_UNUSED(timer);
	if (atomic_get(&efr32_amateur_on) != 0 && !efr32_amateur_slot_requested &&
	    efr32_amateur_id_next(&efr32_amateur_id, efr32_amateur_admin,
				  EFR32_AMATEUR_END_IDLE_MS,
				  CONFIG_EFR32_RADIO_AMATEUR_ID_LEAD_S * MSEC_PER_SEC, airtime_ms,
				  &due)) {
		if (due.release_ms > now) {
			/* Transmissions since moved the end of the communication: later. */
			efr32_amateur_schedule(now);
		} else {
			due_now = true;
		}
	}
	k_spin_unlock(&efr32_amateur_lock, key);
	if (due_now) {
		efr32_amateur_post(due.latest_start_ms > now ? due.latest_start_ms - now : 0U);
	}
}

static void efr32_amateur_watchdog_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	if (atomic_get(&efr32_amateur_on) != 0 && !efr32_amateur_id_started) {
		/* Not on air by its latest start: the station may not transmit unidentified. */
		efr32_radio_policy_stop(&efr32_amateur_id_event, efr32_amateur_id_rule);
	}
}

/* Radio side ------------------------------------------------------------------------------ */

uint32_t efr32_radio_amateur_carrier_khz(uint32_t rf_channel, uint32_t standard_khz)
{
	uint32_t khz = standard_khz;

	if (atomic_get(&efr32_amateur_on) != 0) {
		khz = efr32_amateur_plan_rf[rf_channel];
		/* The plan was checked when it was set and is fixed while amateur rules apply. */
		if (!IS_ENABLED(CONFIG_EFR32_RADIO_AMATEUR_SYNTH_PROBE) &&
		    !efr32_amateur_in_band(efr32_amateur_admin, khz,
					   efr32_radio_amateur_half_bw_khz)) {
			efr32_radio_policy_stop(NULL, "carrier outside the amateur band");
		}
	}
	efr32_amateur_tuned_khz = khz;
	return khz;
}

void efr32_radio_amateur_tx_notify(void)
{
	k_spinlock_key_t key;

	if (atomic_get(&efr32_amateur_on) == 0) {
		return;
	}
	if (IS_ENABLED(CONFIG_EFR32_RADIO_AMATEUR_SYNTH_PROBE)) {
		/* The probe tunes anywhere in its range: it may only receive. */
		efr32_radio_policy_stop(NULL, "transmission in the receive-only synthesizer probe");
		return;
	}
	key = k_spin_lock(&efr32_amateur_lock);
	efr32_amateur_tx_khz = efr32_amateur_tuned_khz;
	if (efr32_amateur_id_tx(&efr32_amateur_id, efr32_amateur_now_ms(),
				EFR32_AMATEUR_END_IDLE_MS)) {
		efr32_amateur_schedule(efr32_amateur_now_ms());
	}
	k_spin_unlock(&efr32_amateur_lock, key);
}

static void efr32_amateur_enc_report(struct k_work *work)
{
	ARG_UNUSED(work);
	printk("efr32-radio: amateur rules (%s): encryption refused, 47 CFR 97.113(a)(4); "
	       "%u refusals\n", efr32_amateur_call, (unsigned int)atomic_get(&efr32_amateur_enc_refusals));
}

static K_WORK_DEFINE(efr32_amateur_enc_work, efr32_amateur_enc_report);

static bool efr32_amateur_enc_allowed(void)
{
	return atomic_get(&efr32_amateur_on) == 0 || !efr32_amateur_admin->encryption_prohibited;
}

/* BT_CTLR_LE_ENC_POLICY: the link layer refuses to start encryption while this is false. */
bool ll_enc_policy_permitted(void)
{
	if (efr32_amateur_enc_allowed()) {
		return true;
	}
	atomic_inc(&efr32_amateur_enc_refusals);
	(void)k_work_submit(&efr32_amateur_enc_work);
	return false;
}

bool efr32_radio_amateur_ccm_permitted(void)
{
	if (efr32_amateur_enc_allowed()) {
		return true;
	}
	/* The link layer never starts encryption under these rules: this is a defect. Stop. */
	efr32_radio_policy_stop(NULL, efr32_amateur_enc_rule);
	return false;
}

/* The identification in the radio slot --------------------------------------------------- */

static void efr32_amateur_cw_done(void *param)
{
	ARG_UNUSED(param);
	efr32_radio_cw_finish();
	k_spinlock_key_t key = k_spin_lock(&efr32_amateur_lock);

	efr32_amateur_id_completed = true;
	if (efr32_amateur_closing) {
		/*
		 * The last identification of the communication: Part 15 from here, in the
		 * reserved slot, so nothing is transmitted under amateur rules after it.
		 */
		atomic_set(&efr32_amateur_on, 0);
	}
	k_spin_unlock(&efr32_amateur_lock, key);
	efr32_amateur_post(EFR32_AMATEUR_LEAD_MS);
}

/* LLL context, the radio idle and the slot reserved. */
static void efr32_amateur_cw_start(void *param)
{
	uint32_t khz;

	ARG_UNUSED(param);
	k_spinlock_key_t key = k_spin_lock(&efr32_amateur_lock);

	efr32_amateur_id_started = true;
	khz = efr32_amateur_tx_khz;
	k_spin_unlock(&efr32_amateur_lock, key);
	k_timer_stop(&efr32_amateur_watchdog_timer);
	efr32_radio_cw_transmit(khz, efr32_amateur_cw, efr32_amateur_cw_octets,
				efr32_amateur_cw_done, NULL);
}

static void efr32_amateur_ticker_op(uint32_t status, void *param)
{
	ARG_UNUSED(status);
	ARG_UNUSED(param);
}

/* ULL_HIGH: the slot's start, after the radio's events were aborted. */
static void efr32_amateur_slot_start(uint32_t ticks_at_expire, uint32_t ticks_drift,
				     uint32_t remainder, uint16_t lazy, uint8_t force,
				     void *context)
{
	static memq_link_t link;
	static struct mayfly mfy = { 0, 0, &link, NULL, efr32_amateur_cw_start };

	ARG_UNUSED(ticks_at_expire);
	ARG_UNUSED(ticks_drift);
	ARG_UNUSED(remainder);
	ARG_UNUSED(lazy);
	ARG_UNUSED(force);
	ARG_UNUSED(context);
	if (!ll_radio_state_is_idle()) {
		/* Not yet: the periodic slot request tries again one slot later. */
		return;
	}
	(void)ticker_stop(TICKER_INSTANCE_ID_CTLR, TICKER_USER_ID_ULL_HIGH,
			  EFR32_AMATEUR_TICKER_SLOT, efr32_amateur_ticker_op, NULL);
	(void)mayfly_enqueue(TICKER_USER_ID_ULL_HIGH, TICKER_USER_ID_LLL, 0U, &mfy);
}

/* ULL_HIGH: the ticker granted the reserved slot. Abort what holds the radio, then start. */
static void efr32_amateur_slot_granted(uint32_t ticks_at_expire, uint32_t ticks_drift,
				       uint32_t remainder, uint16_t lazy, uint8_t force,
				       void *context)
{
	ARG_UNUSED(ticks_drift);
	ARG_UNUSED(remainder);
	ARG_UNUSED(lazy);
	ARG_UNUSED(force);
	ARG_UNUSED(context);
	if (efr32_amateur_id_started) {
		return;
	}
	ll_radio_state_abort();
	(void)ticker_start(TICKER_INSTANCE_ID_CTLR, TICKER_USER_ID_ULL_HIGH,
			   EFR32_AMATEUR_TICKER_START, ticks_at_expire,
			   HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_START_US), 0U, 0U, 0U, 0U,
			   efr32_amateur_slot_start, NULL, efr32_amateur_ticker_op, NULL);
}

/* Thread: ask the ticker for a slot as long as the identification, starting now. */
static int efr32_amateur_slot_request(void)
{
	uint32_t slot_us = efr32_amateur_cw_airtime_us + EVENT_OVERHEAD_START_US +
			   EVENT_OVERHEAD_XTAL_US + EVENT_OVERHEAD_END_US;
	uint32_t ticks_slot = HAL_TICKER_US_TO_TICKS(slot_us);
	uint32_t ret;

	ret = ticker_start(TICKER_INSTANCE_ID_CTLR, TICKER_USER_ID_THREAD,
			   EFR32_AMATEUR_TICKER_SLOT, ticker_ticks_now_get(), 0U, ticks_slot,
			   HAL_TICKER_REMAINDER(slot_us), 0U, ticks_slot,
			   efr32_amateur_slot_granted, NULL, efr32_amateur_ticker_op, NULL);
	return (ret == TICKER_STATUS_SUCCESS || ret == TICKER_STATUS_BUSY) ? 0 : -EIO;
}

static void efr32_amateur_id_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		bool request = false;
		k_spinlock_key_t key;
		uint64_t now;

		k_deadline_event_wait(&efr32_amateur_id_event);
		key = k_spin_lock(&efr32_amateur_lock);
		now = efr32_amateur_now_ms();
		if (efr32_amateur_id_completed) {
			efr32_amateur_id_completed = false;
			efr32_amateur_id_started = false;
			efr32_amateur_slot_requested = false;
			efr32_amateur_id_done(&efr32_amateur_id, now);
			efr32_amateur_ids++;
			efr32_amateur_schedule(now);
			k_sem_give(&efr32_amateur_id_sem);
		} else if (atomic_get(&efr32_amateur_on) != 0 && !efr32_amateur_slot_requested &&
			   (efr32_amateur_id.unidentified || efr32_amateur_id_now)) {
			efr32_amateur_slot_requested = true;
			efr32_amateur_id_now = false;
			k_timer_stop(&efr32_amateur_release_timer);
			request = true;
		} else {
			k_sem_give(&efr32_amateur_id_sem);
		}
		k_spin_unlock(&efr32_amateur_lock, key);
		if (request && efr32_amateur_slot_request() != 0) {
			efr32_radio_policy_stop(&efr32_amateur_id_event, efr32_amateur_id_rule);
		}
	}
}

K_THREAD_DEFINE(efr32_amateur_id_thread, CONFIG_EFR32_RADIO_AMATEUR_ID_STACK_SIZE,
		efr32_amateur_id_job, NULL, NULL, NULL, EFR32_AMATEUR_ID_THREAD_PRIO, 0, 0);

#if defined(CONFIG_SCHED_DEADLINE_ADMISSION)
static int efr32_amateur_budget_init(void)
{
	k_deadline_event_budget_set(&efr32_amateur_id_event,
				    k_ns_to_cyc_ceil32(CONFIG_EFR32_RADIO_WCET_AMATEUR_ID_NS));
	return 0;
}

SYS_INIT(efr32_amateur_budget_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
#endif

/*
 * hci/hci_user_ext.h: the controller's user extension reserves one proprietary receive node type
 * (BT_CTLR_USER_EVT_RANGE, Kconfig.defaults) that nothing here generates.
 */
struct node_rx_pdu;
struct pdu_data;
struct net_buf;
#define EFR32_AMATEUR_HCI_CLASS_NONE 0 /* hci_internal.h HCI_CLASS_NONE */

int8_t hci_user_ext_get_class(struct node_rx_pdu *node_rx)
{
	ARG_UNUSED(node_rx);
	return EFR32_AMATEUR_HCI_CLASS_NONE;
}

void hci_user_ext_encode_control(struct node_rx_pdu *node_rx, struct pdu_data *pdu_data,
				 struct net_buf *buf)
{
	ARG_UNUSED(node_rx);
	ARG_UNUSED(pdu_data);
	ARG_UNUSED(buf);
}

/* API --------------------------------------------------------------------------------------- */

static bool efr32_amateur_connected(void)
{
#if defined(CONFIG_BT_CONN)
	for (uint16_t handle = 0U; handle < CONFIG_BT_MAX_CONN; handle++) {
		if (ll_connected_get(handle) != NULL) {
			return true;
		}
	}
#endif
	return false;
}

/* The Morse frame of @p call and its airtime. */
static int efr32_amateur_cw_build(const char *call)
{
	uint8_t units[EFR32_AMATEUR_CW_UNITS_MAX];
	uint32_t unit_us = efr32_amateur_cw_unit_us(CONFIG_EFR32_RADIO_AMATEUR_CW_WPM);
	uint32_t per_unit = efr32_radio_cw_symbols_per_unit(unit_us);
	uint32_t rate = efr32_radio_cw_symbol_rate_hz();
	size_t n = efr32_amateur_morse_units(call, units, ARRAY_SIZE(units));
	size_t octets;

	/* A unit must be a whole number of symbols, or the keying would drift. */
	if (n == 0U || per_unit == 0U ||
	    (uint64_t)per_unit * USEC_PER_SEC != (uint64_t)rate * unit_us) {
		return -EINVAL;
	}
	octets = efr32_amateur_cw_bits(units, n, per_unit, efr32_amateur_cw,
				       MIN(sizeof(efr32_amateur_cw), efr32_radio_cw_max_octets()));
	if (octets == 0U) {
		return -EINVAL;
	}
	efr32_amateur_cw_octets = octets;
	efr32_amateur_cw_airtime_us = (uint32_t)(((uint64_t)EFR32_AMATEUR_CW_SYNC_SYMBOLS +
						  octets * EFR32_AMATEUR_BITS_PER_OCTET) *
						 USEC_PER_SEC / rate);
	return 0;
}

int efr32_amateur_enable(const struct efr32_amateur_config *config)
{
	const struct efr32_amateur_admin *admin;
	uint32_t plan[EFR32_AMATEUR_CHANNELS];
	char call[EFR32_AMATEUR_CALLSIGN_MAX + 1U];
	k_spinlock_key_t key;
	int bad;
	int err;

	efr32_amateur_error_channel = -1;
	if (config == NULL) {
		return -EINVAL;
	}
	if (atomic_get(&efr32_amateur_on) != 0) {
		return -EALREADY;
	}
	admin = config->admin != NULL ? config->admin : &efr32_amateur_us_part97;
	if (!efr32_amateur_callsign_normalize(admin, config->callsign, call)) {
		return -EINVAL;
	}
	/* The radio's own maximum is under the administration's ceiling (97.313(b)). */
	if (efr32_radio_amateur_hw.max_power_mdbm > admin->max_power_mdbm ||
	    CONFIG_EFR32_RADIO_AMATEUR_CW_WPM > admin->cw_max_wpm) {
		return -EINVAL;
	}
	if (config->plan_khz != NULL) {
		memcpy(plan, config->plan_khz, sizeof(plan));
	} else if (!efr32_amateur_plan_default(admin, &efr32_radio_amateur_hw,
					       efr32_radio_amateur_half_bw_khz, plan)) {
		return -ERANGE;
	}
#if defined(CONFIG_EFR32_RADIO_AMATEUR_SYNTH_PROBE)
	/* Receive only (efr32_radio_amateur_tx_notify()): any carrier in the probe range. */
	bad = -1;
	for (uint32_t ch = 0U; ch < EFR32_AMATEUR_CHANNELS && bad < 0; ch++) {
		if (plan[ch] < EFR32_AMATEUR_PROBE_MIN_KHZ || plan[ch] > EFR32_AMATEUR_PROBE_MAX_KHZ) {
			bad = (int)ch;
		}
	}
#else
	bad = efr32_amateur_plan_check(admin, &efr32_radio_amateur_hw,
				       efr32_radio_amateur_half_bw_khz, plan);
#endif
	if (bad >= 0) {
		efr32_amateur_error_channel = bad;
		return -ERANGE;
	}
	if (efr32_amateur_connected()) {
		return -EBUSY;
	}
	err = efr32_amateur_cw_build(call);
	if (err != 0) {
		return err;
	}

	key = k_spin_lock(&efr32_amateur_lock);
	efr32_amateur_admin = admin;
	strcpy(efr32_amateur_call, call);
	for (uint32_t rf = 0U; rf < EFR32_AMATEUR_CHANNELS; rf++) {
		efr32_amateur_plan_rf[rf] = plan[efr32_amateur_rf_to_index(rf)];
	}
	efr32_amateur_id_init(&efr32_amateur_id, efr32_amateur_now_ms());
	efr32_amateur_id_started = false;
	efr32_amateur_id_completed = false;
	efr32_amateur_slot_requested = false;
	efr32_amateur_id_now = false;
	efr32_amateur_tx_khz = plan[EFR32_AMATEUR_ADV_CHANNEL_FIRST];
	atomic_set(&efr32_amateur_on, 1);
	k_spin_unlock(&efr32_amateur_lock, key);
	printk("efr32-radio: amateur rules, %s, station %s; CW identification %u WPM, %u ms\n",
	       admin->name, efr32_amateur_call, CONFIG_EFR32_RADIO_AMATEUR_CW_WPM,
	       (unsigned int)DIV_ROUND_UP(efr32_amateur_cw_airtime_us, USEC_PER_MSEC));
	return 0;
}

int efr32_amateur_end_of_communication(void)
{
	k_spinlock_key_t key;
	bool post;

	if (atomic_get(&efr32_amateur_on) == 0) {
		return -EALREADY;
	}
	key = k_spin_lock(&efr32_amateur_lock);
	post = efr32_amateur_id.unidentified && !efr32_amateur_slot_requested;
	k_spin_unlock(&efr32_amateur_lock, key);
	if (post) {
		efr32_amateur_post(EFR32_AMATEUR_LEAD_MS);
	}
	return 0;
}

int efr32_amateur_disable(void)
{
	k_spinlock_key_t key;
	bool identify;
	uint32_t wait_ms;

	if (atomic_get(&efr32_amateur_on) == 0) {
		return -EALREADY;
	}
	if (efr32_amateur_connected()) {
		return -EBUSY;
	}
	key = k_spin_lock(&efr32_amateur_lock);
	identify = efr32_amateur_id.unidentified || efr32_amateur_slot_requested;
	efr32_amateur_id_now = identify;
	efr32_amateur_closing = identify;
	k_spin_unlock(&efr32_amateur_lock, key);
	if (identify) {
		/*
		 * The end of the communication (97.119(a)): identify, and leave amateur rules as
		 * that identification ends (efr32_amateur_cw_done()).
		 */
		k_sem_reset(&efr32_amateur_id_sem);
		efr32_amateur_post(EFR32_AMATEUR_LEAD_MS);
		wait_ms = CONFIG_EFR32_RADIO_AMATEUR_ID_LEAD_S * MSEC_PER_SEC +
			  DIV_ROUND_UP(efr32_amateur_cw_airtime_us, USEC_PER_MSEC);
		while (atomic_get(&efr32_amateur_on) != 0 &&
		       k_sem_take(&efr32_amateur_id_sem, K_MSEC(wait_ms)) == 0) {
		}
		if (atomic_get(&efr32_amateur_on) != 0) {
			efr32_radio_policy_stop(&efr32_amateur_id_event, efr32_amateur_id_rule);
			return -EIO;
		}
	}
	key = k_spin_lock(&efr32_amateur_lock);
	efr32_amateur_closing = false;
	atomic_set(&efr32_amateur_on, 0);
	k_timer_stop(&efr32_amateur_release_timer);
	k_timer_stop(&efr32_amateur_watchdog_timer);
	k_spin_unlock(&efr32_amateur_lock, key);
	printk("efr32-radio: Part 15 rules\n");
	return 0;
}

bool efr32_amateur_active(void)
{
	return atomic_get(&efr32_amateur_on) != 0;
}

const char *efr32_amateur_callsign(void)
{
	return efr32_amateur_active() ? efr32_amateur_call : NULL;
}

int efr32_amateur_last_error_channel(void)
{
	return efr32_amateur_error_channel;
}

uint32_t efr32_amateur_id_count(void)
{
	return efr32_amateur_ids;
}
