#!/usr/bin/env python3
"""Drive the QEMU HCI UART controller through the vhci chardev without root.

The vhci chardev opens its path with qemu_open(), so it accepts a QEMU fd set.
This script hands QEMU one end of a SOCK_SEQPACKET socket pair as the
"/dev/vhci" of soc1's USART1 and plays the host kernel on the other end with
the same packet semantics: one H4 packet per read()/write(), vendor packet
0xff for controller creation.  It then runs the host's first steps (HCI_Reset,
local version and address reads) and an LE passive scan, and reports every
advertising report together with the host-side (real) latency of each command.

usage: fake-vhci-scan.py <qemu> <soc0-image> <soc1-hci-image> [seconds] [icount]
With CONNECT=1 it then initiates a connection to the first advertiser and
sends an ATT Read By Group Type request, as a GATT client's first step.
"""
import os
import select
import socket
import struct
import subprocess
import sys
import time

H4_COMMAND, H4_EVENT, VHCI_VENDOR = 0x01, 0x04, 0xff
EVT_COMMAND_COMPLETE, EVT_COMMAND_STATUS, EVT_LE_META = 0x0e, 0x0f, 0x3e
LE_ADVERTISING_REPORT = 0x02
# drivers/bluetooth/hci_vhci.c: create a primary controller; reply carries
# the index.  HCI_CMD_TIMEOUT (include/net/bluetooth/hci.h) is 2 s.
VHCI_CREATE_PRIMARY = 0x00
HCI_CMD_TIMEOUT_S = 2.0
FD_SET_ID = 1


def opcode(ogf, ocf):
    return (ogf << 10) | ocf


OGF_HOST_CTL, OGF_INFO, OGF_LE = 0x03, 0x04, 0x08
HCI_SET_EVENT_MASK = opcode(OGF_HOST_CTL, 0x001)
HCI_RESET = opcode(OGF_HOST_CTL, 0x003)
HCI_READ_LOCAL_VERSION = opcode(OGF_INFO, 0x001)
HCI_READ_BD_ADDR = opcode(OGF_INFO, 0x009)
LE_SET_SCAN_PARAMETERS = opcode(OGF_LE, 0x00b)
LE_SET_SCAN_ENABLE = opcode(OGF_LE, 0x00c)
LE_SET_RANDOM_ADDRESS = opcode(OGF_LE, 0x005)
LE_CREATE_CONNECTION = opcode(OGF_LE, 0x00d)
H4_ACL = 0x02
EVT_DISCONNECTION_COMPLETE, EVT_NUM_COMPLETED_PACKETS = 0x05, 0x13
LE_CONNECTION_COMPLETE = 0x01
# ACL packet boundary flag 0b00: first non-automatically-flushable fragment.
ACL_START_NO_FLUSH = 0x0000
L2CAP_CID_ATT = 0x0004
ATT_READ_BY_GROUP_TYPE_REQ, ATT_READ_BY_GROUP_TYPE_RSP = 0x10, 0x11
GATT_PRIMARY_SERVICE_UUID = 0x2800
# A static random address (two most significant bits set).
OWN_STATIC_ADDRESS = bytes.fromhex("c0ffee000001")
# Core Vol 4, Part E, 7.3.1: the default mask without LE Meta (bit 61), which
# a host adds before it scans, as Linux does.
DEFAULT_EVENT_MASK = 0x00001FFFFFFFFFFF
LE_META_EVENT_MASK_BIT = 61


def command(op, params=b""):
    return bytes([H4_COMMAND]) + struct.pack("<HB", op, len(params)) + params


def recv(sock, timeout):
    ready, _, _ = select.select([sock], [], [], timeout)
    return sock.recv(70000) if ready else None


