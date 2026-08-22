#include "aa2acp/bridge/config.hpp"
#include "aa2acp/bridge/daemon_log.hpp"

#include <array>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace aa2acp::bridge {
namespace {

std::string log_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count() %
      1000;
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
  localtime_r(&time, &local_time);
  std::array<char, 40> text{};
  std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M:%S", &local_time);
  std::snprintf(text.data() + 19, text.size() - 19, ".%03lld ",
                static_cast<long long>(milliseconds));
  return text.data();
}

} // namespace

void RecentLog::append(const std::string_view text) {
  std::lock_guard lock(mutex_);
  contents_ += text;
  if (contents_.size() > kMaximumBytes)
    contents_.erase(0, contents_.size() - kMaximumBytes);
}

std::string RecentLog::snapshot() const {
  std::lock_guard lock(mutex_);
  return contents_;
}

class DaemonLog::TeeBuffer final : public std::streambuf {
public:
  TeeBuffer(std::streambuf *console, std::ofstream *file, RecentLog &recent,
            std::mutex &mutex, const LogLevel default_level)
      : console_(console), file_(file), recent_(recent), mutex_(mutex),
        default_level_(default_level) {}

private:
  static bool has_log_level_prefix(const std::string_view text) {
    for (const auto level : {LogLevel::debug, LogLevel::info, LogLevel::warning,
                             LogLevel::error}) {
      const auto name = log_level_name(level);
      if (text.starts_with("[" + std::string(name)))
        return true;
    }
    return false;
  }

  int_type overflow(const int_type character) override {
    if (traits_type::eq_int_type(character, traits_type::eof()))
      return traits_type::not_eof(character);
    std::lock_guard lock(mutex_);
    const auto text = traits_type::to_char_type(character);
    return write_locked(std::string_view(&text, 1)) ? character
                                                    : traits_type::eof();
  }

  std::streamsize xsputn(const char *text,
                         const std::streamsize size) override {
    std::lock_guard lock(mutex_);
    return write_locked(std::string_view(text, size)) ? size : 0;
  }

  int sync() override {
    std::lock_guard lock(mutex_);
    const auto console_result = console_->pubsync();
    if (file_ != nullptr)
      file_->flush();
    return console_result == 0 && (file_ == nullptr || *file_) ? 0 : -1;
  }

  bool write_locked(const std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
      if (at_line_start_) {
        const bool explicit_level = has_log_level_prefix(text.substr(offset));
        const auto level = current_log_level().value_or(default_level_);
        const char marker = offset == 0 ? '>' : '|';
        const std::string prefix =
            explicit_level ? "" : log_prefix(level, marker);
        if (!prefix.empty() && console_->sputn(prefix.data(), prefix.size()) !=
                                   static_cast<std::streamsize>(prefix.size()))
          return false;
        const auto retained_prefix = log_timestamp() + prefix;
        if (file_ != nullptr &&
            !file_->write(retained_prefix.data(), retained_prefix.size()))
          return false;
        recent_.append(retained_prefix);
        at_line_start_ = false;
      }
      const auto line_end = text.find('\n', offset);
      const auto count = line_end == std::string_view::npos
                             ? text.size() - offset
                             : line_end - offset + 1;
      if (console_->sputn(text.data() + offset, count) !=
          static_cast<std::streamsize>(count))
        return false;
      if (file_ != nullptr && !file_->write(text.data() + offset, count))
        return false;
      recent_.append(text.substr(offset, count));
      at_line_start_ = text[offset + count - 1] == '\n';
      offset += count;
    }
    return true;
  }

  std::streambuf *console_;
  std::ofstream *file_;
  RecentLog &recent_;
  std::mutex &mutex_;
  LogLevel default_level_;
  bool at_line_start_{true};
};

DaemonLog::DaemonLog(RecentLog &recent,
                     const std::optional<std::filesystem::path> &path)
    : path_(path),
      file_(path_ ? *path_ : std::filesystem::path{}, std::ios::app),
      cout_buffer_(std::make_unique<TeeBuffer>(std::cout.rdbuf(),
                                               file_ ? &file_ : nullptr, recent,
                                               mutex_, LogLevel::info)),
      cerr_buffer_(std::make_unique<TeeBuffer>(std::cerr.rdbuf(),
                                               file_ ? &file_ : nullptr, recent,
                                               mutex_, LogLevel::error)) {
  old_cout_ = std::cout.rdbuf(cout_buffer_.get());
  old_cerr_ = std::cerr.rdbuf(cerr_buffer_.get());
}

DaemonLog::~DaemonLog() {
  if (old_cout_ != nullptr)
    std::cout.rdbuf(old_cout_);
  if (old_cerr_ != nullptr)
    std::cerr.rdbuf(old_cerr_);
}

std::optional<std::filesystem::path> next_daemon_log_path() {
  const auto directory = default_state_directory() / "logs";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error)
    return std::nullopt;
  std::vector<std::filesystem::directory_entry> logs;
  for (const auto &entry :
       std::filesystem::directory_iterator(directory, error)) {
    if (entry.is_regular_file() && entry.path().extension() == ".log" &&
        entry.path().filename().string().starts_with("aa2acp-bridge-daemon-"))
      logs.push_back(entry);
  }
  std::sort(logs.begin(), logs.end(), [](const auto &left, const auto &right) {
    return left.last_write_time() > right.last_write_time();
  });
  for (std::size_t index = 29; index < logs.size(); ++index)
    std::filesystem::remove(logs[index], error);
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  const auto path =
      directory / ("aa2acp-bridge-daemon-" + std::to_string(stamp) + "-" +
                   std::to_string(getpid()) + ".log");
  return path;
}

} // namespace aa2acp::bridge
