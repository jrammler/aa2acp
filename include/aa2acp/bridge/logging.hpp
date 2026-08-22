#pragma once

#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>

namespace aa2acp::bridge {

enum class LogLevel { debug, info, warning, error };

inline std::string_view log_level_name(const LogLevel level) {
  switch (level) {
  case LogLevel::debug:
    return "debug";
  case LogLevel::info:
    return "info";
  case LogLevel::warning:
    return "warning";
  case LogLevel::error:
    return "error";
  }
  return "unknown";
}

inline bool debug_logging_enabled() {
  const auto *level = std::getenv("AA2ACP_LOG_LEVEL");
  return level != nullptr && std::string_view(level) == "debug";
}

inline bool log_level_enabled(const LogLevel level) {
  return level != LogLevel::debug || debug_logging_enabled();
}

inline thread_local std::optional<LogLevel> active_log_level;

class ScopedLogLevel final {
public:
  explicit ScopedLogLevel(const LogLevel level) : previous_(active_log_level) {
    active_log_level = level;
  }
  ~ScopedLogLevel() { active_log_level = previous_; }

private:
  std::optional<LogLevel> previous_;
};

inline std::optional<LogLevel> current_log_level() { return active_log_level; }

class LogLine final {
public:
  explicit LogLine(const LogLevel level)
      : level_(level), enabled_(log_level_enabled(level)) {}
  LogLine(const LogLine &) = delete;
  LogLine &operator=(const LogLine &) = delete;

  ~LogLine() {
    if (!enabled_ || message_.tellp() == std::streampos(0))
      return;
    auto &output = level_ == LogLevel::warning || level_ == LogLevel::error
                       ? std::cerr
                       : std::cout;
    output << '[' << log_level_name(level_) << "] " << message_.str();
  }

  template <typename Value> LogLine &operator<<(const Value &value) {
    if (enabled_)
      message_ << value;
    return *this;
  }

  LogLine &operator<<(std::ostream &(*manipulator)(std::ostream &)) {
    if (enabled_)
      manipulator(message_);
    return *this;
  }

  LogLine &write(const char *text, const std::streamsize size) {
    if (enabled_)
      message_.write(text, size);
    return *this;
  }

private:
  LogLevel level_;
  bool enabled_;
  std::ostringstream message_;
};

inline LogLine log(const LogLevel level) { return LogLine(level); }

} // namespace aa2acp::bridge
