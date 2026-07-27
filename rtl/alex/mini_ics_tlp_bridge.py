#!/usr/bin/env python3
"""Transaction-level Mini-ICS v2 bridge used by cocotb Alex endpoints.

The bridge deliberately stops at the BAR/TLP boundary: QEMU owns PCI
configuration and MSI-X tables, while an Alex endpoint supplies MMIO and DMA
transactions through the callbacks below.
"""
from __future__ import annotations

import asyncio
import struct
from dataclasses import dataclass
from typing import Awaitable, Callable, Optional

MAGIC = 0x5343494D
VERSION = 2
HEADER_SIZE = 64

HELLO, HELLO_ACK = 1, 2
MMIO_READ_REQ, MMIO_READ_RSP = 3, 4
MMIO_WRITE_REQ, MMIO_WRITE_RSP = 5, 6
DMA_READ_REQ, DMA_READ_RSP = 7, 8
DMA_WRITE_REQ, DMA_WRITE_RSP = 9, 10
MSIX, RESET_REQ, RESET_RSP, ERROR, SHUTDOWN = 11, 12, 13, 14, 15
RESPONSE = 1
SUCCESS = 0
DISCONNECTED = 9


@dataclass
class Header:
    msg_type: int
    flags: int = 0
    payload_len: int = 0
    txn_id: int = 0
    sim_timestamp: int = 0
    address: int = 0
    byte_enable: int = 0
    status: int = SUCCESS
    transfer_len: int = 0

    def pack(self) -> bytes:
        length = self.transfer_len or self.payload_len
        return struct.pack("<IHHHHIQQQQIIII", MAGIC, VERSION, self.msg_type,
                           self.flags, HEADER_SIZE, self.payload_len,
                           self.txn_id, self.sim_timestamp, self.address,
                           self.byte_enable, self.status, 0, length, 0)

    @classmethod
    def unpack(cls, raw: bytes) -> "Header":
        if len(raw) != HEADER_SIZE:
            raise ValueError("short Mini-ICS header")
        magic, version, typ, flags, hlen, plen, txn, ts, addr, be, status, _r, xfer, _r2 = struct.unpack(
            "<IHHHHIQQQQIIII", raw)
        if magic != MAGIC or version != VERSION or hlen != HEADER_SIZE:
            raise ValueError("invalid Mini-ICS v2 header")
        if plen > 16 * 1024 * 1024:
            raise ValueError("payload too large")
        return cls(typ, flags, plen, txn, ts, addr, be, status, xfer)


async def recv_message(reader: asyncio.StreamReader) -> tuple[Header, bytes]:
    header = Header.unpack(await reader.readexactly(HEADER_SIZE))
    return header, await reader.readexactly(header.payload_len)


async def send_message(writer: asyncio.StreamWriter, header: Header, payload: bytes = b"") -> None:
    if len(payload) != header.payload_len:
        raise ValueError("payload length/header mismatch")
    writer.write(header.pack() + payload)
    await writer.drain()


