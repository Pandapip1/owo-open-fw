/* SPDX-License-Identifier: Apache-2.0 */
/*
 * EFR32BG22 PROTIMER enable probe.
 *
 * This is standalone Cortex-M33 code, independent of Zephyr and the real
 * driver (modules/efr32-radio/hal/radio.c). It is NOT a bootable image: it
 * has no vector table and is meant to be injected into SRAM and executed
 * from a halt-point inside the already-running, already-correctly-
 * configured production firmware (same SRAM-injection technique
 * scripts/bg22-openocd-flash.cfg's bg22_require_sram_execution already uses
 * reliably), right before that firmware's own RAC.CMD.TXEN write.
 *
 * WHY a separate probe instead of extending radio.c directly: the question
 * being tested ("does enabling PROTIMER, with no scheduling logic at all,
 * change FRC's behavior") doesn't need the real driver's own TX-scheduling
 * path touched at all, and hand-replicating the ~30-write PHY config table
 * in a from-scratch bare-metal image would risk exactly the kind of
 * transcription error this project has caught before (see
 * docs/radio-tables.md SS7.13's PROTIMER_0x38/FRC_0x1C mixup). Reusing the
 * real, already-correct firmware for everything except the one new
 * variable (PROTIMER) isolates that variable cleanly.
 *
 * Deliberately leaves NVIC IRQ36 (PROTIMER) masked: this probe tests only
 * whether a *running, clocked* PROTIMER changes FRC's behavior, not
 * whether servicing its interrupt does. Enabling the IRQ without a real
 * vector-table entry (Zephyr's table is flash-resident; this probe cannot
 * safely patch it at runtime) is what crashed the earlier live register
 * poke (docs/radio-tables.md SS7.32). A follow-up probe that *does* want to
 * test the IRQ-handshake sub-hypothesis needs a real installed handler,
 * which needs either a from-scratch bootable image (like
 * tests/hardware-lockstep/m33-seq-txwarm.S) or Zephyr's own
 * IRQ_CONNECT()/software-ISR-table mechanism -- not attempted here.
 *
 * Register values below are the exact sequence stock is observed to issue
 * at PROTIMER bring-up (docs/radio-tables.md SS7.9's "radio-trace.log:64-67"
 * citation and generic_phy.o's enable helper at flash offset 0x29a3a,
 * cross-confirmed independently this session via tests/hardware-lockstep's
 * stage2-pre-txen-protimer.bin capture, see SS7.32). What each bit/field
 * actually means is NOT established (the cited docs are explicit that
 * PROTIMER's command/IF/IEN semantics remain unresolved) -- this probe
 * replicates the observed write *sequence* as a best-effort "do what stock
 * does", not a field-decoded, understood configuration. Do not copy these
 * values into production code without first resolving that gap.
 */

.syntax unified
.cpu cortex-m33
.thumb

.equ PROTIMER_BASE,      0xb801c000 /* non-secure alias: this probe runs in whatever CPU security state the host firmware left it in (Non-secure, like the rest of the real driver), and the secure 0xa801c000 window silently drops writes from there -- confirmed by running this probe once against the secure address: it completed with no fault but every readback stayed 0 */
.equ PROTIMER_EN,        PROTIMER_BASE + 0x004  /* EN.EN, same one-bit latch pattern FRC/AGC/RFCRC/MODEM/SYNTH/RAC each have at +0x004 (docs/radio-tables.md SS7, public *_S_register.py:24-37) */
.equ PROTIMER_CTRL,      PROTIMER_BASE + 0x008  /* value observed, not field-decoded -- see file header */
.equ PROTIMER_CTRL_VAL,  0x00011102
.equ PROTIMER_CMD,       PROTIMER_BASE + 0x00c  /* stock pairs "1" here with enabling NVIC IRQ36, "4" with disabling it (docs SS7.9) */
.equ PROTIMER_CMD_START, 0x00000001
.equ PROTIMER_REG_030,   PROTIMER_BASE + 0x030  /* "timer state/period input", not further resolved (docs SS7.9) */
.equ PROTIMER_REG_030_VAL, 0x00001233
.equ PROTIMER_REG_038,   PROTIMER_BASE + 0x038
.equ PROTIMER_REG_038_VAL, 0x80015558
.equ PROTIMER_COUNTER,   PROTIMER_BASE + 0x020  /* matches vendor/qemu's own modeled offset AND the offset observed changing live in stock's capture (docs SS7.32) */

.equ SCRATCH_MARKER,     0x200007f0  /* same sentinel address bg22_require_sram_execution uses to prove execution */
.equ SCRATCH_DONE,       0x51a7c0de

.section .text, "ax", %progbits
.global protimer_probe_enable
.thumb_func
protimer_probe_enable:
	cpsid i

	ldr	r0, =PROTIMER_EN
	movs	r1, #1
	str	r1, [r0]

	ldr	r0, =PROTIMER_CTRL
	ldr	r1, =PROTIMER_CTRL_VAL
	str	r1, [r0]

	ldr	r0, =PROTIMER_REG_030
	ldr	r1, =PROTIMER_REG_030_VAL
	str	r1, [r0]

	ldr	r0, =PROTIMER_REG_038
	ldr	r1, =PROTIMER_REG_038_VAL
	str	r1, [r0]

	ldr	r0, =PROTIMER_CMD
	ldr	r1, =PROTIMER_CMD_START
	str	r1, [r0]

	/* Deliberately no NVIC enable here -- see file header. */

	ldr	r0, =SCRATCH_MARKER
	ldr	r1, =SCRATCH_DONE
	str	r1, [r0]
	bkpt	#0
