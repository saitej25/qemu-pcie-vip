# Mini-ICS v2 wire format

Every message starts with a 64-byte little-endian header followed by
`payload_len` bytes. Fields are fixed at these offsets:

| offset | size | field |
|---:|---:|---|
| 0 | 4 | magic `0x5343494d` |
| 4 | 2 | version `2` |
| 6 | 2 | message type |
| 8 | 2 | flags |
| 10 | 2 | header length (`64`) |
| 12 | 4 | payload length |
| 16 | 8 | transaction ID |
| 24 | 8 | simulation timestamp |
| 32 | 8 | address |
| 40 | 8 | byte enables |
| 48 | 4 | status |
| 52 | 4 | reserved |
| 56 | 4 | **transfer length** |
| 60 | 4 | reserved |

`transfer_len` is the requested MMIO or DMA transfer size and is independent
of `payload_len`. In particular, a DMA read request has an empty payload but a
non-zero transfer length. Implementations must use exact socket reads/writes,
validate both lengths, correlate responses by transaction ID, and reject a
second connection after runtime disconnect until QEMU is restarted.
