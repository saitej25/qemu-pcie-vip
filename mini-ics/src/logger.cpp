#include "mini_ics/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iomanip>

namespace mini_ics {

std::string ToString(LogLevel level) {
    switch (level) {
        case LogLevel::kError: return "ERROR";
        case LogLevel::kWarn: return "WARN";
        case LogLevel::kInfo: return "INFO";
        case LogLevel::kDebug: return "DEBUG";
        case LogLevel::kTrace: return "TRACE";
    }
    return "UNKNOWN";
}

LogLevel LogLevelFromString(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower == "error") return LogLevel::kError;
    if (lower == "warn" || lower == "warning") return LogLevel::kWarn;
    if (lower == "debug") return LogLevel::kDebug;
    if (lower == "trace") return LogLevel::kTrace;
    return LogLevel::kInfo;
}

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::SetLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel Logger::Level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

void Logger::Log(LogLevel level, const std::string& source, std::uint64_t sim_time_ns,
                  std::int64_t txn_id, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level > level_) {
        return;
    }
    // Pad source to 4 characters so log columns line up, matching the
    // example format in docs/protocol.md ("[HOST]", "[RTL ]").
    std::string src = source;
    if (src.size() < 4) src.resize(4, ' ');

    std::fprintf(stdout, "[%s][%08llu ns][TXN %lld] %s\n", src.c_str(),
                 static_cast<unsigned long long>(sim_time_ns), static_cast<long long>(txn_id),
                 message.c_str());
    std::fflush(stdout);
}

void Logger::Log(LogLevel level, const std::string& source, std::uint64_t sim_time_ns,
                  const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level > level_) {
        return;
    }
    std::string src = source;
    if (src.size() < 4) src.resize(4, ' ');

    std::fprintf(stdout, "[%s][%08llu ns] %s\n", src.c_str(),
                 static_cast<unsigned long long>(sim_time_ns), message.c_str());
    std::fflush(stdout);
}

}  // namespace mini_ics
