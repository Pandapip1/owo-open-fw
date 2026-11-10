/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Shared by the PROTIMER probes: register addresses, the completion marker and the
 * entry stub that runs a probe from a debugger halt-point inside the host firmware.
 */
#ifndef PROTIMER_PROBE_H
#define PROTIMER_PROBE_H

#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

/*
 * Non-secure alias: the probes run in whatever CPU security state the host firmware left
 * them in (Non-secure, like the rest of the real driver), and the secure 0xa801c000 window
 * silently drops writes from there -- confirmed by running probe.S once against the secure
 * address: it completed with no fault but every readback stayed 0.
 */
#define PROTIMER_BASE 0xb801c000U
/* EN.EN, same one-bit latch pattern FRC/AGC/RFCRC/MODEM/SYNTH/RAC each have at +0x004
 * (docs/radio-tables.md SS7, public *_S_register.py:24-37). */
#define PROTIMER_EN (PROTIMER_BASE + 0x004U)
/* Value observed, not field-decoded -- see probe.c's header. */
#define PROTIMER_CTRL (PROTIMER_BASE + 0x008U)
#define PROTIMER_CTRL_VAL 0x00011102U
/* Stock pairs "1" here with enabling NVIC IRQ36, "4" with disabling it (docs SS7.9). */
#define PROTIMER_CMD (PROTIMER_BASE + 0x00cU)
#define PROTIMER_CMD_START 0x00000001U
/* Matches vendor/qemu's own modeled offset AND the offset observed changing live in stock's
 * capture (docs SS7.32). */
#define PROTIMER_COUNTER (PROTIMER_BASE + 0x020U)
/* "Timer state/period input", not further resolved (docs SS7.9). */
#define PROTIMER_REG_030 (PROTIMER_BASE + 0x030U)
#define PROTIMER_REG_030_VAL 0x00001233U
#define PROTIMER_REG_038 (PROTIMER_BASE + 0x038U)
#define PROTIMER_REG_038_VAL 0x80015558U
/* Unresolved bit meanings -- see probe-ien-sweep.c's header. */
#define PROTIMER_IEN (PROTIMER_BASE + 0x070U)
#define PROTIMER_IF_CLR (PROTIMER_BASE + 0x2064U)
/* All-ones: a blanket sweep, not a field-decoded value (docs SS7.9, SS7.34). */
#define PROTIMER_ALL_SOURCES 0xffffffffU

#define VTOR 0xe000ed08U
#define NVIC_ISER1 0xe000e104U
#define NVIC_IRQ36_BIT (1U << 4) /* IRQ36 - 32 = bit 4 of ISER1 */

/* VTOR_NS, confirmed via probe-read-vtor.c. */
#define OLD_VECTOR_TABLE 0x00000000U
/*
 * 256-byte, 0x100-aligned -- clear of the probes' own code at 0x200007d0, which the copy
 * would otherwise overwrite mid-execution, corrupting trivial_irq36_handler before it's ever
 * used.
 */
#define NEW_VECTOR_TABLE 0x20007d00U
#define VECTOR_WORDS 64U
#define IRQ36_VECTOR_INDEX 52U /* 16 fixed exceptions + IRQ36 */

/* Same sentinel address bg22_require_sram_execution uses to prove execution. */
#define SCRATCH_MARKER 0x200007f0
#define SCRATCH_DONE 0x51a7c0deU

#define PROBE_STR_(x) #x
#define PROBE_STR(x) PROBE_STR_(x)

/* The fixed part of stock's PROTIMER bring-up, before IEN and CMD (see probe.c's header). */
static inline void protimer_configure(void)
{
	REG(PROTIMER_EN) = 1U;
	REG(PROTIMER_CTRL) = PROTIMER_CTRL_VAL;
	REG(PROTIMER_REG_030) = PROTIMER_REG_030_VAL;
	REG(PROTIMER_REG_038) = PROTIMER_REG_038_VAL;
}

/* Copy the live 64-word vector table into SRAM, word by word in ascending order. */
static inline volatile uint32_t *copy_vector_table(void)
{
	volatile const uint32_t *src = (volatile const uint32_t *)OLD_VECTOR_TABLE;
	volatile uint32_t *dst = (volatile uint32_t *)NEW_VECTOR_TABLE;

	for (uint32_t i = 0; i < VECTOR_WORDS; i++) {
		dst[i] = src[i];
	}
	return dst;
}

/*
 * The probe's entry point, linked first at the injection address. The debugger resumes it in
 * the halted host firmware's thread context and, after the bkpt, resumes the host at its own
 * halt-point (README.md), so this stub keeps the host's r0-r3, r12, lr and flags intact
 * around the C body. The body returns the marker value, stored here before the restore.
 * Stays asm: it runs on the host's stack with the host's registers live, and every
 * instruction after the marker store must lie below SCRATCH_MARKER, which overlaps this
 * image (the store overwrites the probe's own bytes at +0x20, here this stub's literal).
 * PREFIX runs first, before anything else (cpsid i for the probes that mask interrupts).
 * The flags are pushed with a pad word, so SP stays 8-aligned for the call.
 */
#define PROBE_ENTRY(name, body, prefix)                                                            \
	__attribute__((naked, section(".text.entry"))) void name(void)                             \
	{                                                                                          \
		__asm__ volatile(prefix                                                            \
				 "push.w {r0-r3, r12, lr}\n"                                       \
				 "mrs r0, apsr\n"                                                  \
				 "push {r0, r1}\n"                                                 \
				 "bl " #body "\n"                                                  \
				 "ldr r1, =" PROBE_STR(SCRATCH_MARKER) "\n"                        \
				 "str r0, [r1]\n"                                                  \
				 "pop {r0, r1}\n"                                                  \
				 "msr apsr_nzcvq, r0\n"                                            \
				 "pop.w {r0-r3, r12, lr}\n"                                        \
				 "bkpt #0\n"                                                       \
				 ".ltorg\n");                                                      \
	}

#endif /* PROTIMER_PROBE_H */
