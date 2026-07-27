#include "mini_ics/server.hpp"

#include <cstdio>

namespace mini_ics {

MiniIcsServer::MiniIcsServer(std::string socket_path, int io_timeout_ms, int request_timeout_ms)
    : socket_path_(std::move(socket_path)),
      io_timeout_ms_(io_timeout_ms),
      request_timeout_ms_(request_timeout_ms) {}

MiniIcsServer::~MiniIcsServer() { Stop(); }

bool MiniIcsServer::Start() {
    if (!listener_.Listen(socket_path_, /*backlog=*/1)) {
        MINI_ICS_LOG_NOTXN(LogLevel::kError, "SERV", 0,
                            "failed to listen on " + socket_path_);
        return false;
    }
    MINI_ICS_LOG_NOTXN(LogLevel::kInfo, "SERV", 0, "listening on " + socket_path_);

    running_.store(true, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);
    io_thread_ = std::thread(&MiniIcsServer::IoThreadMain, this);
    return true;
}

void MiniIcsServer::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        // Already stopped (or never started); still make sure a thread
        // object that was joined once isn't joined again.
        if (io_thread_.joinable()) io_thread_.join();
        return;
    }
    stop_requested_.store(true, std::memory_order_release);
    inbound_queue_.Shutdown();
    pending_.CancelAll();
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    listener_.Close();
    client_socket_.Close();
    MINI_ICS_LOG_NOTXN(LogLevel::kInfo, "SERV", 0, "stopped");
}

bool MiniIcsServer::HandleHello(const Message& msg) {
    if (msg.header.Type() != MessageType::kHello) {
        MINI_ICS_LOG_NOTXN(LogLevel::kError, "SERV", 0,
                            "expected HELLO as first message, got " + ToString(msg.header.Type()));
        return false;
    }
    std::vector<std::uint8_t> empty;
    Header ack = MakeHeader(MessageType::kHelloAck, msg.header.txn_id, 0, 0, StatusCode::kSuccess,
                             msg.header.sim_timestamp, 0, kFlagIsResponse);
    auto bytes = SerializeMessage(ack, empty);
    std::lock_guard<std::mutex> lock(send_mutex_);
    return client_socket_.SendAll(bytes, io_timeout_ms_) == IoStatus::kOk;
}

void MiniIcsServer::DispatchIncoming(Message msg) {
    // Responses to RTL-initiated requests (DMA_READ_RSP, DMA_WRITE_RSP)
    // are routed to whichever thread is blocked in SendRequestToHost();
    // everything else is a host-initiated request/notification that the
    // RTL/DPI side consumes via PollInbound().
    MessageType type = msg.header.Type();
    if (type == MessageType::kDmaReadRsp || type == MessageType::kDmaWriteRsp) {
        if (!pending_.Complete(msg.header.txn_id, msg)) {
            MINI_ICS_LOG(LogLevel::kWarn, "SERV", msg.header.sim_timestamp,
                         static_cast<std::int64_t>(msg.header.txn_id),
                         "response for unknown/expired transaction: " + ToString(type));
        }
        return;
    }
    inbound_queue_.Push(std::move(msg));
}

