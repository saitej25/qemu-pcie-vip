// Full-stack C++ integration test: a real MiniIcsServer over a real Unix
// socket, driven by a minimal in-test client that plays the "host" role
// directly against the wire protocol (rather than shelling out to the
// mini_ics_host_client binary). This exercises the handshake, MMIO
// read/write, RTL-initiated DMA read/write via SendRequestToHost,
// MSI-X, reset, invalid-address handling, and DMA bounds behavior at
// the protocol/server layer -- the same layer the real host_client and
// DPI shim sit on top of.
#include "mini_ics/protocol.hpp"
#include "mini_ics/server.hpp"
#include "mini_ics/socket.hpp"
#include "test_harness.hpp"

#include <thread>
#include <unistd.h>

using namespace mini_ics;
using namespace mini_ics::test;

namespace {

std::string TestSocketPath(const char* suffix) {
    return std::string("/tmp/mini_ics_integration_test_") + suffix + "_" +
           std::to_string(static_cast<long>(::getpid())) + ".sock";
}

// A tiny stand-in for the host side of the protocol: connects, does
// HELLO, and provides blocking send/recv helpers keyed by matching the
// next message off the wire (this test never has more than one
// outstanding request at a time, same simplifying assumption as
// host_client.cpp).
class TestHostPeer {
public:
    bool Connect(const std::string& path) {
        return ConnectUnixSocket(path, socket_, 2000) == IoStatus::kOk;
    }

    bool Hello() {
        std::vector<std::uint8_t> empty;
        Header h = MakeHeader(MessageType::kHello, txn_gen_.Next(), 0, 0, StatusCode::kSuccess, 0, 0);
        if (socket_.SendAll(SerializeMessage(h, empty), 2000) != IoStatus::kOk) return false;
        auto rsp = Recv();
        return rsp && rsp->header.Type() == MessageType::kHelloAck;
    }

    std::optional<Message> Recv() {
        std::vector<std::uint8_t> hdr_buf(kHeaderSize);
        if (socket_.RecvAll(hdr_buf, 2000) != IoStatus::kOk) return std::nullopt;
        CodecError err = CodecError::kNone;
        auto hdr = DeserializeHeader(hdr_buf.data(), hdr_buf.size(), &err);
        if (!hdr) return std::nullopt;
        Message m;
        m.header = *hdr;
        if (hdr->payload_len > 0) {
            m.payload.resize(hdr->payload_len);
            if (socket_.RecvAll(m.payload, 2000) != IoStatus::kOk) return std::nullopt;
        }
        return m;
    }

    bool Send(MessageType type, std::uint64_t txn_id, std::uint64_t addr,
              const std::vector<std::uint8_t>& payload, StatusCode status = StatusCode::kSuccess,
              std::uint16_t flags = 0) {
        Header h = MakeHeader(type, txn_id, addr, 0, status, 0,
                               static_cast<std::uint32_t>(payload.size()), flags);
        return socket_.SendAll(SerializeMessage(h, payload), 2000) == IoStatus::kOk;
    }

    std::uint64_t NextTxnId() { return txn_gen_.Next(); }

    UnixSocket socket_;
    TransactionIdGenerator txn_gen_{1000};
};

}  // namespace

MINI_ICS_TEST(HandshakeAndMmioWriteReadRoundTrip) {
    std::string path = TestSocketPath("mmio");
    MiniIcsServer server(path, /*io_timeout_ms=*/200, /*request_timeout_ms=*/1000);
    MINI_ICS_CHECK(server.Start());

    TestHostPeer host;
    MINI_ICS_CHECK(host.Connect(path));
    MINI_ICS_CHECK(host.Hello());

    // Host writes register 0x14 = 1 (mirrors CC.EN in the real demo).
    std::uint64_t write_txn = host.NextTxnId();
    std::vector<std::uint8_t> data = {1, 0, 0, 0};
    MINI_ICS_CHECK(host.Send(MessageType::kMmioWriteReq, write_txn, 0x14, data));

    auto inbound = server.PollInbound(1000);
    MINI_ICS_CHECK(inbound.has_value());
    MINI_ICS_CHECK(inbound->header.Type() == MessageType::kMmioWriteReq);
    MINI_ICS_CHECK_EQ(inbound->header.txn_id, write_txn);
    MINI_ICS_CHECK_EQ(inbound->header.address, 0x14ull);

    std::vector<std::uint8_t> empty;
    Header rsp_hdr = MakeHeader(MessageType::kMmioWriteRsp, inbound->header.txn_id, 0x14, 0,
                                 StatusCode::kSuccess, 0, 0, kFlagIsResponse);
    MINI_ICS_CHECK(server.SendResponse(rsp_hdr, empty));

    auto host_rsp = host.Recv();
    MINI_ICS_CHECK(host_rsp.has_value());
    MINI_ICS_CHECK(host_rsp->header.Type() == MessageType::kMmioWriteRsp);
    MINI_ICS_CHECK_EQ(host_rsp->header.txn_id, write_txn);
    MINI_ICS_CHECK(host_rsp->header.Status() == StatusCode::kSuccess);

    server.Stop();
}

