/* SPDX-License-Identifier: Apache-2.0 */
/* QEMU fixture: release the radio M0+, then request a TXWARM interrupt. */
.syntax unified
.cpu cortex-m33
.thumb

.equ CMU_CLKEN1_SET,       0x40009068
.equ CMU_RADIOCLKCTRL_SET, 0x40009280
.equ CMU_RADIO_BLOCKS,     0x0000087f
.equ CMU_RADIOCLKCTRL_EN,  0x00000001
.equ RAC_CMD,              0xa8020010
.equ RAC_CTRL,             0xa8020014
.equ RAC_CTRL_SEQRESET,    (1 << 24)
.equ RAC_CTRL_CPUWAITDIS,  (1 << 26)
.equ RAC_CMD_TXEN,         (1 << 0)

.section .vectors, "a", %progbits
.word 0x20008000
.word reset + 1
.rept 14
.word fault + 1
.endr

.section .text, "ax", %progbits
.global reset
.thumb_func
reset:
    cpsid i
    ldr r0, =CMU_RADIOCLKCTRL_SET
    ldr r1, =CMU_RADIOCLKCTRL_EN
    str r1, [r0]
    ldr r0, =CMU_CLKEN1_SET
    ldr r1, =CMU_RADIO_BLOCKS
    str r1, [r0]

    ldr r0, =RAC_CTRL
    ldr r1, =RAC_CTRL_SEQRESET
    str r1, [r0]
    ldr r1, =RAC_CTRL_CPUWAITDIS
    str r1, [r0]

    /* TXEN enters TXWARM and asserts external IRQ8 on the released M0+. */
    ldr r0, =RAC_CMD
    ldr r1, =RAC_CMD_TXEN
    str r1, [r0]
1:  wfi
    b 1b

.thumb_func
fault:
    b fault
