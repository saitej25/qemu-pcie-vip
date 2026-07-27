// DPI-C shim: bridges the SystemVerilog testbench to the in-process
// Mini ICS server (mini_ics/server.hpp). This .so is loaded directly by
// the simulator (QuestaSim vsim -sv_lib, or a Verilator DPI link), so
// "the server" and "the RTL simulator" are the same OS process here --
// unlike the standalone mini_ics_server executable, which is a separate
// process talking over the same socket to a separate host_client.
#include "mini_ics_dpi.hpp"

#include <memory>
#include <mutex>
#include <vector>

#include "mini_ics/logger.hpp"
#include "mini_ics/server.hpp"

using namespace mini_ics;

namespace {

// A single global server instance is intentional, not an oversight: DPI
// import functions are plain C functions with no `this`, and QuestaSim
// (and Verilator) only ever instantiate one Mini ICS endpoint per
// simulation process in this milestone, so there is exactly one server
// to own. Nothing sim-side is stored in globals; SV-visible register
// state stays inside rtl/*.sv.
std::unique_ptr<MiniIcsServer> g_server;
std::mutex g_state_mutex;  // guards g_server and the two staging buffers below

// Payload staged by the most recent successful mini_ics_poll(), read
// back one byte at a time via mini_ics_get_payload_byte(). Kept
// separate from g_response_payload so a DMA response received while
// draining an inbound poll can never clobber it.
std::vector<std::uint8_t> g_inbound_payload;

// Payload staged by the most recent mini_ics_send_request() response,
// read back via mini_ics_get_response_payload_byte().
std::vector<std::uint8_t> g_response_payload;

constexpr int kPollTimeoutMs = 1;         // mini_ics_poll must return promptly every call
constexpr int kDefaultRequestTimeoutMs = 2000;

}  // namespace

extern "C" {

int mini_ics_init(const char* socket_path) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_server) {
        MINI_ICS_LOG_NOTXN(LogLevel::kWarn, "DPI ", 0, "mini_ics_init called twice; ignoring");
        return 0;
    }
    if (socket_path == nullptr) {
        MINI_ICS_LOG_NOTXN(LogLevel::kError, "DPI ", 0, "mini_ics_init: null socket_path");
        return -1;
    }

    g_server = std::make_unique<MiniIcsServer>(std::string(socket_path), /*io_timeout_ms=*/50,
                                                kDefaultRequestTimeoutMs);
    if (!g_server->Start()) {
        MINI_ICS_LOG_NOTXN(LogLevel::kError, "DPI ", 0, "failed to start server");
        g_server.reset();
        return -1;
    }
    MINI_ICS_LOG_NOTXN(LogLevel::kInfo, "DPI ", 0,
                        std::string("mini_ics_init: listening on ") + socket_path);
    return 0;
}

int mini_ics_poll(unsigned int* msg_type, unsigned long long* transaction_id,
                   unsigned long long* address, unsigned int* length, unsigned int* status) {
    if (!msg_type || !transaction_id || !address || !length || !status) {
        return -1;
    }

    MiniIcsServer* server = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (!g_server || !g_server->IsRunning()) {
            return -1;
        }
        server = g_server.get();
    }

    // Bounded by kPollTimeoutMs so this DPI call always returns quickly;
    // the SV side is expected to call mini_ics_poll() once per clock
    // (or configurable interval) rather than block waiting here.
    auto msg = server->PollInbound(kPollTimeoutMs);
    if (!msg) {
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_inbound_payload = msg->payload;
    }

    *msg_type = static_cast<unsigned int>(msg->header.msg_type);
    *transaction_id = msg->header.txn_id;
    *address = msg->header.address;
    *length = msg->header.transfer_len;
    *status = static_cast<unsigned int>(msg->header.status);
    return 1;
}

int mini_ics_get_payload_byte(unsigned int index, svBitVecVal* value) {
    if (!value) return -1;
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (index >= g_inbound_payload.size()) {
        return -1;
    }
    *value = g_inbound_payload[index];
    return 0;
}