class MiniIcsTlpBridge:
    """Async socket bridge for a cocotb TLP model.

    ``mmio_read``/``mmio_write`` are called for QEMU-originated BAR traffic;
    ``dma_read``/``dma_write`` service endpoint-originated PCIe memory TLPs.
    """

    def __init__(self, socket_path: str,
                 mmio_read: Callable[[int, int, int], Awaitable[bytes]],
                 mmio_write: Callable[[int, bytes, int], Awaitable[int]],
                 dma_read: Callable[[int, int], Awaitable[bytes]],
                 dma_write: Callable[[int, bytes], Awaitable[int]],
                 msix: Optional[Callable[[int], Awaitable[None]]] = None):
        self.socket_path = socket_path
        self.mmio_read_cb, self.mmio_write_cb = mmio_read, mmio_write
        self.dma_read_cb, self.dma_write_cb = dma_read, dma_write
        self.msix_cb = msix
        self._pending = {}
        self._next_txn = 1
        self.reader: Optional[asyncio.StreamReader] = None
        self.writer: Optional[asyncio.StreamWriter] = None
        self._txn = 1

    async def connect(self) -> None:
        self.reader, self.writer = await asyncio.open_unix_connection(self.socket_path)
        await send_message(self.writer, Header(HELLO, payload_len=0, transfer_len=0))
        h, _ = await recv_message(self.reader)
        if h.msg_type != HELLO_ACK or h.status != SUCCESS:
            raise RuntimeError("Mini-ICS HELLO rejected")

    async def listen(self) -> None:
        """Listen for QEMU's client connection and serve one endpoint.

        QEMU's ``pcie-vip`` deliberately owns the connection attempt and
        fails realization if the backend is absent.  This is the entry point
        used by cocotb/Alex runners; ``connect`` remains available for tools
        that act as a QEMU-side client.
        """
        try:
            import os
            os.unlink(self.socket_path)
        except FileNotFoundError:
            pass
        connected = asyncio.Event()

        async def accept(reader, writer):
            if connected.is_set():
                writer.close()
                await writer.wait_closed()
                return
            self.reader, self.writer = reader, writer
            connected.set()

        server = await asyncio.start_unix_server(accept, path=self.socket_path)
        print(f"Mini-ICS Alex bridge listening on {self.socket_path}", flush=True)
        async with server:
            await connected.wait()
            print("Mini-ICS client connected", flush=True)
            h, _ = await recv_message(self.reader)
            if h.msg_type != HELLO:
                raise RuntimeError("Mini-ICS client did not send HELLO")
            await send_message(self.writer, Header(HELLO_ACK, RESPONSE,
                                                   txn_id=h.txn_id))
            print("Mini-ICS HELLO acknowledged", flush=True)
            await self.serve()

    async def serve(self) -> None:
        if self.reader is None or self.writer is None:
            await self.connect()
        while True:
            h, payload = await recv_message(self.reader)
            if h.flags & RESPONSE:
                waiter = self._pending.pop(h.txn_id, None)
                if waiter is not None and not waiter.done():
                    waiter.set_result((h, payload))
                continue
            if h.msg_type == MMIO_READ_REQ:
                data = await self.mmio_read_cb(h.address, h.transfer_len, h.byte_enable)
                await send_message(self.writer, Header(MMIO_READ_RSP, RESPONSE, len(data), h.txn_id,
                                                       address=h.address, status=SUCCESS,
                                                       transfer_len=len(data)), data)
            elif h.msg_type == MMIO_WRITE_REQ:
                status = await self.mmio_write_cb(h.address, payload, h.byte_enable)
                await send_message(self.writer, Header(MMIO_WRITE_RSP, RESPONSE, 0, h.txn_id,
                                                       address=h.address, status=status,
                                                       transfer_len=h.transfer_len))
            elif h.msg_type == DMA_READ_REQ:
                data = await self.dma_read_cb(h.address, h.transfer_len)
                await send_message(self.writer, Header(DMA_READ_RSP, RESPONSE, len(data), h.txn_id,
                                                       address=h.address, status=SUCCESS,
                                                       transfer_len=len(data)), data)
            elif h.msg_type == DMA_WRITE_REQ:
                status = await self.dma_write_cb(h.address, payload)
                await send_message(self.writer, Header(DMA_WRITE_RSP, RESPONSE, 0, h.txn_id,
                                                       address=h.address, status=status,
                                                       transfer_len=h.transfer_len))
            elif h.msg_type == MSIX:
                if self.msix_cb:
                    await self.msix_cb(h.address & 0xffff)
            elif h.msg_type == RESET_REQ:
                await send_message(self.writer, Header(RESET_RSP, RESPONSE,
                                                       txn_id=h.txn_id,
                                                       status=SUCCESS))
            elif h.msg_type == SHUTDOWN:
                return

    async def request(self, msg_type: int, address: int, payload: bytes = b"",
                      transfer_len: int = 0):
        """Send a backend-initiated request to QEMU and await its response."""
        if self.writer is None:
            raise RuntimeError("Mini-ICS bridge is not connected")
        txn = self._next_txn
        self._next_txn += 1
        loop = asyncio.get_running_loop()
        waiter = loop.create_future()
        self._pending[txn] = waiter
        await send_message(self.writer, Header(msg_type, payload_len=len(payload),
                                               txn_id=txn, address=address,
                                               transfer_len=transfer_len or len(payload)), payload)
        return await asyncio.wait_for(waiter, timeout=5.0)
