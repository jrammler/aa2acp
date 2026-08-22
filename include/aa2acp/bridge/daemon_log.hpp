#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <streambuf>
#include <string>
#include <string_view>

#include "aa2acp/bridge/logging.hpp"

namespace aa2acp::bridge {

// Retains a bounded tail of recent log output for the management UI.
class RecentLog final {
public:
  static constexpr std::size_t kMaximumBytes = 200 * 1024;

  void append(std::string_view text);
  std::string snapshot() const;

private:
  mutable std::mutex mutex_;
  std::string contents_;
};

// Mirrors stdout/stderr into an optional log file and a RecentLog while
// adding timestamps and default level prefixes.
class DaemonLog final {
public:
  DaemonLog(RecentLog &recent,
            const std::optional<std::filesystem::path> &path);
  ~DaemonLog();

  DaemonLog(const DaemonLog &) = delete;
  DaemonLog &operator=(const DaemonLog &) = delete;

  const std::optional<std::filesystem::path> &path() const { return path_; }

private:
  class TeeBuffer;

  std::optional<std::filesystem::path> path_;
  std::ofstream file_;
  std::mutex mutex_;
  std::unique_ptr<TeeBuffer> cout_buffer_;
  std::unique_ptr<TeeBuffer> cerr_buffer_;
  std::streambuf *old_cout_{};
  std::streambuf *old_cerr_{};
};

// Returns a fresh log path under the state directory's logs subdirectory,
// pruning older daemon logs beyond thirty entries.
std::optional<std::filesystem::path> next_daemon_log_path();

} // namespace aa2acp::bridge