def run_command(sock, op, params=b""):
    sent = time.monotonic()
    sock.send(command(op, params))
    while True:
        remaining = sent + HCI_CMD_TIMEOUT_S - time.monotonic()
        packet = recv(sock, max(remaining, 0))
        if packet is None:
            raise SystemExit(f"opcode 0x{op:04x}: no reply within HCI_CMD_TIMEOUT")
        if packet[0] != H4_EVENT:
            continue
        code = packet[1]
        if code == EVT_COMMAND_COMPLETE and struct.unpack_from("<H", packet, 4)[0] == op:
            latency = time.monotonic() - sent
            return packet[6:], latency
        if code == EVT_COMMAND_STATUS and struct.unpack_from("<H", packet, 5)[0] == op:
            return packet[3:4], time.monotonic() - sent


def main():
    qemu, image0, image1 = sys.argv[1:4]
    seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 10.0
    icount = sys.argv[5] if len(sys.argv) > 5 else "shift=3,sleep=on"
    host, guest = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    console = os.path.join(os.getcwd(), "fake-vhci")
    args = [qemu, "-M", f"efr32bg22-dual,flash0={image0},flash1={image1}",
            "-display", "none", "-monitor", "none", "-icount", icount,
            "-add-fd", f"fd={guest.fileno()},set={FD_SET_ID}",
            "-chardev", f"vhci,id=soc1.usart1,path=/dev/fdset/{FD_SET_ID}",
            "-chardev", f"file,id=soc1.usart0,path={console}-soc1.log",
            "-chardev", f"file,id=soc0.usart1,path={console}-soc0.log"]
    # Extra QEMU options, e.g. traces, from the environment.
    args += os.environ.get("QEMU_EXTRA_ARGS", "").split()
    started = time.monotonic()
    proc = subprocess.Popen(args, pass_fds=[guest.fileno()])
    guest.close()
    try:
        create = recv(host, 10.0)
        if create != bytes([VHCI_VENDOR, VHCI_CREATE_PRIMARY]):
            raise SystemExit(f"no vhci create packet: {create!r}")
        print(f"vhci create after {time.monotonic() - started:.3f} s real")
        host.send(bytes([VHCI_VENDOR, VHCI_CREATE_PRIMARY, 0x7f, 0x00]))
        status, dt = run_command(host, HCI_RESET)
        print(f"HCI_Reset status=0x{status[0]:02x} in {dt * 1000:.1f} ms")
        ret, dt = run_command(host, HCI_READ_LOCAL_VERSION)
        print(f"Read_Local_Version {ret.hex()} in {dt * 1000:.1f} ms")
        ret, dt = run_command(host, HCI_READ_BD_ADDR)
        print(f"Read_BD_ADDR {ret.hex()} in {dt * 1000:.1f} ms")
        mask = DEFAULT_EVENT_MASK | (1 << LE_META_EVENT_MASK_BIT)
        ret, dt = run_command(host, HCI_SET_EVENT_MASK, struct.pack("<Q", mask))
        print(f"Set_Event_Mask status=0x{ret[0]:02x} in {dt * 1000:.1f} ms")
        # Passive scan with tests/radio-rx-port's 100 ms interval and window
        # (0.625 ms units), public address, accept all.
        scan_window = struct.pack("<BHHBB", 0x00, 0x00a0, 0x00a0, 0x00, 0x00)
        ret, dt = run_command(host, LE_SET_SCAN_PARAMETERS, scan_window)
        print(f"LE_Set_Scan_Parameters status=0x{ret[0]:02x} in {dt * 1000:.1f} ms")
        ret, dt = run_command(host, LE_SET_SCAN_ENABLE, bytes([0x01, 0x00]))
        print(f"LE_Set_Scan_Enable status=0x{ret[0]:02x} in {dt * 1000:.1f} ms")
        reports, first = 0, None
        scan_start = time.monotonic()
        while time.monotonic() - scan_start < seconds:
            packet = recv(host, 0.5)
            if not packet or packet[0] != H4_EVENT:
                continue
            if packet[1] == EVT_LE_META and packet[3] == LE_ADVERTISING_REPORT:
                reports += 1
                if first is None:
                    first = time.monotonic() - scan_start
                    addr = packet[7:13][::-1].hex(":")
                    peer_type, peer = packet[6], packet[7:13]
                    data = packet[14:14 + packet[13]]
                    print(f"first report after {first:.3f} s: {addr} type={packet[5]} "
                          f"rssi={struct.unpack('b', packet[14 + packet[13]:][:1])[0]} data={data.hex()}")
        print(f"{reports} advertising reports in {seconds:.0f} s real")
        if not reports:
            return 1
        if os.environ.get("CONNECT") != "1":
            return 0
        return connect(host, peer_type, peer, seconds)
    finally:
        proc.terminate()
        proc.wait(5)


