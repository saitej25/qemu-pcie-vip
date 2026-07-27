"""Generic one-segment PCIe Memory TLP encoder/decoder for Alex RTL.

The bit layout follows verilog-pcie's 128-bit request header: fields are
returned as an integer whose bit zero is the least-significant TLP header bit.
"""
from dataclasses import dataclass

MEM_TYPE = 0b00000
FMT_3DW = 0b000
FMT_3DW_DATA = 0b010


def _set(value, high, low, field):
    mask = (1 << (high - low + 1)) - 1
    return value | ((field & mask) << low)


@dataclass(frozen=True)
class MemoryTlp:
    header: int
    data: int = 0

    @property
    def is_write(self):
        return ((self.header >> 125) & 0x7) == FMT_3DW_DATA

    @property
    def address(self):
        if ((self.header >> 125) & 0x1):
            return self.header & ~0x3
        return (((self.header >> 34) & ((1 << 30) - 1)) << 2)

    @property
    def first_be(self):
        return (self.header >> 64) & 0xf

    @property
    def requester_id(self):
        return (self.header >> 80) & 0xffff


def memory_read(address: int, requester_id: int = 0, tag: int = 0,
                byte_enable: int = 0xf) -> MemoryTlp:
    h = 0
    h = _set(h, 127, 125, FMT_3DW)
    h = _set(h, 124, 120, MEM_TYPE)
    h = _set(h, 105, 96, 1)
    h = _set(h, 95, 80, requester_id)
    h = _set(h, 79, 72, tag)
    h = _set(h, 71, 68, byte_enable)
    h = _set(h, 67, 64, byte_enable)
    # 3-DW Memory TLPs carry a 32-bit address in DW2 bits 31:2,
    # corresponding to header bits 63:34 in the 128-bit representation.
    h = _set(h, 63, 34, address >> 2)
    return MemoryTlp(h)


def memory_write(address: int, data: int, requester_id: int = 0,
                 tag: int = 0, byte_enable: int = 0xf) -> MemoryTlp:
    h = memory_read(address, requester_id, tag, byte_enable).header
    h &= ~(0x7 << 125)
    h = _set(h, 127, 125, FMT_3DW_DATA)
    return MemoryTlp(h, data & 0xffffffff)
