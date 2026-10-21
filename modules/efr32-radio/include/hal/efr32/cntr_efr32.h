/* Tick-counter compare flag shared between hal/cntr.c and the derived lll.c. SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>
#include <stdint.h>

/* True once per compare interrupt (set by the counter alarm callback). */
bool cntr_cmp_take(void);

void cntr_radio_cmp_set(uint8_t trx, uint32_t value, uint32_t generation);
void cntr_radio_cmp_cancel(void);
/* QEMU-only TIFS wake shares CC0 but never issues a RAC command in its ISR. */
void cntr_qemu_tifs_cmp_set(uint32_t value, uint32_t generation);
void cntr_qemu_tifs_cmp_cancel(void);
void efr32_radio_launch_wake(uint8_t trx, uint32_t generation);
void efr32_radio_qemu_tifs_wake(uint32_t generation);
