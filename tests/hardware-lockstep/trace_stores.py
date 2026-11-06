#!/usr/bin/env python3
"""Trace Cortex-M MMIO stores while single-stepping an OpenOCD RSP target."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from compare import RspClient, RspError, parse_int


LINE = re.compile(
    r"^\s*([0-9a-fA-F]+):\s+(?:[0-9a-fA-F]{4}\s+)+"
    r"([a-z][a-z0-9.]*)\s*(.*?)\s*(?:@.*)?$"
)
STORE = re.compile(
    r"^(r(?:1[0-2]|[0-9])),\s*\[(r(?:1[0-5]|[0-9]))"
    r"(?:,\s*#(-?(?:0x[0-9a-fA-F]+|[0-9]+)))?\]!?$"
)


def disassembly(path: Path) -> dict[int, tuple[str, str]]:
    instructions: dict[int, tuple[str, str]] = {}
    for line in path.read_text().splitlines():
        match = LINE.match(line)
        if match:
            instructions[int(match.group(1), 16)] = (match.group(2), match.group(3))
    return instructions


def core_registers(packet: bytes) -> list[int]:
    if len(packet) < 68:
        raise RspError(f"short Cortex-M register packet: {len(packet)} bytes")
    return [int.from_bytes(packet[offset : offset + 4], "little")
            for offset in range(0, 68, 4)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", default="127.0.0.1:3334")
    parser.add_argument("--disassembly", type=Path, required=True)
    parser.add_argument("--breakpoint", type=parse_int, required=True)
    parser.add_argument(
        "--current", action="store_true",
        help="start from an already halted entry instead of continuing to the breakpoint",
    )
    parser.add_argument(
        "--hardware-breakpoint", action="store_true",
        help="use a flash-safe hardware breakpoint for the entry address",
    )
    parser.add_argument(
        "--mask-interrupts", action="store_true",
        help="set Cortex-M PRIMASK after the entry breakpoint",
    )
    parser.add_argument(
        "--stop-address", type=parse_int,
        help="stop before executing this address (for reset/startup traces)",
    )
    parser.add_argument("--max-steps", type=int, default=10000)
    parser.add_argument(
        "--output", type=Path,
        help="write JSON directly to this path instead of standard output",
    )
    parser.add_argument(
        "--progress-steps", type=int, default=0,
        help="report the current PC to stderr every N steps",
    )
    parser.add_argument(
        "--run-through", action="append", default=[], metavar="FROM=TO",
        help=("resume from a polling-loop PC to a temporary hardware breakpoint; "
              "use when debug single-step freezes the peripheral being polled"),
    )
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--mmio-min", type=parse_int, default=0xA0000000)
    args = parser.parse_args()
    run_through: dict[int, int] = {}
    for item in args.run_through:
        try:
            source_text, target_text = item.split("=", 1)
            run_through[parse_int(source_text)] = parse_int(target_text)
        except ValueError as error:
            parser.error(f"invalid --run-through {item!r}: {error}")

    decoded = disassembly(args.disassembly)
    client = RspClient(args.endpoint, args.timeout)
    events: list[dict[str, object]] = []
    try:
        client.initialize()
        if args.current:
            current_pc = core_registers(client.registers())[15] & ~1
            if current_pc != args.breakpoint:
                raise RspError(
                    f"target is halted at {current_pc:#x}, not entry {args.breakpoint:#x}"
                )
            stop = "current"
        else:
            stop = client.continue_to(
                args.breakpoint, hardware=args.hardware_breakpoint
            )
        if args.mask_interrupts:
            # This image executes nonsecure. OpenOCD exposes its one-bit
            # PRIMASK_NS as GDB register 40 (hex 28), separately from the
            # secure/current-bank PRIMASK register 20.
            if client.command("P28=01") != "OK":
                raise RspError("remote rejected PRIMASK_NS update")
            if client.command("p28") != "01":
                raise RspError("PRIMASK_NS did not retain the masked state")
        entry = core_registers(client.registers())
        entry_exception = entry[16] & 0x1FF
        complete = False
        boundary = None
        for step in range(args.max_steps):
            regs = core_registers(client.registers())
            pc = regs[15] & ~1
            if args.progress_steps and step % args.progress_steps == 0:
                print(f"trace-stores: step={step} pc={pc:#010x} "
                      f"stores={len(events)}", file=sys.stderr, flush=True)
            if args.stop_address is not None and step and pc == args.stop_address:
                after = regs
                complete = True
                boundary = "stop-address"
                break
            instruction = decoded.get(pc)
            if pc in run_through:
                client.continue_to(run_through[pc], hardware=True)
                after = core_registers(client.registers())
                continue
            if instruction:
                mnemonic, operands = instruction
                match = STORE.match(operands) if mnemonic.startswith("str") else None
                if match:
                    source = int(match.group(1)[1:])
                    base = int(match.group(2)[1:])
                    displacement = int(match.group(3), 0) if match.group(3) else 0
                    address = (regs[base] + displacement) & 0xFFFFFFFF
                    if address >= args.mmio_min:
                        width = 1 if mnemonic.startswith("strb") else (
                            2 if mnemonic.startswith("strh") else 4
                        )
                        events.append({
                            "step": step,
                            "pc": f"0x{pc:08x}",
                            "instruction": f"{mnemonic} {operands}",
                            "address": f"0x{address:08x}",
                            "value": f"0x{regs[source] & ((1 << (8 * width)) - 1):0{2 * width}x}",
                            "width": width,
                            "exception": regs[16] & 0x1FF,
                        })
            client.step()
            after = core_registers(client.registers())
            if (args.stop_address is None and step and
                    (after[16] & 0x1FF) != entry_exception):
                complete = True
                boundary = "exception-return"
                break
        else:
            boundary = (f"stop address {args.stop_address:#x} was not reached"
                        if args.stop_address is not None else
                        "handler did not return")

        result = json.dumps({
            "stop": stop,
            "complete": complete,
            "boundary": boundary,
            "entry_exception": entry_exception,
            "entry_lr": f"0x{entry[14]:08x}",
            "steps": step + 1,
            "exit_exception": after[16] & 0x1FF,
            "stop_address": (f"0x{args.stop_address:08x}"
                             if args.stop_address is not None else None),
            "stores": events,
        }, indent=2)
        if args.output is None:
            print(result)
        else:
            args.output.write_text(result + "\n")
        if not complete:
            print(f"trace-stores: {boundary} within {args.max_steps} steps; "
                  "emitting partial trace", file=sys.stderr)
            return 2
        return 0
    finally:
        client.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RspError, ValueError) as error:
        print(f"trace-stores: {error}", file=sys.stderr)
        raise SystemExit(2)