def connect(host, peer_type, peer, seconds):
    """Initiate a connection and send one ATT request, as a GATT client would."""
    run_command(host, LE_SET_SCAN_ENABLE, bytes([0x00, 0x00]))
    ret, _ = run_command(host, LE_SET_RANDOM_ADDRESS, OWN_STATIC_ADDRESS[::-1])
    print(f"LE_Set_Random_Address status=0x{ret[0]:02x}")
    # BlueZ's defaults: 60 ms scan interval/window, 30-50 ms connection
    # interval (hdev->le_conn_min/max_interval 0x0018/0x0028), 420 ms
    # supervision timeout (le_supv_timeout 0x002a).  Own address random.
    params = struct.pack("<HHBB6sBHHHHHH", 0x0060, 0x0060, 0x00, peer_type, peer,
                         0x01, 0x0018, 0x0028, 0x0000, 0x002a, 0x0000, 0x0000)
    ret, dt = run_command(host, LE_CREATE_CONNECTION, params)
    print(f"LE_Create_Connection status=0x{ret[0]:02x} in {dt * 1000:.1f} ms")
    handle = None
    start = time.monotonic()
    att_sent = False
    while time.monotonic() - start < seconds:
        packet = recv(host, 0.5)
        if not packet:
            continue
        if packet[0] == H4_ACL:
            print(f"ACL from controller after {time.monotonic() - start:.3f} s: {packet.hex()}")
            if packet[9] == ATT_READ_BY_GROUP_TYPE_RSP:
                return 0
            continue
        if packet[0] != H4_EVENT:
            continue
        if packet[1] == EVT_LE_META and packet[3] == LE_CONNECTION_COMPLETE:
            status, handle = packet[4], struct.unpack_from("<H", packet, 5)[0]
            print(f"LE Connection Complete after {time.monotonic() - start:.3f} s: "
                  f"status=0x{status:02x} handle=0x{handle:04x} "
                  f"interval={struct.unpack_from('<H', packet, 15)[0]}")
            if status:
                return 1
        elif packet[1] == EVT_DISCONNECTION_COMPLETE:
            print(f"Disconnection Complete after {time.monotonic() - start:.3f} s: "
                  f"reason=0x{packet[6]:02x}")
            return 1
        elif packet[1] == EVT_NUM_COMPLETED_PACKETS:
            print(f"Number Of Completed Packets after {time.monotonic() - start:.3f} s")
        if handle is not None and not att_sent:
            # ATT Read By Group Type Request: primary services 0x0001-0xffff.
            att = struct.pack("<BHHH", ATT_READ_BY_GROUP_TYPE_REQ, 0x0001, 0xffff,
                              GATT_PRIMARY_SERVICE_UUID)
            l2cap = struct.pack("<HH", len(att), L2CAP_CID_ATT) + att
            acl = struct.pack("<BHH", H4_ACL, handle | ACL_START_NO_FLUSH, len(l2cap)) + l2cap
            host.send(acl)
            att_sent = True
    print("no ATT response")
    return 1


if __name__ == "__main__":
    sys.exit(main())
