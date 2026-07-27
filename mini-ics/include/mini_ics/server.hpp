// In-process Mini ICS transaction server. This class is deliberately
// reusable from two different `main()`s: the standalone
// mini_ics_server executable (for the pure-C++ demo / integration
// tests) and the DPI-C shim (dpi/mini_ics_dpi.cpp), which links this
// same server logic directly into the SystemVerilog simulator process
// instead of running it as a separate OS process.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "mini_ics/logger.hpp"
#include "mini_ics/protocol.hpp"
#include "mini_ics/socket.hpp"
#include "mini_ics/transaction.hpp"

namespace mini_ics {

// Messages the DPI/RTL side needs to react to: anything the host client
// sent that isn't itself a response to an RTL-initiated request.
struct InboundRequest {
    Message message;
};

// A server accepts exactly one client (MVP scope), performs the HELLO
// handshake, then shuttles messages between the socket and two
// queues:
//   - inbound_: host-initiated requests (MMIO reads/writes, RESET,
//     SHUTDOWN) for the RTL/DPI side to consume via Poll().
//   - Responses to RTL-initiated requests (DMA_READ_RSP/DMA_WRITE_RSP)
//     are correlated through `pending_` and delivered synchronously to
//     whichever thread is waiting in SendRequestToHost().
class MiniIcsServer {
public:
    explicit MiniIcsServer(std::string socket_path, int io_timeout_ms = 1000,
                            int request_timeout_ms = 5000);
    ~MiniIcsServer();

    MiniIcsServer(const MiniIcsServer&) = delete;
    MiniIcsServer& operator=(const MiniIcsServer&) = delete;

    // Starts the listener and the socket I/O thread. Returns false if
    // the listen() call itself fails (e.g. bad path/permissions).
    bool Start();

    // Signals the I/O thread to stop and joins it. Safe to call more
    // than once and safe to call from a signal handler's requested
    // shutdown flag (it does not itself run in signal-handler context,
    // but is intended to be triggered promptly by a flag it polls).
    void Stop();

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }
    bool HasClient() const { return has_client_.load(std::memory_order_acquire); }

    // Non-blocking-ish poll for the DPI/RTL side: waits up to
    // timeout_ms for the next host-initiated request. Returns
    // std::nullopt on timeout (the common case every simulator clock
    // tick when nothing has arrived) so the simulator thread is never
    // stuck.
    std::optional<Message> PollInbound(int timeout_ms);

    // Sends a response (to a host-initiated request) back over the
    // socket. Used by the RTL/DPI side after processing a PollInbound()
    // result.
    bool SendResponse(const Header& header, const std::vector<std::uint8_t>& payload);

    // Sends an RTL-initiated request (DMA_READ_REQ, DMA_WRITE_REQ,
    // MSI_X) to the host client and, for request types that expect a
    // reply (DMA read/write), blocks up to request_timeout_ms for the
    // correlated response. MSI_X is fire-and-forget: pass
    // expect_response=false.
    std::optional<Message> SendRequestToHost(MessageType type, std::uint64_t address,
                                              std::uint64_t byte_enable,
                                              const std::vector<std::uint8_t>& payload,
                                              std::uint32_t transfer_len,
                                              std::uint64_t sim_timestamp,
                                              bool expect_response, std::uint64_t* out_txn_id = nullptr);

    std::uint64_t NextTxnId() { return txn_gen_.Next(); }

private:
    void IoThreadMain();
    bool HandleHello(const Message& msg);
    void DispatchIncoming(Message msg);

    std::string socket_path_;
    int io_timeout_ms_;
    int request_timeout_ms_;

    UnixListener listener_;
    UnixSocket client_socket_;
    std::mutex send_mutex_;  // serializes writes from IoThread vs. RTL-request senders

    std::thread io_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> has_client_{false};
    std::atomic<bool> stop_requested_{false};

    TransactionIdGenerator txn_gen_;
    BlockingQueue<Message> inbound_queue_;
    PendingRequestTable pending_;
};

}  // namespace mini_ics
