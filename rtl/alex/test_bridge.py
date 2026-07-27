import asyncio
import struct

from mini_ics_tlp_bridge import (HEADER_SIZE, MAGIC, VERSION, Header,
                                 MMIO_READ_REQ, RESPONSE)


def test_v2_header_golden():
    h = Header(MMIO_READ_REQ, RESPONSE, payload_len=0x20, txn_id=7,
               address=0x1234, byte_enable=0xF, transfer_len=4)
    raw = h.pack()
    assert len(raw) == HEADER_SIZE
    assert struct.unpack_from("<I", raw)[0] == MAGIC
    assert struct.unpack_from("<H", raw, 4)[0] == VERSION
    assert struct.unpack_from("<I", raw, 56)[0] == 4
    assert Header.unpack(raw) == h


def test_partial_socket_transfer():
    async def run():
        async def handle(reader, writer):
            raw = Header(MMIO_READ_REQ, payload_len=0, transfer_len=8).pack()
            for byte in raw:
                writer.write(bytes((byte,)))
                await writer.drain()
            writer.close()

        server = await asyncio.start_server(handle, "127.0.0.1", 0)
        host, port = server.sockets[0].getsockname()[:2]
        reader, writer = await asyncio.open_connection(host, port)
        del writer
        raw = await reader.readexactly(HEADER_SIZE)
        assert Header.unpack(raw).transfer_len == 8
        server.close()
        await server.wait_closed()

    asyncio.run(run())