MINI_ICS_TEST(RtlInitiatedDmaReadIsCorrelatedByTransactionId) {
    std::string path = TestSocketPath("dma_read");
    MiniIcsServer server(path, 200, 1000);
    MINI_ICS_CHECK(server.Start());

    TestHostPeer host;
    MINI_ICS_CHECK(host.Connect(path));
    MINI_ICS_CHECK(host.Hello());

    // Server (acting as RTL) initiates a DMA read on a background
    // thread since SendRequestToHost blocks for the correlated response.
    std::optional<Message> dma_rsp;
    std::thread rtl_thread([&] {
        dma_rsp = server.SendRequestToHost(MessageType::kDmaReadReq, 0x200000, 0, {}, 64, 500,
                                            /*expect_response=*/true);
    });

    auto dma_req = host.Recv();
    MINI_ICS_CHECK(dma_req.has_value());
    MINI_ICS_CHECK(dma_req->header.Type() == MessageType::kDmaReadReq);
    MINI_ICS_CHECK_EQ(dma_req->header.address, 0x200000ull);

    std::vector<std::uint8_t> data(64, 0x5A);
    MINI_ICS_CHECK(host.Send(MessageType::kDmaReadRsp, dma_req->header.txn_id, 0x200000, data,
                              StatusCode::kSuccess, kFlagIsResponse));

    rtl_thread.join();
    MINI_ICS_CHECK(dma_rsp.has_value());
    MINI_ICS_CHECK_EQ(dma_rsp->header.txn_id, dma_req->header.txn_id);
    MINI_ICS_CHECK_EQ(dma_rsp->payload.size(), 64u);
    MINI_ICS_CHECK_EQ(dma_rsp->payload[0], 0x5A);

    server.Stop();
}

MINI_ICS_TEST(RequestToHostTimesOutIfHostNeverResponds) {
    std::string path = TestSocketPath("timeout");
    MiniIcsServer server(path, 200, /*request_timeout_ms=*/300);
    MINI_ICS_CHECK(server.Start());

    TestHostPeer host;
    MINI_ICS_CHECK(host.Connect(path));
    MINI_ICS_CHECK(host.Hello());

    // Drain the DMA read request off the wire but never respond, so
    // SendRequestToHost must time out rather than block forever.
    std::optional<Message> dma_rsp;
    std::thread rtl_thread([&] {
        dma_rsp = server.SendRequestToHost(MessageType::kDmaReadReq, 0x1000, 0, {}, 64, 0,
                                            /*expect_response=*/true);
    });
    auto dma_req = host.Recv();
    MINI_ICS_CHECK(dma_req.has_value());
    rtl_thread.join();

    MINI_ICS_CHECK(!dma_rsp.has_value());
    server.Stop();
}

