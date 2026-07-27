#include "mini_ics/socket.hpp"
#include "test_harness.hpp"

#include <thread>
#include <unistd.h>

using namespace mini_ics;
using namespace mini_ics::test;

namespace {
std::string TestSocketPath(const char* suffix) {
    return std::string("/tmp/mini_ics_socket_test_") + suffix + "_" +
           std::to_string(static_cast<long>(::getpid())) + ".sock";
}
}  // namespace

MINI_ICS_TEST(ListenerBindsAndAcceptsClient) {
    std::string path = TestSocketPath("accept");
    UnixListener listener;
    MINI_ICS_CHECK(listener.Listen(path));

    UnixSocket server_side, client_side;
    std::thread server_thread([&] {
        IoStatus st = listener.Accept(server_side, 2000);
        MINI_ICS_CHECK(st == IoStatus::kOk);
    });

    IoStatus cst = ConnectUnixSocket(path, client_side, 2000);
    server_thread.join();

    MINI_ICS_CHECK(cst == IoStatus::kOk);
    MINI_ICS_CHECK(client_side.IsValid());
    MINI_ICS_CHECK(server_side.IsValid());
}

MINI_ICS_TEST(SendAllAndRecvAllRoundTrip) {
    std::string path = TestSocketPath("roundtrip");
    UnixListener listener;
    MINI_ICS_CHECK(listener.Listen(path));

    UnixSocket server_side, client_side;
    std::thread server_thread(
        [&] { MINI_ICS_CHECK(listener.Accept(server_side, 2000) == IoStatus::kOk); });
    MINI_ICS_CHECK(ConnectUnixSocket(path, client_side, 2000) == IoStatus::kOk);
    server_thread.join();

    std::vector<std::uint8_t> payload(5000);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i);

    std::thread sender([&] { MINI_ICS_CHECK(client_side.SendAll(payload, 2000) == IoStatus::kOk); });

    std::vector<std::uint8_t> received(payload.size());
    IoStatus rst = server_side.RecvAll(received, 2000);
    sender.join();

    MINI_ICS_CHECK(rst == IoStatus::kOk);
    MINI_ICS_CHECK(received == payload);
}

MINI_ICS_TEST(RecvAllHandlesPartialDelivery) {
    // Exercises payloads larger than a single socket read/recv by
    // sending in several small chunks with delays, verifying RecvAll
    // reassembles them transparently.
    std::string path = TestSocketPath("partial");
    UnixListener listener;
    MINI_ICS_CHECK(listener.Listen(path));

    UnixSocket server_side, client_side;
    std::thread server_thread(
        [&] { MINI_ICS_CHECK(listener.Accept(server_side, 2000) == IoStatus::kOk); });
    MINI_ICS_CHECK(ConnectUnixSocket(path, client_side, 2000) == IoStatus::kOk);
    server_thread.join();

    constexpr std::size_t kTotal = 1000;
    std::thread sender([&] {
        std::vector<std::uint8_t> chunk(37, 0xAB);  // odd size to force many partial writes
        std::size_t sent = 0;
        while (sent < kTotal) {
            std::size_t n = std::min(chunk.size(), kTotal - sent);
            std::vector<std::uint8_t> piece(chunk.begin(), chunk.begin() + n);
            MINI_ICS_CHECK(client_side.SendAll(piece, 2000) == IoStatus::kOk);
            sent += n;
        }
    });

    std::vector<std::uint8_t> received(kTotal);
    IoStatus rst = server_side.RecvAll(received, 2000);
    sender.join();

    MINI_ICS_CHECK(rst == IoStatus::kOk);
    for (auto b : received) MINI_ICS_CHECK_EQ(b, 0xAB);
}

MINI_ICS_TEST(RecvAllDetectsClientDisconnect) {
    std::string path = TestSocketPath("disconnect");
    UnixListener listener;
    MINI_ICS_CHECK(listener.Listen(path));

    UnixSocket server_side, client_side;
    std::thread server_thread(
        [&] { MINI_ICS_CHECK(listener.Accept(server_side, 2000) == IoStatus::kOk); });
    MINI_ICS_CHECK(ConnectUnixSocket(path, client_side, 2000) == IoStatus::kOk);
    server_thread.join();

    client_side.Close();

    std::vector<std::uint8_t> buf(16);
    IoStatus st = server_side.RecvAll(buf, 2000);
    MINI_ICS_CHECK(st == IoStatus::kDisconnected);
}

MINI_ICS_TEST(RecvAllTimesOutWhenNoDataArrives) {
    std::string path = TestSocketPath("timeout");
    UnixListener listener;
    MINI_ICS_CHECK(listener.Listen(path));

    UnixSocket server_side, client_side;
    std::thread server_thread(
        [&] { MINI_ICS_CHECK(listener.Accept(server_side, 2000) == IoStatus::kOk); });
    MINI_ICS_CHECK(ConnectUnixSocket(path, client_side, 2000) == IoStatus::kOk);
    server_thread.join();

    std::vector<std::uint8_t> buf(16);
    IoStatus st = server_side.RecvAll(buf, 150);  // nobody ever sends
    MINI_ICS_CHECK(st == IoStatus::kTimeout);
}

MINI_ICS_TEST(ListenerAcceptTimesOutWithNoConnection) {
    std::string path = TestSocketPath("accept_timeout");
    UnixListener listener;
    MINI_ICS_CHECK(listener.Listen(path));

    UnixSocket server_side;
    IoStatus st = listener.Accept(server_side, 150);
    MINI_ICS_CHECK(st == IoStatus::kTimeout);
}

MINI_ICS_TEST(ConnectFailsToNonexistentSocket) {
    std::string path = TestSocketPath("does_not_exist");
    ::unlink(path.c_str());
    UnixSocket client_side;
    IoStatus st = ConnectUnixSocket(path, client_side, 500);
    MINI_ICS_CHECK(st == IoStatus::kError);
}

int main() { return RunAll("SocketTest"); }
