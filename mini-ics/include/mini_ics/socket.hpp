// RAII Unix domain socket wrapper with partial-I/O-safe send/recv and
// poll()-based timeouts. Nothing in this header blocks forever: every
// blocking call takes a timeout and returns a structured result instead
// of throwing on ordinary conditions (peer closed, timed out).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mini_ics {

enum class IoStatus {
    kOk,
    kTimeout,
    kDisconnected,
    kError,
};

std::string ToString(IoStatus status);

// Wraps a single connected socket file descriptor. Move-only; closes the
// fd on destruction. Not copyable, since a copied fd would create two
// owners of the same kernel resource.
class UnixSocket {
public:
    UnixSocket() = default;
    explicit UnixSocket(int fd) : fd_(fd) {}
    ~UnixSocket();

    UnixSocket(const UnixSocket&) = delete;
    UnixSocket& operator=(const UnixSocket&) = delete;
    UnixSocket(UnixSocket&& other) noexcept;
    UnixSocket& operator=(UnixSocket&& other) noexcept;

    bool IsValid() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    // Closes the socket early (also happens automatically in the
    // destructor). Safe to call multiple times.
    void Close();

    // Sends exactly `data.size()` bytes, looping over partial writes and
    // EINTR. Returns kTimeout if a single poll() wait exceeds
    // timeout_ms, kDisconnected if the peer closed mid-write, kError on
    // unrecoverable errno.
    IoStatus SendAll(const std::vector<std::uint8_t>& data, int timeout_ms);

    // Reads exactly `out.size()` bytes (caller pre-sizes the buffer),
    // looping over partial reads and EINTR. Same status semantics as
    // SendAll. A recv() returning 0 is treated as a graceful peer close
    // (kDisconnected), never silently swallowed as "0 bytes read".
    IoStatus RecvAll(std::vector<std::uint8_t>& out, int timeout_ms);

    // Non-blocking check for readability within timeout_ms (may be 0 for
    // an instant poll). Used by callers that want to peek without
    // committing to a full RecvAll.
    IoStatus WaitReadable(int timeout_ms);

private:
    int fd_ = -1;
};

// Server-side listening socket bound to a filesystem path. Removes any
// stale socket file at the same path before binding (a common source of
// EADDRINUSE across restarts), and unlinks it on destruction.
class UnixListener {
public:
    UnixListener() = default;
    ~UnixListener();

    UnixListener(const UnixListener&) = delete;
    UnixListener& operator=(const UnixListener&) = delete;
    UnixListener(UnixListener&& other) noexcept;
    UnixListener& operator=(UnixListener&& other) noexcept;

    // Creates, binds, and listens on `path`. Returns false and leaves the
    // listener invalid on failure (check errno via caller-side logging).
    bool Listen(const std::string& path, int backlog = 1);

    bool IsValid() const { return fd_ >= 0; }

    // Blocks up to timeout_ms waiting for an incoming connection.
    // Returns kTimeout if none arrived, otherwise kOk with `out` set to
    // the accepted socket.
    IoStatus Accept(UnixSocket& out, int timeout_ms);

    void Close();

private:
    int fd_ = -1;
    std::string path_;
};

// Client-side helper: connects to a Unix socket path with a timeout.
// Returns kOk and populates `out` on success.
IoStatus ConnectUnixSocket(const std::string& path, UnixSocket& out, int timeout_ms);

}  // namespace mini_ics