namespace {
// Copies bytes out of a SystemVerilog open array of `byte unsigned` (a
// packed 8-bit vector per element) into a std::vector<uint8_t>. DPI open
// arrays are only valid for the duration of the call, so this must
// happen before returning to the caller of mini_ics_send_response /
// mini_ics_send_request.
std::vector<std::uint8_t> CopyOpenArray(const svOpenArrayHandle handle, unsigned int length) {
    std::vector<std::uint8_t> out(length);
    auto* data = static_cast<std::uint8_t*>(svGetArrayPtr(handle));
    if (data != nullptr) {
        // Contiguous packed array fast path.
        for (unsigned int i = 0; i < length; ++i) out[i] = data[i];
    } else {
        for (unsigned int i = 0; i < static_cast<unsigned int>(svSize(handle, 1)) && i < length; ++i) {
            void* elem = svGetArrElemPtr1(handle, i);
            out[i] = elem ? *static_cast<std::uint8_t*>(elem) : 0;
        }
    }
    return out;
}
}  // namespace

int mini_ics_send_response(unsigned int msg_type, unsigned long long transaction_id,
                            unsigned long long address, unsigned int status,
                            const svOpenArrayHandle payload, unsigned int length) {
    MiniIcsServer* server = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (!g_server || !g_server->IsRunning()) return -1;
        server = g_server.get();
    }

    std::vector<std::uint8_t> data = length > 0 && payload ? CopyOpenArray(payload, length)
                                                            : std::vector<std::uint8_t>{};

    Header hdr = MakeHeader(static_cast<MessageType>(msg_type), transaction_id, address, 0,
                             static_cast<StatusCode>(status), /*sim_timestamp=*/0,
                             static_cast<std::uint32_t>(data.size()), kFlagIsResponse);
    return server->SendResponse(hdr, data) ? 0 : -2;
}

int mini_ics_send_request(unsigned int msg_type, unsigned long long* transaction_id,
                           unsigned long long address, const svOpenArrayHandle payload,
                           unsigned int length, unsigned int* out_status) {
    if (!transaction_id || !out_status) return -1;

    MiniIcsServer* server = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (!g_server || !g_server->IsRunning()) return -1;
        server = g_server.get();
    }

    MessageType type = static_cast<MessageType>(msg_type);
    std::vector<std::uint8_t> data;
    if (type != MessageType::kDmaReadReq && length > 0 && payload) {
        data = CopyOpenArray(payload, length);
    }
    // Only DMA requests expect a correlated response; MSI_X is
    // fire-and-forget by protocol design (there is no MSI_X_RSP type).
    bool expect_response = (type == MessageType::kDmaReadReq || type == MessageType::kDmaWriteReq);

    std::uint64_t txn_id = 0;
    auto rsp = server->SendRequestToHost(type, address, /*byte_enable=*/0, data, length,
                                          /*sim_timestamp=*/0, expect_response, &txn_id);
    *transaction_id = txn_id;

    if (!expect_response) {
        *out_status = static_cast<unsigned int>(StatusCode::kSuccess);
        return 0;
    }
    if (!rsp) {
        *out_status = static_cast<unsigned int>(StatusCode::kErrorTimeout);
        return -3;
    }

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_response_payload = rsp->payload;
    }
    *out_status = static_cast<unsigned int>(rsp->header.status);
    return rsp->header.Status() == StatusCode::kSuccess ? 0 : -2;
}

int mini_ics_get_response_payload_byte(unsigned int index, svBitVecVal* value) {
    if (!value) return -1;
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (index >= g_response_payload.size()) {
        return -1;
    }
    *value = g_response_payload[index];
    return 0;
}

void mini_ics_shutdown() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_server) {
        g_server->Stop();
        g_server.reset();
    }
    g_inbound_payload.clear();
    g_response_payload.clear();
}

}  // extern "C"
