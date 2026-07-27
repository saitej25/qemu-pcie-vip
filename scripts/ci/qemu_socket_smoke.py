#!/usr/bin/env python3
"""Headless QEMU + Mini-ICS socket smoke test for CI.

This intentionally checks host-side realization and PCI enumeration only.  A
guest image and Linux pcimem/driver test belong in the optional guest job.
"""

from __future__ import annotations

import json
import os
import socket
import sys
import time
from pathlib import Path
from subprocess import Popen


def read_json_line(sock: socket.socket) -> dict:
    data = bytearray()
    while b"\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("QMP closed before sending a response")
        data.extend(chunk)
    line = bytes(data).split(b"\n", 1)[0]
    return json.loads(line)


def command(sock: socket.socket, name: str, arguments: dict | None = None) -> dict:
    request = {"execute": name}
    if arguments:
        request["arguments"] = arguments
    sock.sendall(json.dumps(request).encode() + b"\n")
    while True:
        response = read_json_line(sock)
        if "return" in response or "error" in response:
            return response


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} QEMU SOCKET", file=sys.stderr)
        return 2
    qemu = Path(sys.argv[1]).resolve()
    backend = sys.argv[2]
    qmp = f"/tmp/pcie-vip-qmp-{os.getpid()}.sock"
    try:
        proc = Popen(
            [
                str(qemu),
                "-machine",
                "q35",
                "-m",
                "128M",
                "-nodefaults",
                "-display",
                "none",
                "-qmp",
                f"unix:{qmp},server=on,wait=off",
                "-device",
                f"pcie-vip,socket={backend},timeout-ms=5000",
            ],
            stdout=sys.stdout,
            stderr=sys.stderr,
        )
        deadline = time.monotonic() + 20
        qmp_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        while True:
            try:
                qmp_sock.connect(qmp)
                break
            except OSError:
                if proc.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("QEMU did not open its QMP socket")
                time.sleep(0.1)

        greeting = read_json_line(qmp_sock)
        if "QMP" not in greeting:
            raise RuntimeError(f"unexpected QMP greeting: {greeting}")
        command(qmp_sock, "qmp_capabilities")
        reply = command(qmp_sock, "query-pci")
        if "error" in reply:
            raise RuntimeError(f"query-pci failed: {reply}")
        encoded = json.dumps(reply)
        if '"vendor_id": 4660' not in encoded or '"device_id": 4585' not in encoded:
            raise RuntimeError(f"pcie-vip 1234:11e9 not found in query-pci: {reply}")
        print("QEMU PCIe VIP host smoke PASS (1234:11e9)")
        command(qmp_sock, "quit")
        proc.wait(timeout=10)
        return 0
    finally:
        try:
            qmp_sock.close()
        except UnboundLocalError:
            pass
        if "proc" in locals() and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=5)
        try:
            os.unlink(qmp)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
