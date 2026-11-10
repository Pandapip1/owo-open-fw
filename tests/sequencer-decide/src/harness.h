/*
 * SEQRAM the decision harness uses, in the sequencer's own addresses (the M33 sees SEQRAM at
 * 0xb0000000): between the open image (below 0x1400) and the stack it grows down from 0x3e00.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SEQUENCER_DECIDE_HARNESS_H
#define SEQUENCER_DECIDE_HARNESS_H

/* The M0+ entry (tramp.c, linked here by tramp.ld). */
#define HARNESS_ENTRY 0x3000U
/* Control words. */
#define HARNESS_BLOCK 0x3400U
#define HARNESS_DECIDE 0x00 /* efr32_seq_conn_decide's address, Thumb bit set */
#define HARNESS_RET 0x04    /* what it returned */
#define HARNESS_BEFORE 0x08 /* SysTick CVR just before the call */
#define HARNESS_AFTER 0x0c  /* and just after it */
#define HARNESS_RELOAD 0x10 /* SysTick RVR (CVR counts down from it) */
#define HARNESS_DONE 0x14   /* calls completed */
/* The received frame's RX FIFO (EFR32_SEQ_CONN_RX_BASE, mask 0xff) and the staged PDUs. */
#define HARNESS_RX_FIFO 0x3500U
#define HARNESS_RX_MASK 0xffU
#define HARNESS_CUR 0x3600U
#define HARNESS_NEXT 0x3680U

#endif