void MiniIcsServer::IoThreadMain() {
    while (running_.load(std::memory_order_acquire) && !stop_requested_.load(std::memory_order_acquire)) {
        if (!has_client_.load(std::memory_order_acquire)) {
            UnixSocket accepted;
            IoStatus st = listener_.Accept(accepted, io_timeout_ms_);
            if (st == IoStatus::kTimeout) {
                continue;
            }
            if (st != IoStatus::kOk) {
                MINI_ICS_LOG_NOTXN(LogLevel::kError, "SERV", 0, "accept() failed: " + ToString(st));
                continue;
            }
            client_socket_ = std::move(accepted);

            // Handshake: first message off the wire must be HELLO.
            std::vector<std::uint8_t> hdr_buf(kHeaderSize);
            IoStatus hst = client_socket_.RecvAll(hdr_buf, io_timeout_ms_);
            if (hst != IoStatus::kOk) {
                MINI_ICS_LOG_NOTXN(LogLevel::kError, "SERV", 0,
                                    "handshake recv failed: " + ToString(hst));
                client_socket_.Close();
                continue;
            }
            CodecError cerr = CodecError::kNone;
            auto hdr = DeserializeHeader(hdr_buf.data(), hdr_buf.size(), &cerr);
            if (!hdr) {
                MINI_ICS_LOG_NOTXN(LogLevel::kError, "SERV", 0,
                                    "handshake bad header: " + ToString(cerr));
                client_socket_.Close();
                continue;
            }
            Message hello_msg;
            hello_msg.header = *hdr;
            if (hdr->payload_len > 0) {
                hello_msg.payload.resize(hdr->payload_len);
                if (client_socket_.RecvAll(hello_msg.payload, io_timeout_ms_) != IoStatus::kOk) {
                    client_socket_.Close();
                    continue;
                }
            }
            if (!HandleHello(hello_msg)) {
                client_socket_.Close();
                continue;
            }
            has_client_.store(true, std::memory_order_release);
            MINI_ICS_LOG_NOTXN(LogLevel::kInfo, "SERV", 0, "client handshake complete");
            continue;
        }

        // Normal operation: wait for the next full message from the
        // connected client and dispatch it.
        IoStatus wait = client_socket_.WaitReadable(io_timeout_ms_);
        if (wait == IoStatus::kTimeout) {
            continue;
        }
        if (wait != IoStatus::kOk) {
            MINI_ICS_LOG_NOTXN(LogLevel::kWarn, "SERV", 0,
                                "client disconnected: " + ToString(wait));
            client_socket_.Close();
            has_client_.store(false, std::memory_order_release);
            continue;
        }

        std::vector<std::uint8_t> hdr_buf(kHeaderSize);
        IoStatus st = client_socket_.RecvAll(hdr_buf, io_timeout_ms_);
        if (st != IoStatus::kOk) {
            MINI_ICS_LOG_NOTXN(LogLevel::kWarn, "SERV", 0,
                                "client disconnected: " + ToString(st));
            client_socket_.Close();
            has_client_.store(false, std::memory_order_release);
            continue;
        }

        CodecError cerr = CodecError::kNone;
        auto hdr = DeserializeHeader(hdr_buf.data(), hdr_buf.size(), &cerr);
        if (!hdr) {
            MINI_ICS_LOG_NOTXN(LogLevel::kError, "SERV", 0, "malformed header: " + ToString(cerr));
            // A malformed header desynchronizes the stream (we don't
            // know how many payload bytes to skip), so the safest
            // recovery is to drop the connection rather than guess.
            client_socket_.Close();
            has_client_.store(false, std::memory_order_release);
            continue;
        }

        Message msg;
        msg.header = *hdr;
        if (hdr->payload_len > 0) {
            msg.payload.resize(hdr->payload_len);
            st = client_socket_.RecvAll(msg.payload, io_timeout_ms_);
            if (st != IoStatus::kOk) {
                MINI_ICS_LOG_NOTXN(LogLevel::kWarn, "SERV", 0,
                                    "client disconnected mid-payload: " + ToString(st));
                client_socket_.Close();
                has_client_.store(false, std::memory_order_release);
                continue;
            }
        }

        if (msg.header.Type() == MessageType::kShutdown) {
            MINI_ICS_LOG(LogLevel::kInfo, "SERV", msg.header.sim_timestamp,
                         static_cast<std::int64_t>(msg.header.txn_id), "SHUTDOWN received");
            inbound_queue_.Push(msg);
            client_socket_.Close();
            has_client_.store(false, std::memory_order_release);
            continue;
        }

        DispatchIncoming(std::move(msg));
    }
}

std::optional<Message> MiniIcsServer::PollInbound(int timeout_ms) {
    return inbound_queue_.Pop(timeout_ms);
}

