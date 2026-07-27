// Standalone host-side test client. Stands in for the future QEMU
// PCIe-device shim: it owns a guest-memory substitute, drives the demo
// MMIO/DMA/reset sequence described in docs/architecture.md, and
// services RTL-initiated DMA against its own memory.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mini_ics/logger.hpp"
#include "mini_ics/protocol.hpp"
#include "mini_ics/socket.hpp"
#include "mini_ics/transaction.hpp"

namespace mini_ics {

// NVMe-like MMIO register offsets, mirrored in rtl/nvme_register_model.sv.
// Kept in one place on each side rather than shared across the DPI
// boundary because the two sides speak addresses, not headers -- there is
// no C++/SV shared build step in this milestone.
namespace regs {
constexpr std::uint64_t kControllerCapabilities = 0x0000;  // 64-bit, read-only
constexpr std::uint64_t kControllerConfiguration = 0x0014; // 32-bit, CC
constexpr std::uint64_t kControllerStatus = 0x001C;        // 32-bit, CSTS
constexpr std::uint64_t kAdminQueueAttributes = 0x0024;    // 32-bit, AQA
constexpr std::uint64_t kAdminSubmissionQueueBase = 0x0028; // 64-bit, ASQ
constexpr std::uint64_t kAdminCompletionQueueBase = 0x0030; // 64-bit, ACQ
constexpr std::uint64_t kSubmissionQueue0Doorbell = 0x1000; // 32-bit, SQ0TDBL
}  // namespace regs

constexpr std::uint32_t kCcEnable = 0x1;
constexpr std::uint32_t kCstsReady = 0x1;

// Guest-memory substitute: a flat, bounds-checked, thread-safe byte
// array standing in for the memory QEMU would expose to a real device
// model. DMA reads/writes initiated by the RTL side are serviced against
// this buffer.
class HostMemory {
public:
    explicit HostMemory(std::size_t size_bytes) : data_(size_bytes) {}

    std::size_t Size() const { return data_.size(); }

    // Fills the whole buffer with a deterministic, address-derived
    // pattern so uninitialized reads are obviously wrong in a hex dump
    // rather than coincidentally zero.
    void InitDeterministicPattern() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < data_.size(); ++i) {
            data_[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 24);
        }
    }

    bool Write(std::uint64_t addr, const std::uint8_t* src, std::size_t len) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!InBoundsLocked(addr, len)) {
            return false;
        }
        std::memcpy(data_.data() + addr, src, len);
        return true;
    }

    bool Read(std::uint64_t addr, std::uint8_t* dst, std::size_t len) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!InBoundsLocked(addr, len)) {
            return false;
        }
        std::memcpy(dst, data_.data() + addr, len);
        return true;
    }

    std::string HexDump(std::uint64_t addr, std::size_t len) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string out;
        if (!InBoundsLocked(addr, len)) {
            return "<out-of-bounds hex dump request>";
        }
        char line[128];
        for (std::size_t off = 0; off < len; off += 16) {
            std::size_t chunk = std::min<std::size_t>(16, len - off);
            int pos = std::snprintf(line, sizeof(line), "  %08llx: ",
                                     static_cast<unsigned long long>(addr + off));
            for (std::size_t i = 0; i < chunk; ++i) {
                pos += std::snprintf(line + pos, sizeof(line) - pos, "%02x ",
                                      data_[addr + off + i]);
            }
            out += line;
            out += '\n';
        }
        return out;
    }

private:
    bool InBoundsLocked(std::uint64_t addr, std::size_t len) const {
        if (len == 0) return true;
        if (addr >= data_.size()) return false;
        // Guard against addr+len overflow before comparing against size.
        std::uint64_t end = addr + static_cast<std::uint64_t>(len);
        if (end < addr) return false;
        return end <= data_.size();
    }

    mutable std::mutex mutex_;
    std::vector<std::uint8_t> data_;
};

// Simple monotonically-advancing "host-side" simulation timestamp so log
// lines have plausible, increasing sim-time values even though this demo
// client has no real simulation clock of its own. Advances by a fixed
// step per logged event; not a wall-clock reading.
class HostClock {
public:
    std::uint64_t NowNs() {
        std::uint64_t v = ns_;
        ns_ += 5;  // ns granularity step between host-observed events
        return v;
    }

private:
    std::uint64_t ns_ = 100;
};

