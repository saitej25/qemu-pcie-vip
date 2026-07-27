#!/usr/bin/env python3
"""Launch the QEMU-facing Mini-ICS listener for an Alex cocotb endpoint.

The callbacks are intentionally small reference models. Replace them with
the cocotb handles driving ``pcie_axil_master`` and ``dma_if_pcie`` in the
Alex endpoint wrapper; the socket ownership and v2 framing remain unchanged.
"""
import asyncio
import sys

from mini_ics_tlp_bridge import Header, MiniIcsTlpBridge, MSIX


REGS = {0x0: 1, 0x14: 0, 0x1C: 0}
ASQ = 0
ACQ = 0
BRIDGE = None


async def zero_read(address, length, byte_enable):
    value = REGS.get(address, 0)
    return bytes((value >> (8 * i)) & 0xff for i in range(length))


async def accept_write(address, payload, byte_enable):
    global ASQ, ACQ
    value = sum(byte << (8 * i) for i, byte in enumerate(payload))
    if address == 0x14:
        REGS[address] = value
        REGS[0x1C] = 1 if value & 1 else 0
    elif address in REGS:
        REGS[address] = value
    elif address == 0x28:
        ASQ = value
    elif address == 0x30:
        ACQ = value
    elif address == 0x1000 and BRIDGE is not None:
        asyncio.create_task(dma_demo())
    return 0


async def dma_demo():
    if BRIDGE is None:
        return
    read_rsp, command = await BRIDGE.request(7, ASQ, transfer_len=64)
    if read_rsp.status != 0:
        return
    completion = bytes(0xc0 + i for i in range(16))
    write_rsp, _ = await BRIDGE.request(9, ACQ, completion, transfer_len=16)
    if write_rsp.status == 0:
        BRIDGE.writer.write(Header(MSIX, address=0, transfer_len=0).pack())
        await BRIDGE.writer.drain()


async def main(path):
    global BRIDGE
    bridge = MiniIcsTlpBridge(path, zero_read, accept_write,
                              lambda address, length: asyncio.sleep(0, result=bytes(length)),
                              lambda address, payload: asyncio.sleep(0, result=0))
    BRIDGE = bridge
    await bridge.listen()


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/pcie-vip.sock"))