bool MiniIcsServer::SendResponse(const Header& header, const std::vector<std::uint8_t>& payload) {
    if (!has_client_.load(std::memory_order_acquire)) {
        return false;
    }
    auto bytes = SerializeMessage(header, payload);
    std::lock_guard<std::mutex> lock(send_mutex_);
    return client_socket_.SendAll(bytes, io_timeout_ms_) == IoStatus::kOk;
}

std::optional<Message> MiniIcsServer::SendRequestToHost(MessageType type, std::uint64_t address,
                                                         std::uint64_t byte_enable,
                                                         const std::vector<std::uint8_t>& payload,
                                                         std::uint32_t transfer_len,
                                                         std::uint64_t sim_timestamp,
                                                         bool expect_response,
                                                         std::uint64_t* out_txn_id) {
    if (!has_client_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }

    std::uint64_t txn_id = txn_gen_.Next();
    if (out_txn_id) *out_txn_id = txn_id;

    if (expect_response) {
        pending_.RegisterPending(txn_id);
    }

    Header hdr = MakeHeader(type, txn_id, address, byte_enable, StatusCode::kSuccess, sim_timestamp,
                             static_cast<std::uint32_t>(payload.size()), kFlagNone, transfer_len);
    auto bytes = SerializeMessage(hdr, payload);

    IoStatus st;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        st = client_socket_.SendAll(bytes, io_timeout_ms_);
    }
    if (st != IoStatus::kOk) {
        MINI_ICS_LOG(LogLevel::kError, "SERV", sim_timestamp, static_cast<std::int64_t>(txn_id),
                     ToString(type) + " send to host failed: " + ToString(st));
        if (expect_response) pending_.WaitFor(txn_id, 0);  // drop the pending entry we registered
        return std::nullopt;
    }

    if (!expect_response) {
        return std::nullopt;
    }
    auto rsp = pending_.WaitFor(txn_id, request_timeout_ms_);
    if (!rsp) {
        MINI_ICS_LOG(LogLevel::kError, "SERV", sim_timestamp, static_cast<std::int64_t>(txn_id),
                     ToString(type) + " timed out waiting for host response");
    }
    return rsp;
}

}  // namespace mini_ics

#ifdef MINI_ICS_BUILD_SERVER_MAIN
#include <csignal>
#include <cstring>

namespace {
std::atomic<bool> g_stop{false};
void HandleSigint(int) { g_stop.store(true); }
}  // namespace

namespace mini_ics {

// Software stand-in for rtl/mini_ics_endpoint_model.sv +
// nvme_register_model.sv, used only by `--demo` mode so the pure C++
// path (server + host_client, no simulator) can be exercised and
// automated-tested without QuestaSim/Verilator installed. It implements
// the same register semantics the SV model implements, driven by the
// same wire protocol, so it is a faithful (if not cycle-accurate) stand
// in for CI and quick regression runs.
class SoftwareEndpointModel {
public:
    explicit SoftwareEndpointModel(MiniIcsServer& server) : server_(server) {}

    // Runs until SHUTDOWN is observed or max_iterations of "no inbound
    // work" polls elapse (bounds the demo so a protocol bug can't hang
    // it forever).
    void Run(bool persistent = false) {
        int idle_polls = 0;
        while (persistent || idle_polls < 200) {
            auto msg = server_.PollInbound(50);
            if (!msg) {
                ++idle_polls;
                continue;
            }
            idle_polls = 0;
            sim_time_ns_ += 15;

            if (msg->header.Type() == MessageType::kShutdown) {
                MINI_ICS_LOG(LogLevel::kInfo, "RTL ", sim_time_ns_,
                             static_cast<std::int64_t>(msg->header.txn_id), "SHUTDOWN received, stopping");
                break;
            }
            Handle(*msg);
        }
    }

private:
    static constexpr std::uint64_t kCapabilities = 0x0000000000000001ULL;  // CAP: minimal, NVMe-inspired
    static constexpr std::uint64_t kAdminSqReg = 0x0028;
    static constexpr std::uint64_t kAdminCqReg = 0x0030;
    static constexpr std::uint64_t kAqaReg = 0x0024;
    static constexpr std::uint64_t kCcReg = 0x0014;
    static constexpr std::uint64_t kCstsReg = 0x001C;
    static constexpr std::uint64_t kCapReg = 0x0000;
    static constexpr std::uint64_t kSq0Doorbell = 0x1000;
    // Core NVMe-like register block is 4 KB (0x000-0xFFF); doorbells
    // live in the next 4 KB page per real NVMe BAR convention, so the
    // addressable MMIO window this model validates against is 8 KB.
    static constexpr std::uint64_t kMmioSpaceSize = 64 * 1024;