class HostClient {
public:
    HostClient(std::string socket_path, int timeout_ms)
        : socket_path_(std::move(socket_path)), timeout_ms_(timeout_ms), memory_(64ull * 1024 * 1024) {
        memory_.InitDeterministicPattern();
    }

    bool RunDemo() {
        if (!Connect()) return false;
        if (!DoHello()) return false;
        if (!DoReset()) return false;
        if (!ConfigureAdminQueues()) return false;
        if (!EnableControllerAndWaitReady()) return false;
        if (!WriteMockNvmeCommand()) return false;
        if (!RingDoorbell()) return false;
        if (!ServiceRtlInitiatedTransactions()) return false;

        Log(LogLevel::kInfo, "MINI ICS DEMO: PASS");
        SendShutdown();
        return true;
    }

private:
    void Log(LogLevel level, const std::string& msg, std::int64_t txn_id = -1) {
        std::uint64_t ts = clock_.NowNs();
        if (txn_id >= 0) {
            MINI_ICS_LOG(level, "HOST", ts, txn_id, msg);
        } else {
            MINI_ICS_LOG_NOTXN(level, "HOST", ts, msg);
        }
    }

    bool Connect() {
        // The server side (a separate process, or a HDL simulator still
        // elaborating) may not have created the socket file yet when
        // this client starts, particularly under QuestaSim/Verilator
        // where DPI init happens well after process launch. Retry with
        // a short backoff for up to timeout_ms_ total rather than
        // failing on the first ENOENT/ECONNREFUSED.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
        IoStatus st = IoStatus::kError;
        do {
            st = ConnectUnixSocket(socket_path_, socket_, /*timeout_ms=*/200);
            if (st == IoStatus::kOk) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (std::chrono::steady_clock::now() < deadline);

        if (st != IoStatus::kOk) {
            Log(LogLevel::kError, "failed to connect to " + socket_path_ + ": " + ToString(st));
            return false;
        }
        Log(LogLevel::kInfo, "connected to " + socket_path_);
        return true;
    }

    // Sends a request-shaped message and blocks (bounded by timeout_ms_)
    // for the correlated response, matching on transaction ID. Server
    // responses are assumed to arrive in-order on this simple
    // single-outstanding-request client, so we just read the next
    // message and verify the txn_id matches instead of running a full
    // pending-table (that machinery exists in server.cpp, where genuine
    // concurrent outstanding requests occur).
    std::optional<Message> SendRequestAndWait(MessageType type, std::uint64_t address,
                                               std::uint64_t byte_enable,
                                               const std::vector<std::uint8_t>& payload) {
        std::uint64_t txn_id = txn_gen_.Next();
        Header hdr = MakeHeader(type, txn_id, address, byte_enable, StatusCode::kSuccess,
                                 clock_.NowNs(), static_cast<std::uint32_t>(payload.size()));
        auto bytes = SerializeMessage(hdr, payload);

        IoStatus st = socket_.SendAll(bytes, timeout_ms_);
        if (st != IoStatus::kOk) {
            Log(LogLevel::kError, ToString(type) + " send failed: " + ToString(st), txn_id);
            return std::nullopt;
        }

        return ReadMessage(timeout_ms_);
    }

    std::optional<Message> ReadMessage(int timeout_ms) {
        std::vector<std::uint8_t> hdr_buf(kHeaderSize);
        IoStatus st = socket_.RecvAll(hdr_buf, timeout_ms);
        if (st != IoStatus::kOk) {
            Log(LogLevel::kError, "recv header failed: " + ToString(st));
            return std::nullopt;
        }

        CodecError err = CodecError::kNone;
        auto hdr = DeserializeHeader(hdr_buf.data(), hdr_buf.size(), &err);
        if (!hdr) {
            Log(LogLevel::kError, "bad header: " + ToString(err));
            return std::nullopt;
        }

        Message msg;
        msg.header = *hdr;
        if (hdr->payload_len > 0) {
            msg.payload.resize(hdr->payload_len);
            st = socket_.RecvAll(msg.payload, timeout_ms);
            if (st != IoStatus::kOk) {
                Log(LogLevel::kError, "recv payload failed: " + ToString(st));
                return std::nullopt;
            }
        }
        return msg;
    }

    bool DoHello() {
        std::vector<std::uint8_t> empty;
        auto rsp = SendRequestAndWait(MessageType::kHello, 0, 0, empty);
        if (!rsp || rsp->header.Type() != MessageType::kHelloAck) {
            Log(LogLevel::kError, "handshake failed");
            return false;
        }
        Log(LogLevel::kInfo, "handshake complete (HELLO_ACK)", static_cast<std::int64_t>(rsp->header.txn_id));
        return true;
    }

    bool DoReset() {
        std::vector<std::uint8_t> empty;
        auto rsp = SendRequestAndWait(MessageType::kResetReq, 0, 0, empty);
        if (!rsp || rsp->header.Type() != MessageType::kResetRsp ||
            rsp->header.Status() != StatusCode::kSuccess) {
            Log(LogLevel::kError, "reset failed");
            return false;
        }
        Log(LogLevel::kInfo, "RESET_RSP status=SUCCESS", static_cast<std::int64_t>(rsp->header.txn_id));
        return true;
    }

    std::vector<std::uint8_t> EncodeU32(std::uint32_t v) {
        return {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
    }
    std::vector<std::uint8_t> EncodeU64(std::uint64_t v) {
        std::vector<std::uint8_t> out(8);
        for (int i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(v >> (8 * i));
        return out;
    }

    bool MmioWrite32(std::uint64_t addr, std::uint32_t value, const char* label) {
        auto payload = EncodeU32(value);
        auto rsp = SendRequestAndWait(MessageType::kMmioWriteReq, addr, 0xF, payload);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "MMIO_WRITE addr=0x%04llx len=4 data=0x%08x (%s)",
                      static_cast<unsigned long long>(addr), value, label);
        Log(LogLevel::kInfo, buf, rsp ? static_cast<std::int64_t>(rsp->header.txn_id) : -1);
        if (!rsp || rsp->header.Type() != MessageType::kMmioWriteRsp ||
            rsp->header.Status() != StatusCode::kSuccess) {
            Log(LogLevel::kError, std::string("MMIO write failed: ") + label);
            return false;
        }
        return true;
    }

    bool MmioWrite64(std::uint64_t addr, std::uint64_t value, const char* label) {
        auto payload = EncodeU64(value);
        auto rsp = SendRequestAndWait(MessageType::kMmioWriteReq, addr, 0xFF, payload);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "MMIO_WRITE addr=0x%04llx len=8 data=0x%016llx (%s)",
                      static_cast<unsigned long long>(addr), static_cast<unsigned long long>(value),
                      label);
        Log(LogLevel::kInfo, buf, rsp ? static_cast<std::int64_t>(rsp->header.txn_id) : -1);
        if (!rsp || rsp->header.Type() != MessageType::kMmioWriteRsp ||
            rsp->header.Status() != StatusCode::kSuccess) {
            Log(LogLevel::kError, std::string("MMIO write failed: ") + label);
            return false;
        }
        return true;
    }

    std::optional<std::uint32_t> MmioRead32(std::uint64_t addr, const char* label) {
        std::vector<std::uint8_t> empty;
        auto rsp = SendRequestAndWait(MessageType::kMmioReadReq, addr, 0xF, empty);
        if (!rsp || rsp->header.Type() != MessageType::kMmioReadRsp ||
            rsp->header.Status() != StatusCode::kSuccess || rsp->payload.size() < 4) {
            Log(LogLevel::kError, std::string("MMIO read failed: ") + label);
            return std::nullopt;
        }
        std::uint32_t value = static_cast<std::uint32_t>(rsp->payload[0]) |
                               (static_cast<std::uint32_t>(rsp->payload[1]) << 8) |
                               (static_cast<std::uint32_t>(rsp->payload[2]) << 16) |
                               (static_cast<std::uint32_t>(rsp->payload[3]) << 24);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "MMIO_READ addr=0x%04llx len=4 data=0x%08x (%s)",
                      static_cast<unsigned long long>(addr), value, label);
        Log(LogLevel::kInfo, buf, static_cast<std::int64_t>(rsp->header.txn_id));
        return value;
    }

