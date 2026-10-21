/* Tick-counter compare flag shared between hal/cntr.c and the derived lll.c. SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>
#include <stdint.h>

/* True once per ticker compare expiry (set by the scheduler clock-event handler). */
bool cntr_cmp_take(void);
/* Start-by deadline of the ticker work the LL thread is about to run. */
uint32_t cntr_ticker_deadline(void);
/* Start-by deadline of the radio launch work the LLL context is about to run. */
uint32_t cntr_launch_deadline(void);

void cntr_radio_cmp_set(uint8_t trx, uint32_t value, uint32_t generation);
void cntr_radio_cmp_cancel(void);
/* QEMU-only TIFS wake @p delay_us from now; its handler never issues a RAC command. */
void cntr_qemu_tifs_arm_after_us(uint32_t delay_us, uint32_t generation);
void cntr_qemu_tifs_cmp_cancel(void);
void efr32_radio_launch_wake(uint8_t trx, uint32_t generation, uint32_t deadline);
void efr32_radio_qemu_tifs_wake(uint32_t generation, uint32_t deadline);