    void Handle(const Message& msg) {
        switch (msg.header.Type()) {
            case MessageType::kResetReq: return HandleReset(msg);
            case MessageType::kMmioWriteReq: return HandleMmioWrite(msg);
            case MessageType::kMmioReadReq: return HandleMmioRead(msg);
            default:
                MINI_ICS_LOG(LogLevel::kWarn, "RTL ", sim_time_ns_,
                             static_cast<std::int64_t>(msg.header.txn_id),
                             "unhandled inbound type: " + ToString(msg.header.Type()));
        }
    }

    void HandleReset(const Message& msg) {
        cc_ = 0;
        csts_ = 0;
        asq_ = 0;
        acq_ = 0;
        aqa_ = 0;
        MINI_ICS_LOG(LogLevel::kInfo, "RTL ", sim_time_ns_,
                     static_cast<std::int64_t>(msg.header.txn_id), "RESET applied");
        std::vector<std::uint8_t> empty;
        Header rsp = MakeHeader(MessageType::kResetRsp, msg.header.txn_id, 0, 0, StatusCode::kSuccess,
                                 sim_time_ns_, 0, kFlagIsResponse);
        server_.SendResponse(rsp, empty);
    }

    bool AddrValid(std::uint64_t addr, std::uint32_t len) const {
        return addr + len <= kMmioSpaceSize;
    }

    void HandleMmioWrite(const Message& msg) {
        std::uint64_t addr = msg.header.address;
        std::uint32_t len = static_cast<std::uint32_t>(msg.payload.size());
        StatusCode status = StatusCode::kSuccess;

        if (!AddrValid(addr, len)) {
            status = StatusCode::kErrorInvalidAddress;
        } else if (addr == kCcReg && len == 4) {
            cc_ = DecodeU32(msg.payload);
            if (cc_ & 0x1) {
                csts_ |= 0x1;  // controller becomes ready once EN is set
                MINI_ICS_LOG(LogLevel::kInfo, "RTL ", sim_time_ns_,
                             static_cast<std::int64_t>(msg.header.txn_id),
                             "controller enabled, CSTS.RDY=1");
            } else {
                csts_ &= ~0x1u;
            }
        } else if (addr == kAsqRegLow() && len == 8) {
            asq_ = DecodeU64(msg.payload);
        } else if (addr == kAcqRegLow() && len == 8) {
            acq_ = DecodeU64(msg.payload);
        } else if (addr == kAqaReg && len == 4) {
            aqa_ = DecodeU32(msg.payload);
        } else if (addr == kSq0Doorbell && len == 4) {
            std::uint32_t val = DecodeU32(msg.payload);
            MINI_ICS_LOG(LogLevel::kInfo, "RTL ", sim_time_ns_,
                         static_cast<std::int64_t>(msg.header.txn_id),
                         "SQ0 doorbell rung, tail=" + std::to_string(val));
            doorbell_rung_ = true;
        } else if (len != 4 && len != 8) {
            status = StatusCode::kErrorBadLength;
        } else {
            status = StatusCode::kErrorInvalidAddress;
        }

        std::vector<std::uint8_t> empty;
        Header rsp = MakeHeader(MessageType::kMmioWriteRsp, msg.header.txn_id, addr, 0, status,
                                 sim_time_ns_, 0, kFlagIsResponse);
        server_.SendResponse(rsp, empty);
        MINI_ICS_LOG(LogLevel::kInfo, "RTL ", sim_time_ns_,
                     static_cast<std::int64_t>(msg.header.txn_id),
                     "MMIO_WRITE_RSP status=" + ToString(status));

        if (doorbell_rung_) {
            doorbell_rung_ = false;
            RunDmaSequence();
        }
    }

