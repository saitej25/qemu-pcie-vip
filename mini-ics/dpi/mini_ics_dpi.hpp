// C-linkage declarations for the Mini ICS DPI-C boundary. This header is
// consumed only from mini_ics_dpi.cpp; SystemVerilog gets its view of
// these functions from the `import "DPI-C"` declarations in
// dpi/mini_ics_dpi_pkg.sv, which must stay in sync with the signatures
// below (see docs/architecture.md for the "why" of each deviation from
// the spec's suggested signatures).
#pragma once

#include "svdpi.h"

extern "C" {

// Starts (or attaches to) the Mini ICS server listening on socket_path
// and blocks briefly for a client (the host_client / QEMU shim) to
// connect. Returns 0 on success, negative on failure. Must be called
// exactly once before any other mini_ics_* call.
int mini_ics_init(const char* socket_path);

// Polls for the next host-initiated inbound message (MMIO
// read/write/reset/shutdown). Non-blocking beyond a short internal
// timeout (see docs/architecture.md) so it never stalls the simulation
// thread. Returns:
//   1  - a message was available; output args populated, payload bytes
//        (if any) staged for retrieval via mini_ics_get_payload_byte
//   0  - no message available this call (the common case)
//  -1  - the server observed the client disconnect or is not running
int mini_ics_poll(unsigned int* msg_type, unsigned long long* transaction_id,
                   unsigned long long* address, unsigned int* length, unsigned int* status);

// Retrieves one byte of the payload staged by the most recent
// mini_ics_poll() call. Returns 0 on success, -1 if index is out of
// range for the staged payload.
int mini_ics_get_payload_byte(unsigned int index, svBitVecVal* value);

// Sends a response (MMIO_READ_RSP, MMIO_WRITE_RSP, RESET_RSP, ERROR) for
// a transaction previously delivered via mini_ics_poll(). `payload` is a
// SystemVerilog open array of bytes (may be empty for writes/resets).
// Returns 0 on success, negative on failure (e.g. no connected client).
int mini_ics_send_response(unsigned int msg_type, unsigned long long transaction_id,
                            unsigned long long address, unsigned int status,
                            const svOpenArrayHandle payload, unsigned int length);

// Sends an RTL-initiated request (DMA_READ_REQ, DMA_WRITE_REQ, MSI_X) to
// the host client. Allocates and returns a fresh transaction_id via the
// output argument. For DMA_READ_REQ/DMA_WRITE_REQ this function BLOCKS
// the calling (DPI) call -- but not the simulator's event queue beyond
// that single call -- until the host's correlated response arrives or
// request_timeout elapses; see docs/architecture.md for why a bounded
// blocking call here is acceptable while mini_ics_poll must not block.
// For MSI_X there is no response to wait for; it returns immediately
// after the send.
// Returns 0 on success, negative on failure or timeout (status output
// reflects ERROR_TIMEOUT in that case for DMA requests).
int mini_ics_send_request(unsigned int msg_type, unsigned long long* transaction_id,
                           unsigned long long address, const svOpenArrayHandle payload,
                           unsigned int length, unsigned int* out_status);

// Retrieves one byte of the response payload from the most recent
// mini_ics_send_request() call (used to fetch DMA_READ_RSP data).
// Returns 0 on success, -1 if index is out of range.
int mini_ics_get_response_payload_byte(unsigned int index, svBitVecVal* value);

// Cleanly stops the server, closes the socket, and joins its thread.
// Safe to call multiple times; subsequent calls are no-ops.
void mini_ics_shutdown();

}  // extern "C"
