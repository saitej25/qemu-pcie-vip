#include "mini_ics/socket.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mini_ics {

std::string ToString(IoStatus status) {
    switch (status) {
        case IoStatus::kOk: return "OK";
        case IoStatus::kTimeout: return "TIMEOUT";
        case IoStatus::kDisconnected: return "DISCONNECTED";
        case IoStatus::kError: return "ERROR";
    }
    return "UNKNOWN_IO_STATUS";
}

namespace {

// Waits for `fd` to become ready for the requested poll event within
// timeout_ms. Retries on EINTR without consuming attempts. Returns kOk if
// ready, kTimeout if the wait elapsed, kError on an unrecoverable poll()
// failure.
IoStatus PollFor(int fd, short events, int timeout_ms) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = events;

    for (;;) {
        int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc > 0) {
            if (pfd.revents & (POLLERR | POLLNVAL)) {
                return IoStatus::kError;
            }
            if (pfd.revents & POLLHUP && !(pfd.revents & events)) {
                return IoStatus::kDisconnected;
            }
            return IoStatus::kOk;
        }
        if (rc == 0) {
            return IoStatus::kTimeout;
        }
        if (errno == EINTR) {
            continue;
        }
        return IoStatus::kError;
    }
}

}  // namespace

UnixSocket::~UnixSocket() { Close(); }

UnixSocket::UnixSocket(UnixSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

UnixSocket& UnixSocket::operator=(UnixSocket&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void UnixSocket::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

IoStatus UnixSocket::SendAll(const std::vector<std::uint8_t>& data, int timeout_ms) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        IoStatus wait = PollFor(fd_, POLLOUT, timeout_ms);
        if (wait != IoStatus::kOk) {
            return wait;
        }
        ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                return IoStatus::kDisconnected;
            }
            return IoStatus::kError;
        }
        if (n == 0) {
            return IoStatus::kDisconnected;
        }
        sent += static_cast<std::size_t>(n);
    }
    return IoStatus::kOk;
}

IoStatus UnixSocket::RecvAll(std::vector<std::uint8_t>& out, int timeout_ms) {
    std::size_t received = 0;
    while (received < out.size()) {
        IoStatus wait = PollFor(fd_, POLLIN, timeout_ms);
        if (wait != IoStatus::kOk) {
            return wait;
        }
        ssize_t n = ::recv(fd_, out.data() + received, out.size() - received, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return IoStatus::kError;
        }
        if (n == 0) {
            // Peer performed an orderly shutdown; never treat as "0 more
            // bytes needed", since `received` may still be < out.size().
            return IoStatus::kDisconnected;
        }
        received += static_cast<std::size_t>(n);
    }
    return IoStatus::kOk;
}

IoStatus UnixSocket::WaitReadable(int timeout_ms) { return PollFor(fd_, POLLIN, timeout_ms); }

UnixListener::~UnixListener() { Close(); }

UnixListener::UnixListener(UnixListener&& other) noexcept
    : fd_(other.fd_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

UnixListener& UnixListener::operator=(UnixListener&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

bool UnixListener::Listen(const std::string& path, int backlog) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    // Remove a stale socket file left behind by a previous, uncleanly
    // terminated server instance. unlink() failing because the path
    // doesn't exist is expected and fine; other failures surface via the
    // subsequent bind() failing.
    ::unlink(path.c_str());

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    if (::listen(fd, backlog) != 0) {
        ::close(fd);
        return false;
    }

    fd_ = fd;
    path_ = path;
    return true;
}

IoStatus UnixListener::Accept(UnixSocket& out, int timeout_ms) {
    IoStatus wait = PollFor(fd_, POLLIN, timeout_ms);
    if (wait != IoStatus::kOk) {
        return wait;
    }
    int client_fd = ::accept(fd_, nullptr, nullptr);
    if (client_fd < 0) {
        return IoStatus::kError;
    }
    out = UnixSocket(client_fd);
    return IoStatus::kOk;
}

void UnixListener::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        if (!path_.empty()) {
            ::unlink(path_.c_str());
            path_.clear();
        }
    }
}

IoStatus ConnectUnixSocket(const std::string& path, UnixSocket& out, int timeout_ms) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return IoStatus::kError;
    }

    // Make the connect() itself non-blocking so a slow/unresponsive
    // listener can't stall this call past timeout_ms.
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return IoStatus::kError;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        ::fcntl(fd, F_SETFL, flags);
        out = UnixSocket(fd);
        return IoStatus::kOk;
    }
    if (errno != EINPROGRESS) {
        ::close(fd);
        return IoStatus::kError;
    }

    IoStatus wait = PollFor(fd, POLLOUT, timeout_ms);
    if (wait != IoStatus::kOk) {
        ::close(fd);
        return wait;
    }

    int so_error = 0;
    socklen_t so_len = sizeof(so_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 || so_error != 0) {
        ::close(fd);
        return IoStatus::kError;
    }

    ::fcntl(fd, F_SETFL, flags);
    out = UnixSocket(fd);
    return IoStatus::kOk;
}

}  // namespace mini_ics