    void HandleMmioRead(const Message& msg) {
        std::uint64_t addr = msg.header.address;
        std::uint32_t len = msg.header.transfer_len;
        if (len == 0) {
            len = static_cast<std::uint32_t>(__builtin_popcountll(msg.header.byte_enable));
        }
        StatusCode status = StatusCode::kSuccess;
        std::uint32_t value = 0;

        if (len != 1 && len != 2 && len != 4 && len != 8) {
            status = StatusCode::kErrorBadLength;
            len = 0;
        }

        if (!AddrValid(addr, len)) {
            status = StatusCode::kErrorInvalidAddress;
        } else if (addr == kCstsReg) {
            value = csts_;
        } else if (addr == kCcReg) {
            value = cc_;
        } else if (addr == kAqaReg) {
            value = aqa_;
        } else if (addr == kCapReg) {
            value = static_cast<std::uint32_t>(kCapabilities);
        } else {
            status = StatusCode::kErrorInvalidAddress;
        }

        std::vector<std::uint8_t> payload;
        if (status == StatusCode::kSuccess) {
            payload.resize(len, 0);
            const std::uint64_t wide_value = value;
            for (std::uint32_t i = 0; i < len; ++i) {
                payload[i] = static_cast<std::uint8_t>(wide_value >> (8 * i));
            }
        }
        Header rsp = MakeHeader(MessageType::kMmioReadRsp, msg.header.txn_id, addr, 0, status,
                                 sim_time_ns_, static_cast<std::uint32_t>(payload.size()),
                                 kFlagIsResponse);
        server_.SendResponse(rsp, payload);
        MINI_ICS_LOG(LogLevel::kInfo, "RTL ", sim_time_ns_,
                     static_cast<std::int64_t>(msg.header.txn_id),
                     "MMIO_READ_RSP status=" + ToString(status));
    }

