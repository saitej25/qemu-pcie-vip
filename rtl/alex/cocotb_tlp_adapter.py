"""Cocotb transaction adapter for the Alex PCIe endpoint wrapper.

This module keeps Mini-ICS framing independent from simulator details. A DUT
driver supplies the three signal-level coroutines (AXI-Lite, DMA, interrupt)
and this class exposes the callbacks expected by ``MiniIcsTlpBridge``.
"""
from __future__ import annotations

from typing import Awaitable, Callable


class AlexTlpAdapter:
    def __init__(self,
                 axil_read: Callable[[int, int], Awaitable[bytes]],
                 axil_write: Callable[[int, bytes, int], Awaitable[int]],
                 dma_read: Callable[[int, int], Awaitable[bytes]],
                 dma_write: Callable[[int, bytes], Awaitable[int]],
                 raise_msix: Callable[[int], Awaitable[None]]):
        self.axil_read = axil_read
        self.axil_write = axil_write
        self.dma_read = dma_read
        self.dma_write = dma_write
        self.raise_msix = raise_msix

    async def mmio_read(self, address: int, length: int, byte_enable: int) -> bytes:
        return await self.axil_read(address, length)

    async def mmio_write(self, address: int, payload: bytes, byte_enable: int) -> int:
        return await self.axil_write(address, payload, byte_enable)

    async def endpoint_dma_read(self, address: int, length: int) -> bytes:
        return await self.dma_read(address, length)

    async def endpoint_dma_write(self, address: int, payload: bytes) -> int:
        return await self.dma_write(address, payload)

    async def endpoint_msix(self, vector: int) -> None:
        await self.raise_msix(vector)

