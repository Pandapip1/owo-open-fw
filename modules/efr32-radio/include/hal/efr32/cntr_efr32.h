/* Tick-counter compare flag shared between hal/cntr.c and the derived lll.c. SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>
#include <stdint.h>

/* True once per ticker compare expiry (set by the scheduler clock-event handler). */
bool cntr_cmp_take(void);

/* Scheduler-clock cycles (32-bit logical time) of the ticker tick @p tick nearest to now. */
uint32_t cntr_tick_cycles(uint32_t tick);
/*
 * Arm the launch of @p trx at scheduler-clock @p instant; an RX launch with @p tolerance_us is a
 * Peripheral's receive window, held to the instant and late by at most that (ll_deadlines.h).
 */
void cntr_radio_cmp_set(uint8_t trx, uint32_t instant, uint32_t tolerance_us,
			uint32_t generation);
void cntr_radio_cmp_cancel(void);
/*
 * QEMU-only TIFS job release, not before @p not_before and armed the wake latency earlier with
 * @p lead; the job must start by @p deadline. The handler never issues a RAC command.
 */
void cntr_qemu_tifs_arm_at(uint32_t not_before, int deadline, uint32_t generation, bool lead);
/* Timeout on the same clock event: may fire late, never spins the caller. */
void cntr_rx_timeout_arm_after_us(uint32_t delay_us, uint32_t generation);
/* The same timeout at scheduler-clock time @p at (or at once, if that has passed). */
void cntr_rx_timeout_arm_at(uint32_t at, uint32_t generation);
void cntr_qemu_tifs_cmp_cancel(void);
/* Cancel every scheduler clock event of the controller (ISR-safe); none fires again. */
void cntr_shutdown(void);
/*
 * A TX launch, and an RX launch with @p hold, posts its command to the sequencer, which issues it
 * at @p instant and not after @p limit (ll_deadlines.h). Every run starts by @p deadline.
 */
void efr32_radio_launch_wake(uint8_t trx, bool hold, uint32_t generation, uint32_t instant,
			     uint32_t limit, int deadline);
void efr32_radio_qemu_tifs_wake(uint32_t generation, uint32_t not_before, int deadline);
/* Deadline events the launch and TIFS wakes publish (NULL without the QEMU TIFS seam). */
struct k_deadline_event *efr32_radio_launch_event(uint8_t trx);
struct k_deadline_event *efr32_radio_tifs_event(void);
