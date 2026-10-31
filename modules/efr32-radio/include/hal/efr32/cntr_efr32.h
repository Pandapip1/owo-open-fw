/* Tick-counter compare flag shared between hal/cntr.c and the derived lll.c. SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>
#include <stdint.h>

/* True once per ticker compare expiry (set by the scheduler clock-event handler). */
bool cntr_cmp_take(void);

void cntr_radio_cmp_set(uint8_t trx, uint32_t value, uint32_t generation);
void cntr_radio_cmp_cancel(void);
/* QEMU-only TIFS wake @p delay_us from now; its handler never issues a RAC command. */
/* No-earlier-than release: waited out exactly when inside the clock event's arm lead. */
void cntr_qemu_tifs_arm_after_us(uint32_t delay_us, uint32_t generation);
/* Timeout on the same clock event: may fire late, never spins the caller. */
void cntr_rx_timeout_arm_after_us(uint32_t delay_us, uint32_t generation);
void cntr_qemu_tifs_cmp_cancel(void);
/* Cancel every scheduler clock event of the controller (ISR-safe); none fires again. */
void cntr_shutdown(void);
void efr32_radio_launch_wake(uint8_t trx, uint32_t generation, uint32_t deadline);
void efr32_radio_qemu_tifs_wake(uint32_t generation, uint32_t deadline);
/* Deadline events the launch and TIFS wakes publish (NULL without the QEMU TIFS seam). */
struct k_deadline_event *efr32_radio_launch_event(uint8_t trx);
struct k_deadline_event *efr32_radio_tifs_event(void);
