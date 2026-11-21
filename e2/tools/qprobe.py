#!/usr/bin/env python3
"""Minimal GDB RSP client for qemu-user, to read real CPU state at a guest address.

The interpreter and qemu disagree somewhere in CPython's start-up, and reading that code
only produces hypotheses. This asks the reference implementation directly: stop at an
address, read registers and memory, and compare.

Usage:
  qprobe.py --port 12345 --at 0x1017b004 [--regs x0,x24,x27] [--mem 0x10806068,8]
"""
import argparse
import socket
import sys
import time


class Rsp:
    def __init__(self, port, timeout=10.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""

    def _recv_packet(self):
        """Read exactly one packet, discarding ack bytes and anything stale."""
        while True:
            start = self.buf.find(b"$")
            while start < 0:
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise EOFError("qemu closed the connection")
                self.buf += chunk
                start = self.buf.find(b"$")
            end = self.buf.find(b"#", start)
            while end < 0:
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise EOFError("qemu closed the connection")
                self.buf += chunk
                end = self.buf.find(b"#", start)
            payload = self.buf[start + 1:end]
            self.buf = self.buf[end + 3:]
            self.sock.sendall(b"+")
            return payload.decode("ascii", "replace")

    def cmd(self, payload):
        data = payload.encode("ascii")
        checksum = sum(data) & 0xFF
        self.sock.sendall(b"$" + data + b"#" + ("%02x" % checksum).encode("ascii"))
        return self._recv_packet()

    def regs(self):
        raw = self.cmd("g")
        # aarch64 layout qemu uses: x0..x30 and sp and pc are 8 bytes, cpsr is 4 - so the
        # packet is 268 bytes (536 hex characters), not the 272 a uniform reading assumes.
        if len(raw) < 31 * 16 + 16 + 16 + 8:
            raise ValueError("short register packet (%d chars)" % len(raw))
        out = {}
        offset = 0
        for i in range(31):
            out["x%d" % i] = int.from_bytes(bytes.fromhex(raw[offset:offset + 16]), "little")
            offset += 16
        out["sp"] = int.from_bytes(bytes.fromhex(raw[offset:offset + 16]), "little")
        offset += 16
        out["pc"] = int.from_bytes(bytes.fromhex(raw[offset:offset + 16]), "little")
        offset += 16
        out["cpsr"] = int.from_bytes(bytes.fromhex(raw[offset:offset + 8]), "little")
        return out

    def mem(self, addr, length):
        raw = self.cmd("m%x,%x" % (addr, length))
        if raw.startswith("E"):
            raise ValueError("qemu refused to read 0x%x: %s" % (addr, raw))
        return bytes.fromhex(raw)

    def set_break(self, addr):
        return self.cmd("Z0,%x,4" % addr)

    def cont(self):
        return self.cmd("c")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=12345)
    ap.add_argument("--at", required=True)
    ap.add_argument("--regs", default="x0,x1,x2,x3,x19,x24,x27")
    ap.add_argument("--mem", action="append", default=[])
    ap.add_argument("--until", action="append", default=[],
                    help="keep continuing until reg==value holds, e.g. --until x19=0x1")
    ap.add_argument("--hits", type=int, default=1, help="how many matching stops to report")
    args = ap.parse_args()

    at = int(args.at, 0)
    rsp = Rsp(args.port)
    print("features:", rsp.cmd("qSupported")[:80])
    print("stop reason:", rsp.cmd("?"))

    conds = []
    for spec in args.until:
        name, _, value = spec.partition("=")
        conds.append((name.strip(), int(value, 0)))
    rsp.set_break(at)
    seen = 0
    while True:
        stop = rsp.cont()
        if stop.startswith("W") or stop.startswith("X"):
            print("process ended before the condition matched:", stop)
            return 2
        regs = rsp.regs()
        if all(regs.get(n) == v for n, v in conds):
            seen += 1
            if seen >= args.hits:
                break
    print("stopped at 0x%x after matching %s (%d match(es))"
          % (regs["pc"], args.until or "no condition", seen))
    print("pc = 0x%x" % regs["pc"])
    for name in args.regs.split(","):
        name = name.strip()
        if name in regs:
            print("%-4s = 0x%016x" % (name, regs[name]))
    for spec in args.mem:
        addr, _, length = spec.partition(",")
        addr = int(addr, 0)
        length = int(length or "16", 0)
        try:
            data = rsp.mem(addr, length)
            print("mem@0x%x: %s" % (addr, data.hex(" ")))
        except ValueError as exc:
            print("mem@0x%x: %s" % (addr, exc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