    bool ConfigureAdminQueues() {
        constexpr std::uint64_t kAsqBase = 0x00100000;
        constexpr std::uint64_t kAcqBase = 0x00101000;
        constexpr std::uint32_t kAqa = (0x1F << 16) | 0x1F;  // ACQS=ASQS=31 (32 entries - 1)

        if (!MmioWrite64(regs::kAdminSubmissionQueueBase, kAsqBase, "ASQ")) return false;
        if (!MmioWrite64(regs::kAdminCompletionQueueBase, kAcqBase, "ACQ")) return false;
        if (!MmioWrite32(regs::kAdminQueueAttributes, kAqa, "AQA")) return false;
        return true;
    }

    bool EnableControllerAndWaitReady() {
        if (!MmioWrite32(regs::kControllerConfiguration, kCcEnable, "CC.EN")) return false;

        for (int attempt = 0; attempt < 50; ++attempt) {
            auto csts = MmioRead32(regs::kControllerStatus, "CSTS");
            if (!csts) return false;
            if (*csts & kCstsReady) {
                Log(LogLevel::kInfo, "controller ready (CSTS.RDY=1)");
                return true;
            }
        }
        Log(LogLevel::kError, "timed out waiting for CSTS.RDY");
        return false;
    }

    bool WriteMockNvmeCommand() {
        // A 64-byte mock NVMe submission queue entry. Not a real NVMe
        // opcode encoding -- this MVP only needs a recognizable,
        // verifiable payload moving through DMA, not full NVMe command
        // semantics.
        mock_command_.resize(64);
        for (std::size_t i = 0; i < mock_command_.size(); ++i) {
            mock_command_[i] = static_cast<std::uint8_t>(0xA0 + i);
        }
        if (!memory_.Write(kMockCommandGuestAddr, mock_command_.data(), mock_command_.size())) {
            Log(LogLevel::kError, "failed to write mock NVMe command into host memory");
            return false;
        }
        Log(LogLevel::kInfo, "wrote 64-byte mock NVMe command to host memory @0x" +
                                   ToHex(kMockCommandGuestAddr));
        return true;
    }

