#!/usr/bin/env python3
"""Compare event-boundary state through two GDB Remote Serial Protocol stubs.

One endpoint is normally OpenOCD attached to the physical BG22 and the other
is QEMU's built-in gdbstub.  No GDB installation or vendor probe library is
required; this deliberately uses only the small common RSP subset.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import socket
import sys
from dataclasses import dataclass


def parse_int(value: str) -> int:
    return int(value, 0)


def checksum(payload: bytes) -> bytes:
    return f"{sum(payload) & 0xff:02x}".encode()


@dataclass(frozen=True)
class Region:
    name: str
    address: int
    length: int


class RspError(RuntimeError):
    pass


class RspClient:
    def __init__(self, endpoint: str, timeout: float):
        host, port = endpoint.rsplit(":", 1)
        self.sock = socket.create_connection((host, int(port)), timeout)
        self.sock.settimeout(timeout)
        self.no_ack = False
        self.thread: str | None = None

    def close(self) -> None:
        self.sock.close()

    def _byte(self) -> bytes:
        value = self.sock.recv(1)
        if not value:
            raise RspError("GDB endpoint closed the connection")
        return value

    def _receive(self) -> str:
        while self._byte() != b"$":
            pass
        payload = bytearray()
        while True:
            value = self._byte()
            if value == b"#":
                break
            if value == b"}":
                value = bytes([self._byte()[0] ^ 0x20])
            payload.extend(value)
        received = self.sock.recv(2)
        if received.lower() != checksum(payload):
            if not self.no_ack:
                self.sock.sendall(b"-")
            raise RspError("bad RSP checksum")
        if not self.no_ack:
            self.sock.sendall(b"+")
        return payload.decode("ascii")

    def command(self, command: str, *, wait_for_stop: bool = False) -> str:
        payload = command.encode("ascii")
        self.sock.sendall(b"$" + payload + b"#" + checksum(payload))
        if not self.no_ack:
            while self._byte() != b"+":
                pass
        reply = self._receive()
        # OpenOCD can forward semihosting/console bytes as one or more `Oxx`
        # packets while a continue request is outstanding.  They are
        # asynchronous output, not the stop acknowledgement at our checkpoint.
        if wait_for_stop:
            while reply.startswith("O"):
                reply = self._receive()
        if reply.startswith("E"):
            raise RspError(f"{command}: remote error {reply}")
        return reply

    def initialize(self) -> None:
        self.command("qSupported:multiprocess+;swbreak+")
        if self.command("QStartNoAckMode") == "OK":
            self.no_ack = True
        self.command("?")

    def select_thread(self, thread: str) -> None:
        for operation in ("g", "c"):
            if self.command(f"H{operation}{thread}") != "OK":
                raise RspError(f"could not select thread {thread!r}")
        self.thread = thread

    def read_memory(self, address: int, length: int) -> bytes:
        reply = self.command(f"m{address:x},{length:x}")
        data = bytes.fromhex(reply)
        if len(data) != length:
            raise RspError(
                f"short read at {address:#x}: wanted {length}, got {len(data)}"
            )
        return data

    def registers(self) -> bytes:
        return bytes.fromhex(self.command("g"))

    def write_registers(self, registers: bytes) -> None:
        if self.command("G" + registers.hex()) != "OK":
            raise RspError("remote rejected register normalization")

    def continue_to(self, address: int, *, hardware: bool = False) -> str:
        breakpoint_type = 1 if hardware else 0
        if self.command(f"Z{breakpoint_type},{address:x},2") != "OK":
            raise RspError(f"could not install breakpoint at {address:#x}")
        try:
            try:
                return self.command("c", wait_for_stop=True)
            except socket.timeout as error:
                # A remote target remains running after a receive timeout.  It
                # cannot process breakpoint-removal packets until an RSP
                # interrupt has stopped it, so restore a debuggable state
                # before unwinding.
                self.sock.sendall(b"\x03")
                self._receive()
                raise RspError(
                    f"target did not reach breakpoint {address:#x} before timeout"
                ) from error
        finally:
            self.command(f"z{breakpoint_type},{address:x},2")

    def step(self) -> str:
        if self.thread:
            return self.command(f"vCont;s:{self.thread}", wait_for_stop=True)
        return self.command("s", wait_for_stop=True)


def parse_region(value: str) -> Region:
    try:
        name, extent = value.split("=", 1)
        address, length = extent.split(":", 1)
        return Region(name, parse_int(address), parse_int(length))
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "region must be NAME=ADDRESS:LENGTH"
        ) from error


def changed_words(left: bytes, right: bytes, base: int) -> list[dict[str, object]]:
    changes = []
    for offset in range(0, max(len(left), len(right)), 4):
        lhs = left[offset : offset + 4]
        rhs = right[offset : offset + 4]
        if lhs != rhs:
            changes.append(
                {
                    "address": f"0x{base + offset:08x}",
                    "hardware": f"0x{int.from_bytes(lhs, 'little'):08x}",
                    "qemu": f"0x{int.from_bytes(rhs, 'little'):08x}",
                }
            )
    return changes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hardware", default="127.0.0.1:3333")
    parser.add_argument("--qemu", default="127.0.0.1:3334")
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--continue-to", type=parse_int, metavar="ADDRESS")
    parser.add_argument(
        "--current", action="append", choices=("hardware", "qemu"), default=[],
        help="endpoint is already stopped at the checkpoint; do not continue it",
    )
    parser.add_argument("--registers", action="store_true")
    parser.add_argument(
        "--normalize-registers", action="store_true",
        help="copy QEMU's complete register packet to hardware before stepping",
    )
    parser.add_argument(
        "--set-pc", type=parse_int, metavar="ADDRESS",
        help="with --normalize-registers, set both Cortex-M PCs before stepping",
    )
    parser.add_argument("--hardware-thread")
    parser.add_argument("--qemu-thread")
    parser.add_argument(
        "--steps", type=int, default=0,
        help="single-step both selected cores and report the first register divergence",
    )
    parser.add_argument(
        "--region", action="append", type=parse_region, required=True,
        help="repeatable NAME=ADDRESS:LENGTH snapshot",
    )
    args = parser.parse_args()

    clients = {
        "hardware": RspClient(args.hardware, args.timeout),
        "qemu": RspClient(args.qemu, args.timeout),
    }
    try:
        for client in clients.values():
            client.initialize()
        if args.hardware_thread:
            clients["hardware"].select_thread(args.hardware_thread)
        if args.qemu_thread:
            clients["qemu"].select_thread(args.qemu_thread)
        stops: dict[str, str] = {}
        if args.continue_to is not None:
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                pending = {
                    name: pool.submit(client.continue_to, args.continue_to)
                    for name, client in clients.items()
                    if name not in args.current
                }
                stops = {name: future.result() for name, future in pending.items()}
            stops.update({name: "current" for name in args.current})

        report: dict[str, object] = {"stops": stops, "regions": {}}
        differences = 0
        if args.normalize_registers:
            qemu_registers = bytearray(clients["qemu"].registers())
            hardware_registers = clients["hardware"].registers()
            if len(qemu_registers) != len(hardware_registers):
                raise RspError(
                    "cannot normalize different register packet layouts: "
                    f"hardware={len(hardware_registers)}, qemu={len(qemu_registers)}"
                )
            if args.set_pc is not None:
                # The common Cortex-M RSP core packet is r0..r15,xPSR as
                # little-endian 32-bit words. Refuse a shorter/non-core packet
                # rather than silently changing an unknown register layout.
                if len(qemu_registers) < 68:
                    raise RspError("register packet is too short for Cortex-M PC")
                qemu_registers[60:64] = args.set_pc.to_bytes(4, "little")
            normalized = bytes(qemu_registers)
            clients["hardware"].write_registers(normalized)
            clients["qemu"].write_registers(normalized)
            report["registers_normalized"] = len(qemu_registers)
        if args.steps:
            for step in range(1, args.steps + 1):
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                    pending = {
                        name: pool.submit(client.step)
                        for name, client in clients.items()
                    }
                    step_stops = {}
                    for name, future in pending.items():
                        try:
                            step_stops[name] = future.result()
                        except Exception as error:
                            raise RspError(f"{name} single-step failed: {error}") from error
                step_registers = {
                    name: client.registers() for name, client in clients.items()
                }
                if step_registers["hardware"] != step_registers["qemu"]:
                    report["step_divergence"] = {
                        "step": step,
                        "stops": step_stops,
                        "hardware": step_registers["hardware"].hex(),
                        "qemu": step_registers["qemu"].hex(),
                    }
                    differences += 1
                    break
            else:
                report["steps_equal"] = args.steps
        if args.registers:
            registers = {name: client.registers() for name, client in clients.items()}
            report["register_bytes"] = {
                name: value.hex() for name, value in registers.items()
            }
            report["registers_equal"] = registers["hardware"] == registers["qemu"]
            differences += not report["registers_equal"]

        for region in args.region:
            values = {
                name: client.read_memory(region.address, region.length)
                for name, client in clients.items()
            }
            changes = changed_words(values["hardware"], values["qemu"], region.address)
            report["regions"][region.name] = {
                "address": f"0x{region.address:08x}",
                "length": region.length,
                "differences": changes,
            }
            differences += len(changes)
        print(json.dumps(report, indent=2))
        return 1 if differences else 0
    finally:
        for client in clients.values():
            client.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RspError, concurrent.futures.TimeoutError) as error:
        print(f"lockstep comparison failed: {error}", file=sys.stderr)
        sys.exit(2)
