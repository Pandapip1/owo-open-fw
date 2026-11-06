#!/usr/bin/env python3
"""Generate a temporary MMIO replay probe from AP3 store-trace JSON files."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load(path: Path) -> dict:
    text = path.read_text()
    return json.loads(text[text.index("{"):])


def table(name: str, trace: dict) -> list[str]:
    lines = [f"{name}:"]
    for event in trace["stores"]:
        lines.extend((
            f"    /* vendor PC {event['pc']}, trace step {event['step']} */",
            f"    .word {event['address']}, {event['value']}, {event['width']}",
        ))
    lines.append("    .word 0, 0, 0")
    return lines


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--extra-irq", action="append", default=[], metavar="IRQ=TRACE",
        help="emit an additional captured IRQ table (repeatable)",
    )
    parser.add_argument(
        "--extra-output", type=Path,
        help="write only --extra-irq tables for inclusion by the hand-curated fixture",
    )
    parser.add_argument("--irq2", type=Path)
    parser.add_argument("--irq8", type=Path)
    parser.add_argument("--irq9", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    if args.extra_output is not None:
        if not args.extra_irq:
            parser.error("--extra-output requires at least one --extra-irq")
        emitted: list[str] = [
            "/* Generated silicon-characterization tables; not production firmware. */"
        ]
        for item in args.extra_irq:
            try:
                irq_text, path_text = item.split("=", 1)
                irq = int(irq_text, 0)
            except ValueError as error:
                parser.error(f"invalid --extra-irq {item!r}: {error}")
            trace = load(Path(path_text))
            if trace["entry_exception"] != 16 + irq:
                parser.error(
                    f"trace {path_text} is exception {trace['entry_exception']}, not IRQ{irq}"
                )
            emitted.extend(("", *table(f"irq{irq}_table", trace)))
        args.extra_output.write_text("\n".join(emitted) + "\n")
        return

    if args.irq8 is None or args.irq9 is None or args.output is None:
        parser.error("normal generation requires --irq8, --irq9, and --output")
    irq8 = load(args.irq8)
    irq9 = load(args.irq9)
    irq2 = load(args.irq2) if args.irq2 else None
    if irq8["entry_exception"] != 24 or irq9["entry_exception"] != 25:
        raise SystemExit("trace exception numbers do not identify IRQ8/IRQ9")
    if irq2 is not None and irq2["entry_exception"] != 18:
        raise SystemExit("trace exception number does not identify IRQ2")

    source = """/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Generated silicon-characterization fixture, not production firmware.
 * It replays the ordered MMIO stores observed by single-stepping the shipped
 * controller through AP3. Raw values deliberately retain their trace PC and
 * must be replaced by named fields/phase logic before entering the open HAL.
 */
.syntax unified
.cpu cortex-m0plus
.thumb

.equ SEQ_SPARE0, 0x00004fe0
.equ NVIC_ISER,  0xe000e100

.section .vectors, "a", %progbits
.word 0x00003e00
.word reset + 1
.rept 62
.word exception + 1
.endr

.section .text, "ax", %progbits
.global reset
.thumb_func
reset:
    cpsid i
    ldr r0, =SEQ_SPARE0
    ldr r1, =0x52504c59       /* RPLY */
    str r1, [r0]
    ldr r0, =NVIC_ISER
    movs r1, #1
    rsbs r1, r1, #0
    str r1, [r0]
    str r1, [r0, #4]
    cpsie i
1:  wfi
    b 1b

.thumb_func
exception:
    push {r4-r7, lr}
    mrs r4, ipsr
    ldr r5, =SEQ_SPARE0
    str r4, [r5]
    cmp r4, #18
    bne 1f
    ldr r0, =irq2_table
    bl replay
    b 3f
1:
    cmp r4, #24
    bne 2f
    ldr r0, =irq8_table
    bl replay
    b 3f
2:  cmp r4, #25
    bne 3f
    ldr r0, =irq9_table
    bl replay
3:  pop {r4-r7, pc}

.thumb_func
replay:
    push {r4-r7, lr}
4:  ldmia r0!, {r1-r3}
    cmp r1, #0
    beq 8f
    cmp r3, #1
    beq 5f
    cmp r3, #2
    beq 6f
    str r2, [r1]
    b 7f
5:  strb r2, [r1]
    b 7f
6:  strh r2, [r1]
7:  dsb
    b 4b
8:  pop {r4-r7, pc}

.ltorg
.align 2
"""
    if irq2 is not None:
        source += "\n".join(table("irq2_table", irq2)) + "\n\n"
    source += "\n".join(table("irq8_table", irq8)) + "\n\n"
    source += "\n".join(table("irq9_table", irq9)) + "\n"
    args.output.write_text(source)


if __name__ == "__main__":
    main()