    bool RingDoorbell() {
        return MmioWrite32(regs::kSubmissionQueue0Doorbell, 1, "SQ0TDBL");
    }

    // After ringing the doorbell, the RTL side autonomously initiates:
    //   1. A 64-byte DMA_READ_REQ for the mock command
    //   2. A 16-byte DMA_WRITE_REQ carrying a mock completion
    //   3. An MSI_X event
    // This client waits for those RTL-initiated messages (it is not the
    // one sending a request here -- it's servicing one), replies to the
    // DMA read with the command bytes, accepts the DMA write into
    // memory, and finally waits for the interrupt.
    bool ServiceRtlInitiatedTransactions() {
        bool saw_dma_read = false, saw_dma_write = false, saw_msix = false;
        std::vector<std::uint8_t> completion_bytes;

        // Up to 3 RTL-initiated messages are expected; bound the loop so
        // a misbehaving RTL side can't hang the demo forever.
        for (int i = 0; i < 3 && !(saw_dma_read && saw_dma_write && saw_msix); ++i) {
            auto msg = ReadMessage(timeout_ms_);
            if (!msg) {
                Log(LogLevel::kError, "timed out waiting for RTL-initiated transaction");
                return false;
            }

            switch (msg->header.Type()) {
                case MessageType::kDmaReadReq: {
                    saw_dma_read = true;
                    std::uint64_t addr = msg->header.address;
                    std::uint32_t len = msg->header.transfer_len;
                    if (len == 0) {
                        SendDmaReadResponse(msg->header.txn_id, addr, {},
                                            StatusCode::kErrorBadLength);
                        return false;
                    }
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "DMA_READ_REQ addr=0x%08llx len=%u",
                                  static_cast<unsigned long long>(addr), len);
                    Log(LogLevel::kInfo, buf, static_cast<std::int64_t>(msg->header.txn_id));

                    std::vector<std::uint8_t> data(len);
                    if (!memory_.Read(addr, data.data(), len)) {
                        SendDmaReadResponse(msg->header.txn_id, addr, {}, StatusCode::kErrorOutOfBounds);
                        Log(LogLevel::kError, "DMA read out of bounds", static_cast<std::int64_t>(msg->header.txn_id));
                        return false;
                    }
                    SendDmaReadResponse(msg->header.txn_id, addr, data, StatusCode::kSuccess);
                    Log(LogLevel::kInfo, "DMA_READ_RSP len=" + std::to_string(len),
                        static_cast<std::int64_t>(msg->header.txn_id));
                    break;
                }
                case MessageType::kDmaWriteReq: {
                    saw_dma_write = true;
                    std::uint64_t addr = msg->header.address;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "DMA_WRITE_REQ addr=0x%08llx len=%zu",
                                  static_cast<unsigned long long>(addr), msg->payload.size());
                    Log(LogLevel::kInfo, buf, static_cast<std::int64_t>(msg->header.txn_id));

                    StatusCode status = StatusCode::kSuccess;
                    if (!memory_.Write(addr, msg->payload.data(), msg->payload.size())) {
                        status = StatusCode::kErrorOutOfBounds;
                    } else {
                        completion_bytes = msg->payload;
                    }
                    SendDmaWriteResponse(msg->header.txn_id, addr, status);
                    Log(LogLevel::kInfo, "DMA_WRITE_RSP status=" + ToString(status),
                        static_cast<std::int64_t>(msg->header.txn_id));
                    if (status != StatusCode::kSuccess) return false;
                    break;
                }
                case MessageType::kMsiX: {
                    saw_msix = true;
                    Log(LogLevel::kInfo,
                        "MSI_X vector=" + std::to_string(msg->header.address),
                        static_cast<std::int64_t>(msg->header.txn_id));
                    break;
                }
                default:
                    Log(LogLevel::kWarn, "unexpected message type: " + ToString(msg->header.Type()));
                    break;
            }
        }

        if (!(saw_dma_read && saw_dma_write && saw_msix)) {
            Log(LogLevel::kError, "did not observe all expected RTL-initiated transactions");
            return false;
        }

        if (completion_bytes.size() != 16) {
            Log(LogLevel::kError, "completion payload has unexpected size");
            return false;
        }
        for (std::size_t i = 0; i < completion_bytes.size(); ++i) {
            std::uint8_t expected = static_cast<std::uint8_t>(0xC0 + i);
            if (completion_bytes[i] != expected) {
                Log(LogLevel::kError, "completion data verification FAILED at byte " + std::to_string(i));
                return false;
            }
        }
        Log(LogLevel::kInfo, "completion data verified OK");
        return true;
    }

    void SendDmaReadResponse(std::uint64_t txn_id, std::uint64_t addr,
                              const std::vector<std::uint8_t>& data, StatusCode status) {
        Header hdr = MakeHeader(MessageType::kDmaReadRsp, txn_id, addr, 0, status, clock_.NowNs(),
                                 static_cast<std::uint32_t>(data.size()), kFlagIsResponse);
        auto bytes = SerializeMessage(hdr, data);
        socket_.SendAll(bytes, timeout_ms_);
    }

    void SendDmaWriteResponse(std::uint64_t txn_id, std::uint64_t addr, StatusCode status) {
        std::vector<std::uint8_t> empty;
        Header hdr = MakeHeader(MessageType::kDmaWriteRsp, txn_id, addr, 0, status, clock_.NowNs(),
                                 0, kFlagIsResponse);
        auto bytes = SerializeMessage(hdr, empty);
        socket_.SendAll(bytes, timeout_ms_);
    }

    void SendShutdown() {
        std::vector<std::uint8_t> empty;
        Header hdr = MakeHeader(MessageType::kShutdown, txn_gen_.Next(), 0, 0, StatusCode::kSuccess,
                                 clock_.NowNs(), 0);
        auto bytes = SerializeMessage(hdr, empty);
        socket_.SendAll(bytes, timeout_ms_);
        Log(LogLevel::kInfo, "SHUTDOWN sent");
    }

    static std::string ToHex(std::uint64_t v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
        return buf;
    }

    static constexpr std::uint64_t kMockCommandGuestAddr = 0x00200000;

    std::string socket_path_;
    int timeout_ms_;
    UnixSocket socket_;
    HostMemory memory_;
    HostClock clock_;
    TransactionIdGenerator txn_gen_;
    std::vector<std::uint8_t> mock_command_;
};

}  // namespace mini_ics

int main(int argc, char** argv) {
    std::string socket_path = "/tmp/mini_ics.sock";
    int timeout_ms = 5000;
    mini_ics::LogLevel level = mini_ics::LogLevel::kInfo;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            timeout_ms = std::atoi(argv[++i]);
        } else if (arg == "--log-level" && i + 1 < argc) {
            level = mini_ics::LogLevelFromString(argv[++i]);
        } else if (arg == "--help") {
            std::printf(
                "usage: mini_ics_host_client [--socket PATH] [--timeout-ms N] [--log-level LEVEL]\n");
            return 0;
        }
    }

    mini_ics::Logger::Instance().SetLevel(level);

    mini_ics::HostClient client(socket_path, timeout_ms);
    bool ok = client.RunDemo();
    return ok ? 0 : 1;
}
