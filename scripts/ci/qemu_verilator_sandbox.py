#!/usr/bin/env python3
"""Run the patched QEMU against the live Verilator Alex adapter.

This is a host-only sandbox test: it checks adapter startup, Mini-ICS
handshake, QEMU realization, and PCI enumeration.  A guest image is not
needed; guest BAR/DMA tests remain the separate Linux acceptance test.
"""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import time
from pathlib import Path


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(f"usage: {sys.argv[0]} QEMU ADAPTER [SOCKET]", file=sys.stderr)
        return 2

    qemu = str(Path(sys.argv[1]).resolve())
    adapter = str(Path(sys.argv[2]).resolve())
    socket_path = sys.argv[3] if len(sys.argv) == 4 else "/tmp/pcie-vip-verilator-sandbox.sock"
    smoke = Path(__file__).with_name("qemu_socket_smoke.py")
    Path(socket_path).unlink(missing_ok=True)

    adapter_log = open("/tmp/pcie-vip-verilator-sandbox.log", "w", encoding="utf-8")
    proc = subprocess.Popen([adapter, socket_path], stdout=adapter_log,
                            stderr=subprocess.STDOUT, start_new_session=True)
    try:
        deadline = time.monotonic() + 15
        while not os.path.exists(socket_path):
            if proc.poll() is not None:
                raise RuntimeError("Verilator adapter exited before opening its socket")
            if time.monotonic() >= deadline:
                raise RuntimeError("timed out waiting for Verilator adapter socket")
            time.sleep(0.1)

        subprocess.run([sys.executable, str(smoke), qemu, socket_path], check=True)
        print("QEMU + Verilator Alex sandbox PASS")
        return 0
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
        adapter_log.close()
        Path(socket_path).unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