    // Drives the RTL-initiated sequence once the doorbell is rung:
    // DMA read the 64-byte command, then DMA write a 16-byte mock
    // completion, then raise MSI-X.
    void RunDmaSequence() {
        sim_time_ns_ += 60;
        std::uint64_t read_txn_id = 0;
        auto read_rsp = server_.SendRequestToHost(MessageType::kDmaReadReq, asq_,
                                                   0, {}, 64, sim_time_ns_, /*expect_response=*/true,
                                                   &read_txn_id);
        MINI_ICS_LOG(LogLevel::kInfo, "RTL ", sim_time_ns_, static_cast<std::int64_t>(read_txn_id),
                     "DMA_READ addr=" + std::to_string(asq_) + " len=64");
        if (!read_rsp || read_rsp->header.Status() != StatusCode::kSuccess) {
            MINI_ICS_LOG(LogLevel::kError, "RTL ", sim_time_ns_,
                         static_cast<std::int64_t>(read_txn_id), "DMA read failed");
            return;
        }
        MINI_ICS_LOG(LogLevel::kInfo, "RTL ", sim_time_ns_, static_cast<std::int64_t>(read_txn_id),
                     "DMA_READ_RSP len=" + std::to_string(read_rsp->payload.size()));

        // Build the 16-byte mock completion. Value chosen to match what
        // host_client.cpp verifies (0xC0 + i).
        std::vector<std::uint8_t> completion(16);
        for (std::size_t i = 0; i < completion.size(); ++i) {
            completion[i] = static_cast<std::uint8_t>(0xC0 + i);
        }

        sim_time_ns_ += 45;
        std::uint64_t write_txn_id = 0;
        auto write_rsp = server_.SendRequestToHost(MessageType::kDmaWriteReq, acq_,
                                                    0, completion,
                                                    static_cast<std::uint32_t>(completion.size()),
                                                    sim_time_ns_,
                                                    /*expect_response=*/true, &write_txn_id);
        MINI_ICS_LOG(LogLevel::kInfo, "RTL ", sim_time_ns_, static_cast<std::int64_t>(write_txn_id),
                     "DMA_WRITE addr=" + std::to_string(acq_) + " len=16");
        if (!write_rsp || write_rsp->header.Status() != StatusCode::kSuccess) {
            MINI_ICS_LOG(LogLevel::kError, "RTL ", sim_time_ns_,
                         static_cast<std::int64_t>(write_txn_id), "DMA write failed");
            return;
        }

        sim_time_ns_ += 30;
        std::uint64_t msix_txn_id = 0;
        constexpr std::uint64_t kMsixVector = 0;
        server_.SendRequestToHost(MessageType::kMsiX, kMsixVector, 0, {}, 0, sim_time_ns_,
                                   /*expect_response=*/false, &msix_txn_id);
        MINI_ICS_LOG(LogLevel::kInfo, "RTL ", sim_time_ns_, static_cast<std::int64_t>(msix_txn_id),
                     "MSI_X vector=" + std::to_string(kMsixVector));
    }

    static std::uint64_t kAsqRegLow() { return 0x0028; }
    static std::uint64_t kAcqRegLow() { return 0x0030; }

    static std::uint32_t DecodeU32(const std::vector<std::uint8_t>& b) {
        return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
               (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
    }
    static std::uint64_t DecodeU64(const std::vector<std::uint8_t>& b) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b[i]) << (8 * i);
        return v;
    }
    static std::vector<std::uint8_t> EncodeU32(std::uint32_t v) {
        return {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
    }

    MiniIcsServer& server_;
    std::uint64_t sim_time_ns_ = 100;
    std::uint32_t cc_ = 0;
    std::uint32_t csts_ = 0;
    std::uint64_t asq_ = 0;
    std::uint64_t acq_ = 0;
    std::uint32_t aqa_ = 0;
    bool doorbell_rung_ = false;
};

}  // namespace mini_ics

int main(int argc, char** argv) {
    std::string socket_path = "/tmp/mini_ics.sock";
    bool demo_mode = false;
    bool persistent_demo = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (arg == "--demo") {
            demo_mode = true;
        } else if (arg == "--persistent") {
            persistent_demo = true;
        } else if (arg == "--log-level" && i + 1 < argc) {
            mini_ics::Logger::Instance().SetLevel(mini_ics::LogLevelFromString(argv[++i]));
        }
    }

    std::signal(SIGINT, HandleSigint);
    std::signal(SIGTERM, HandleSigint);

    mini_ics::MiniIcsServer server(socket_path);
    if (!server.Start()) {
        return 1;
    }

    if (demo_mode) {
        // `--demo` attaches a software endpoint model (a stand-in for
        // the SystemVerilog testbench) so the full transaction sequence
        // can run without a HDL simulator. This is what
        // scripts/run_cpp_demo.sh uses.
        mini_ics::SoftwareEndpointModel model(server);
        model.Run(persistent_demo);
        server.Stop();
        return 0;
    }

    while (!g_stop.load() && server.IsRunning()) {
        auto msg = server.PollInbound(200);
        if (!msg) continue;
        if (msg->header.Type() == mini_ics::MessageType::kShutdown) {
            break;
        }
        // Standalone `mini_ics_server` (no --demo) has no RTL side
        // attached; it just logs what it received. Real handling
        // happens in the DPI shim or the --demo software model above.
    }

    server.Stop();
    return 0;
}
#endif
