#include "aa2acp/bridge/media_forwarders.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <poll.h>

#include "aa2acp/bridge/logging.hpp"

namespace aa2acp::bridge {

MediaSocketForwarder::MediaSocketForwarder(const std::filesystem::path &path,
                                           std::string name)
    : name_(std::move(name)), path_(path) {
  listener_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener_ < 0 || path_.string().size() >= sizeof(sockaddr_un::sun_path)) {
    close_listener();
    return;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto value = path_.string();
  std::copy(value.begin(), value.end(), address.sun_path);
  unlink(value.c_str());
  if (bind(listener_, reinterpret_cast<sockaddr *>(&address),
           sizeof(address)) != 0 ||
      chmod(value.c_str(), S_IRUSR | S_IWUSR) != 0 ||
      listen(listener_, 1) != 0) {
    close_listener();
    unlink(value.c_str());
    return;
  }
}

void MediaSocketForwarder::start_worker() {
  if (listener_ >= 0)
    worker_ =
        std::jthread([this](const std::stop_token stop) { forward(stop); });
}

MediaSocketForwarder::~MediaSocketForwarder() {
  worker_.request_stop();
  shutdown_client();
  frames_ready_.notify_all();
  if (worker_.joinable())
    worker_.join();
  close_listener();
  if (!path_.empty())
    unlink(path_.c_str());
}

void MediaSocketForwarder::enqueue(Bytes frame) {
  if (frame.empty() || frame.size() > kMaximumQueuedBytes)
    return;
  {
    std::lock_guard lock(mutex_);
    while ((!frames_.empty() && frames_.size() >= maximum_queued_frames_) ||
           (!frames_.empty() &&
            queued_bytes_ + frame.size() > kMaximumQueuedBytes)) {
      queued_bytes_ -= frames_.front().size();
      frames_.pop_front();
    }
    frames_.push_back(std::move(frame));
    queued_bytes_ += frames_.back().size();
  }
  frames_ready_.notify_one();
}

bool MediaSocketForwarder::on_client_connected(int) { return true; }

void MediaSocketForwarder::log_client_connected() {}

bool MediaSocketForwarder::send_all(const int socket_fd,
                                    const std::span<const uint8_t> bytes) {
  constexpr auto kWriteDeadline = std::chrono::seconds(2);
  const auto deadline = std::chrono::steady_clock::now() + kWriteDeadline;
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero())
      return false;
    pollfd descriptor{socket_fd, POLLOUT, 0};
    const auto timeout = static_cast<int>(std::min<std::int64_t>(
        100, std::max<std::int64_t>(1, remaining.count())));
    const auto ready = poll(&descriptor, 1, timeout);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (ready == 0)
      continue;
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
      return false;
    const auto count = send(socket_fd, bytes.data() + offset,
                            bytes.size() - offset, MSG_NOSIGNAL);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 &&
        (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    return false;
  }
  return true;
}

bool MediaSocketForwarder::send_frame(const int socket_fd, const Bytes &frame,
                                      const std::stop_token stop) {
  if (stop.stop_requested())
    return false;
  const auto size = frame.size();
  const std::array<std::uint8_t, 4> header{
      static_cast<std::uint8_t>(size >> 24),
      static_cast<std::uint8_t>(size >> 16),
      static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
  return send_all(socket_fd, header) && !stop.stop_requested() &&
         send_all(socket_fd, frame);
}

void MediaSocketForwarder::forward(const std::stop_token stop) {
  while (!stop.stop_requested()) {
    pollfd descriptor{listener_, POLLIN, 0};
    if (poll(&descriptor, 1, 100) <= 0 || (descriptor.revents & POLLIN) == 0)
      continue;
    const auto client =
        accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client < 0)
      continue;
    bool stopping = false;
    {
      std::lock_guard lock(client_mutex_);
      client_.store(client);
      stopping = stop.stop_requested();
      if (stopping && client_.exchange(-1) == client)
        close(client);
    }
    if (stopping)
      continue;
    if (!on_client_connected(client)) {
      std::lock_guard lock(client_mutex_);
      if (client_.exchange(-1) == client)
        close(client);
      continue;
    }
    log_client_connected();
    while (!stop.stop_requested()) {
      Bytes frame;
      {
        std::unique_lock lock(mutex_);
        frames_ready_.wait_for(lock, std::chrono::milliseconds(100), [&] {
          return stop.stop_requested() || !frames_.empty();
        });
        if (frames_.empty())
          continue;
        const auto frame_size = frames_.front().size();
        frame = std::move(frames_.front());
        frames_.pop_front();
        queued_bytes_ -= frame_size;
      }
      if (!send_frame(client, frame, stop))
        break;
    }
    {
      std::lock_guard lock(client_mutex_);
      if (client_.exchange(-1) == client)
        close(client);
    }
  }
}