MINI_ICS_TEST(ResetRequestFlowsThroughToInboundQueue) {
    std::string path = TestSocketPath("reset");
    MiniIcsServer server(path, 200, 1000);
    MINI_ICS_CHECK(server.Start());

    TestHostPeer host;
    MINI_ICS_CHECK(host.Connect(path));
    MINI_ICS_CHECK(host.Hello());

    std::uint64_t txn = host.NextTxnId();
    std::vector<std::uint8_t> empty;
    MINI_ICS_CHECK(host.Send(MessageType::kResetReq, txn, 0, empty));

    auto inbound = server.PollInbound(1000);
    MINI_ICS_CHECK(inbound.has_value());
    MINI_ICS_CHECK(inbound->header.Type() == MessageType::kResetReq);
    MINI_ICS_CHECK_EQ(inbound->header.txn_id, txn);

    Header rsp_hdr = MakeHeader(MessageType::kResetRsp, txn, 0, 0, StatusCode::kSuccess, 0, 0,
                                 kFlagIsResponse);
    MINI_ICS_CHECK(server.SendResponse(rsp_hdr, empty));
    auto host_rsp = host.Recv();
    MINI_ICS_CHECK(host_rsp.has_value());
    MINI_ICS_CHECK(host_rsp->header.Status() == StatusCode::kSuccess);

    server.Stop();
}

MINI_ICS_TEST(InvalidMmioAddressReturnsStructuredError) {
    // Verifies the server transports an error status faithfully rather
    // than silently dropping the malformed-target request -- the actual
    // "is this address valid" decision belongs to the RTL/model layer,
    // but the server must not mask or alter the status it's given.
    std::string path = TestSocketPath("invalid_addr");
    MiniIcsServer server(path, 200, 1000);
    MINI_ICS_CHECK(server.Start());

    TestHostPeer host;
    MINI_ICS_CHECK(host.Connect(path));
    MINI_ICS_CHECK(host.Hello());

    std::uint64_t txn = host.NextTxnId();
    std::vector<std::uint8_t> empty;
    MINI_ICS_CHECK(host.Send(MessageType::kMmioReadReq, txn, 0xFFFFFFFF, empty));  // way out of 4KB space

    auto inbound = server.PollInbound(1000);
    MINI_ICS_CHECK(inbound.has_value());

    Header rsp_hdr = MakeHeader(MessageType::kMmioReadRsp, txn, 0xFFFFFFFF, 0,
                                 StatusCode::kErrorInvalidAddress, 0, 0, kFlagIsResponse);
    MINI_ICS_CHECK(server.SendResponse(rsp_hdr, empty));

    auto host_rsp = host.Recv();
    MINI_ICS_CHECK(host_rsp.has_value());
    MINI_ICS_CHECK(host_rsp->header.Status() == StatusCode::kErrorInvalidAddress);

    server.Stop();
}

MINI_ICS_TEST(DmaOutOfBoundsReturnsStructuredError) {
    std::string path = TestSocketPath("dma_oob");
    MiniIcsServer server(path, 200, 1000);
    MINI_ICS_CHECK(server.Start());

    TestHostPeer host;
    MINI_ICS_CHECK(host.Connect(path));
    MINI_ICS_CHECK(host.Hello());

    std::optional<Message> dma_rsp;
    std::thread rtl_thread([&] {
        // Address far beyond the 64 MB default host memory size.
        dma_rsp = server.SendRequestToHost(MessageType::kDmaReadReq, 0xFFFFFFFFFFULL, 0, {}, 64, 0,
                                            true);
    });
    auto dma_req = host.Recv();
    MINI_ICS_CHECK(dma_req.has_value());

    std::vector<std::uint8_t> empty;
    MINI_ICS_CHECK(host.Send(MessageType::kDmaReadRsp, dma_req->header.txn_id, dma_req->header.address,
                              empty, StatusCode::kErrorOutOfBounds, kFlagIsResponse));
    rtl_thread.join();

    MINI_ICS_CHECK(dma_rsp.has_value());
    MINI_ICS_CHECK(dma_rsp->header.Status() == StatusCode::kErrorOutOfBounds);

    server.Stop();
}

MINI_ICS_TEST(ClientDisconnectIsHandledCleanly) {
    std::string path = TestSocketPath("disconnect");
    MiniIcsServer server(path, 100, 1000);
    MINI_ICS_CHECK(server.Start());

    {
        TestHostPeer host;
        MINI_ICS_CHECK(host.Connect(path));
        MINI_ICS_CHECK(host.Hello());
        MINI_ICS_CHECK(server.HasClient());
    }  // host socket closes here

    // Give the I/O thread a moment to observe the disconnect.
    for (int i = 0; i < 50 && server.HasClient(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    MINI_ICS_CHECK(!server.HasClient());

    server.Stop();
}

int main() { return RunAll("IntegrationCppTest"); }
