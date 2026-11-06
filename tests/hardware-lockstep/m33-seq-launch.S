/* SPDX-License-Identifier: Apache-2.0 */
/* QEMU fixture: release a preloaded radio M0+ image after machine reset. */
.syntax unified
.cpu cortex-m33
.thumb

.equ RAC_CTRL,           0xa8020014
.equ RAC_CTRL_SEQRESET,  (1 << 24)
.equ RAC_CTRL_CPUWAITDIS, (1 << 26)

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
    ldr r0, =RAC_CTRL
    ldr r1, =RAC_CTRL_SEQRESET
    str r1, [r0]
    ldr r1, =RAC_CTRL_CPUWAITDIS
    str r1, [r0]
1:  wfi
    b 1b

.thumb_func
fault:
    b fault
