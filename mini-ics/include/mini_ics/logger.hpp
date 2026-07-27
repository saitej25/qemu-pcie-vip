// Human-readable, level-filtered, thread-safe transaction logger.
//
// Log lines follow the fixed format:
//   [SRC ][NNNNNNNN ns][TXN nn] message...
// where SRC is a 4-character source tag ("HOST", "RTL ", "SERV", ...).
#pragma once

#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

namespace mini_ics {

enum class LogLevel {
    kError = 0,
    kWarn = 1,
    kInfo = 2,
    kDebug = 3,
    kTrace = 4,
};

std::string ToString(LogLevel level);

// Parses a level name (case-insensitive: "error", "warn", "info",
// "debug", "trace"). Returns kInfo if unrecognized.
LogLevel LogLevelFromString(const std::string& name);

// Process-wide logger. A single instance is fine here: log destination
// and level are process-global configuration by nature (there is one
// stdout, one minimum level chosen at startup), and every write is
// serialized through an internal mutex so it is safe to share across
// the socket thread, the DPI/simulator thread, and any worker threads.
class Logger {
public:
    static Logger& Instance();

    void SetLevel(LogLevel level);
    LogLevel Level() const;

    // source: 4-character-ish tag like "HOST", "RTL", "SERV".
    // sim_time_ns: simulation timestamp to render in the line; pass 0
    // for host-wall-clock-only contexts.
    void Log(LogLevel level, const std::string& source, std::uint64_t sim_time_ns,
              std::int64_t txn_id, const std::string& message);

    // Overload for messages not associated with a transaction (e.g.
    // connection setup/teardown).
    void Log(LogLevel level, const std::string& source, std::uint64_t sim_time_ns,
              const std::string& message);

private:
    Logger() = default;

    mutable std::mutex mutex_;
    LogLevel level_ = LogLevel::kInfo;
};

}  // namespace mini_ics

// Convenience macros so call sites read "MINI_ICS_LOG_INFO("HOST", ts, txn, ...)"
// without repeating Logger::Instance() everywhere. Implemented as
// functions under the hood (not string concatenation macros), so no
// macro-hygiene surprises beyond the usual varargs-to-stream forwarding.
#define MINI_ICS_LOG(level, source, sim_time_ns, txn_id, message) \
    ::mini_ics::Logger::Instance().Log((level), (source), (sim_time_ns), (txn_id), (message))

#define MINI_ICS_LOG_NOTXN(level, source, sim_time_ns, message) \
    ::mini_ics::Logger::Instance().Log((level), (source), (sim_time_ns), (message))