void MediaSocketForwarder::close_listener() {
  if (listener_ >= 0) {
    close(listener_);
    listener_ = -1;
  }
}

void MediaSocketForwarder::shutdown_client() {
  std::lock_guard lock(client_mutex_);
  const auto client = client_.load();
  if (client >= 0)
    shutdown(client, SHUT_RDWR);
}

VideoSocketForwarder::VideoSocketForwarder(const std::filesystem::path &path)
    : MediaSocketForwarder(path, "video") {
  if (const char *dump_path = std::getenv("AA2ACP_DUMP_H264");
      dump_path != nullptr && *dump_path != '\0') {
    dump_fd_ = ::open(dump_path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                      S_IRUSR | S_IWUSR);
    struct stat status{};
    if (dump_fd_ >= 0 &&
        (::fstat(dump_fd_, &status) != 0 || !S_ISREG(status.st_mode) ||
         status.st_nlink != 1 || status.st_uid != ::geteuid() ||
         ::fchmod(dump_fd_, S_IRUSR | S_IWUSR) != 0 ||
         ::ftruncate(dump_fd_, 0) != 0)) {
      ::close(dump_fd_);
      dump_fd_ = -1;
    }
    if (dump_fd_ >= 0)
      log(LogLevel::info) << "Bridge daemon: capturing Android Auto H.264 to "
                          << dump_path << " (owner-only, 64 MiB limit)\n";
    else
      log(LogLevel::error)
          << "Bridge daemon: unable to capture Android Auto H.264 to "
          << dump_path << " (refusing unsafe paths)\n";
  }
  start_worker();
}

VideoSocketForwarder::~VideoSocketForwarder() {
  if (dump_fd_ >= 0)
    ::close(dump_fd_);
}

bool VideoSocketForwarder::write_dump(
    const std::span<const std::uint8_t> bytes) {
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto count =
        ::write(dump_fd_, bytes.data() + offset, bytes.size() - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    ::close(dump_fd_);
    dump_fd_ = -1;
    return false;
  }
  return true;
}

std::vector<VideoSocketForwarder::Bytes>
VideoSocketForwarder::nalus(const Bytes &input) {
  std::vector<Bytes> result;
  for (std::size_t offset = 0; offset + 3 <= input.size();) {
    const auto three =
        input[offset] == 0 && input[offset + 1] == 0 && input[offset + 2] == 1;
    const auto four = offset + 4 <= input.size() && input[offset] == 0 &&
                      input[offset + 1] == 0 && input[offset + 2] == 0 &&
                      input[offset + 3] == 1;
    if (!three && !four) {
      ++offset;
      continue;
    }
    const auto start = offset + (four ? 4U : 3U);
    auto end = start;
    while (end + 3 <= input.size()) {
      if ((end + 4 <= input.size() && input[end] == 0 && input[end + 1] == 0 &&
           input[end + 2] == 0 && input[end + 3] == 1) ||
          (input[end] == 0 && input[end + 1] == 0 && input[end + 2] == 1))
        break;
      ++end;
    }
    if (end + 3 > input.size())
      end = input.size();
    if (start < end)
      result.emplace_back(input.begin() + static_cast<std::ptrdiff_t>(start),
                          input.begin() + static_cast<std::ptrdiff_t>(end));
    offset = end;
  }
  return result;
}

