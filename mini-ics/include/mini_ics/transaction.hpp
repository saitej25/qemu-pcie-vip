// Transaction ID generation and request/response correlation, plus a
// small thread-safe queue used to hand messages between the socket I/O
// thread and the DPI-facing (simulator) thread.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "mini_ics/protocol.hpp"

namespace mini_ics {

// Monotonically increasing transaction ID source, safe to call from
// multiple threads (the socket thread allocates IDs for host-initiated
// requests; the DPI thread allocates IDs for RTL-initiated requests).
class TransactionIdGenerator {
public:
    explicit TransactionIdGenerator(std::uint64_t start = 1) : next_(start) {}
    std::uint64_t Next() { return next_.fetch_add(1, std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> next_;
};

// A generic thread-safe FIFO queue with a blocking pop that supports a
// timeout, so consumers never block indefinitely (important for the
// DPI-facing side, which must return control to the simulator
// regularly).
template <typename T>
class BlockingQueue {
public:
    void Push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(item));
        }
        cv_.notify_one();
    }

    // Returns std::nullopt if no item became available within
    // timeout_ms, or if Shutdown() was called and the queue is empty.
    std::optional<T> Pop(int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&] { return !queue_.empty() || shutdown_; })) {
            return std::nullopt;
        }
        if (queue_.empty()) {
            return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    // Non-blocking pop; returns std::nullopt immediately if empty.
    std::optional<T> TryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    bool shutdown_ = false;
};

// Correlates outstanding requests (keyed by transaction ID) with their
// eventual responses, with a per-request timeout. One side calls
// RegisterPending() right before sending a request, then WaitFor() to
// block (with timeout) until the matching response arrives via
// Complete(). If the timeout elapses, WaitFor() returns std::nullopt and
// the pending entry is removed so a late/duplicate response can't be
// mismatched against a future reused transaction ID.
class PendingRequestTable {
public:
    PendingRequestTable() = default;
    ~PendingRequestTable();

    void RegisterPending(std::uint64_t txn_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[txn_id] = std::nullopt;
    }

    // Called when a response message arrives. Returns false if the
    // transaction ID was not (or no longer) pending -- e.g. it already
    // timed out -- which callers should log as an out-of-order/unknown
    // response rather than crash on.
    bool Complete(std::uint64_t txn_id, Message response) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(txn_id);
        if (it == pending_.end()) {
            return false;
        }
        it->second = std::move(response);
        cv_.notify_all();
        return true;
    }

    // Blocks up to timeout_ms waiting for Complete() to be called for
    // txn_id. Removes the pending entry unconditionally before
    // returning, whether it succeeded or timed out.
    std::optional<Message> WaitFor(std::uint64_t txn_id, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = pending_.find(txn_id);
        if (it == pending_.end()) {
            return std::nullopt;
        }
        bool arrived = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            auto cur = pending_.find(txn_id);
            return cur == pending_.end() || cur->second.has_value();
        });

        auto cur = pending_.find(txn_id);
        if (cur == pending_.end()) {
            return std::nullopt;
        }
        std::optional<Message> result;
        if (arrived && cur->second.has_value()) {
            result = std::move(cur->second);
        }
        pending_.erase(cur);
        return result;
    }

    void CancelAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::uint64_t, std::optional<Message>> pending_;
};

}  // namespace mini_ics
