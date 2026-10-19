/* Tick-counter compare flag shared between hal/cntr.c and the derived lll.c. SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>

/* True once per compare interrupt (set by the counter alarm callback). */
bool cntr_cmp_take(void);