void VideoSocketForwarder::push(
    const std::span<const std::uint8_t> access_unit) {
  if (access_unit.empty() || access_unit.size() > kMaximumQueuedBytes)
    return;
  Bytes frame(access_unit.begin(), access_unit.end());
  {
    std::lock_guard lock(mutex_);
    bool keyframe = false;
    const auto frame_nalus = nalus(frame);
    std::string nalu_summary;
    for (const auto &nalu : frame_nalus) {
      if (nalu.empty())
        continue;
      const auto type = nalu[0] & 0x1f;
      if (!nalu_summary.empty())
        nalu_summary += ',';
      nalu_summary += std::to_string(type) + ":" + std::to_string(nalu.size());
      if (type == 7)
        sps_ = nalu;
      else if (type == 8)
        pps_ = nalu;
      else if (type == 5)
        keyframe = true;
    }
    ++received_video_count_;
    constexpr std::size_t kMaximumDumpBytes = 64 * 1024 * 1024;
    if (dump_fd_ >= 0 && dump_bytes_ < kMaximumDumpBytes) {
      const auto count =
          std::min(frame.size(), kMaximumDumpBytes - dump_bytes_);
      if (write_dump(std::span(frame).first(count)))
        dump_bytes_ += count;
      if (dump_bytes_ == kMaximumDumpBytes)
        log(LogLevel::warning)
            << "Bridge daemon: H.264 diagnostic capture reached 64 MiB; "
               "stopping capture\n";
    }
    if (debug_logging_enabled() &&
        (received_video_count_ <= 5 || received_video_count_ % 60 == 0)) {
      log(LogLevel::debug) << "Bridge daemon: Android Auto H.264 access unit #"
                           << received_video_count_ << " (" << frame.size()
                           << " bytes; NAL type:size=" << nalu_summary << ")\n";
    }
    if (!keyframe) {
      // Preserve the original drop-new policy: when the queue is full the
      // incoming frame is discarded rather than evicting queued frames.
      if (frames_.size() < maximum_queued_frames_ &&
          queued_bytes_ + frame.size() <= kMaximumQueuedBytes) {
        queued_bytes_ += frame.size();
        frames_.push_back(std::move(frame));
      }
      frames_ready_.notify_one();
      return;
    }
    // CarPlay setup can take longer than the ordinary frame queue. Keep
    // the latest decoder entry point and every dependent frame after it.
    keyframe_ = frame;
    frames_.clear();
    queued_bytes_ = keyframe_.size();
    frames_.push_back(keyframe_);
    frames_ready_.notify_one();
    if (debug_logging_enabled())
      log(LogLevel::debug) << "Bridge daemon: retained Android Auto H.264 "
                              "keyframe and dependent frame sequence\n";
  }
}

bool VideoSocketForwarder::on_client_connected(const int client) {
  Bytes config;
  bool has_keyframe = false;
  {
    std::lock_guard lock(mutex_);
    if (!sps_.empty() && !pps_.empty()) {
      config = {0, 0, 0, 1};
      config.insert(config.end(), sps_.begin(), sps_.end());
      config.insert(config.end(), {0, 0, 0, 1});
      config.insert(config.end(), pps_.begin(), pps_.end());
    }
    has_keyframe = !keyframe_.empty();
    if (keyframe_.empty()) {
      frames_.clear();
      queued_bytes_ = 0;
    } else if (frames_.empty() || frames_.front() != keyframe_) {
      frames_.clear();
      frames_.push_back(keyframe_);
      queued_bytes_ = keyframe_.size();
    }
  }
  if (!config.empty() && !send_frame(client, config, {}))
    return false;
  if (debug_logging_enabled())
    log(LogLevel::debug) << "Bridge daemon: forwarded Android Auto H.264 "
                         << (config.empty() ? "without cached config"
                                            : "config")
                         << (has_keyframe ? " and cached keyframe sequence\n"
                                          : " and awaiting keyframe\n");
  return true;
}

void VideoSocketForwarder::log_client_connected() {}

AudioSocketForwarder::AudioSocketForwarder(const std::filesystem::path &path,
                                           std::string name)
    : MediaSocketForwarder(path, std::move(name)) {
  maximum_queued_frames_ = 100;
  start_worker();
}

void AudioSocketForwarder::log_client_connected() {
  if (debug_logging_enabled())
    log(LogLevel::debug) << "Bridge daemon: connected to Android Auto " << name_
                         << " audio source\n";
}

void AudioSocketForwarder::push(const std::span<const std::uint8_t> pcm) {
  if (pcm.empty())
    return;
  {
    std::lock_guard lock(mutex_);
    ++received_packets_;
    if (debug_logging_enabled() &&
        (received_packets_ <= 3 || received_packets_ % 500 == 0)) {
      log(LogLevel::debug) << "Bridge daemon: Android Auto " << name_
                           << " audio packet #" << received_packets_ << " ("
                           << pcm.size() << " bytes)\n";
    }
  }
  enqueue(Bytes(pcm.begin(), pcm.end()));
}

} // namespace aa2acp::bridge
