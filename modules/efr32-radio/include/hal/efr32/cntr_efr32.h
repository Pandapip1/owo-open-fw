/* Tick-counter compare flag shared between hal/cntr.c and the derived lll.c. SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>
#include <stdint.h>

/* True once per ticker compare expiry (set by the scheduler clock-event handler). */
bool cntr_cmp_take(void);

void cntr_radio_cmp_set(uint8_t trx, uint32_t value, uint32_t generation);
void cntr_radio_cmp_cancel(void);
/*
 * QEMU-only TIFS job release, not before @p not_before and armed the wake latency earlier with
 * @p lead; the job must start by @p deadline. The handler never issues a RAC command.
 */
void cntr_qemu_tifs_arm_at(uint32_t not_before, int deadline, uint32_t generation, bool lead);
/* Timeout on the same clock event: may fire late, never spins the caller. */
void cntr_rx_timeout_arm_after_us(uint32_t delay_us, uint32_t generation);
void cntr_qemu_tifs_cmp_cancel(void);
/* Cancel every scheduler clock event of the controller (ISR-safe); none fires again. */
void cntr_shutdown(void);
/* A TX launch holds TXEN to @p instant; both runs wait out @p not_before, start by @p deadline. */
void efr32_radio_launch_wake(uint8_t trx, uint32_t generation, uint32_t instant,
			     uint32_t not_before, int deadline);
void efr32_radio_qemu_tifs_wake(uint32_t generation, uint32_t not_before, int deadline);
/* Deadline events the launch and TIFS wakes publish (NULL without the QEMU TIFS seam). */
struct k_deadline_event *efr32_radio_launch_event(uint8_t trx);
struct k_deadline_event *efr32_radio_tifs_event(void);
