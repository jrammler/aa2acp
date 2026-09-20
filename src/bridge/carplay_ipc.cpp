#include "aa2acp/bridge/carplay_ipc.hpp"

#include "aa2acp/bridge/logging.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace aa2acp::bridge {
namespace {
constexpr std::size_t kMaximumFrameSize = 4 * 1024 * 1024;
constexpr std::size_t kMaximumQueuedFrames = 256;
constexpr std::size_t kMaximumQueuedBytes = 16 * 1024 * 1024;

bool send_all(const int socket_fd, const std::span<const std::uint8_t> bytes,
              const std::stop_token stop) {
  constexpr auto kWriteDeadline = std::chrono::seconds(2);
  const auto deadline = std::chrono::steady_clock::now() + kWriteDeadline;
  std::size_t offset{};
  while (offset < bytes.size() && !stop.stop_requested()) {
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
  return offset == bytes.size();
}

bool receive_all(const int socket_fd, std::span<std::uint8_t> bytes,
                 const std::stop_token stop) {
  constexpr auto kReadDeadline = std::chrono::seconds(10);
  const auto deadline = std::chrono::steady_clock::now() + kReadDeadline;
  std::size_t offset{};
  while (offset < bytes.size() && !stop.stop_requested()) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero())
      return false;
    pollfd descriptor{socket_fd, POLLIN, 0};
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
    const auto count =
        recv(socket_fd, bytes.data() + offset, bytes.size() - offset, 0);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 &&
        (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    return false;
  }
  return offset == bytes.size();
}

int connect_unix(const std::filesystem::path &path) {
  const auto text = path.string();
  if (text.size() >= sizeof(sockaddr_un::sun_path))
    return -1;
  const auto socket_fd =
      socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (socket_fd < 0)
    return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::copy(text.begin(), text.end(), address.sun_path);
  const auto connect_result = connect(
      socket_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
  if (connect_result != 0 && errno != EINPROGRESS) {
    close(socket_fd);
    return -1;
  }
  if (connect_result != 0) {
    pollfd descriptor{socket_fd, POLLOUT, 0};
    if (poll(&descriptor, 1, 1000) <= 0 ||
        (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      close(socket_fd);
      return -1;
    }
    int socket_error{};
    socklen_t socket_error_size = sizeof(socket_error);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error,
                   &socket_error_size) != 0 ||
        socket_error != 0) {
      close(socket_fd);
      return -1;
    }
  }
  return socket_fd;
}
} // namespace

FrameSocketReceiver::FrameSocketReceiver(const std::filesystem::path &path,
                                         std::string name, Callback callback)
    : path_(path), name_(std::move(name)), callback_(std::move(callback)) {
  const auto text = path_.string();
  if (text.size() >= sizeof(sockaddr_un::sun_path))
    return;
  listener_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener_ < 0)
    return;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::copy(text.begin(), text.end(), address.sun_path);
  unlink(text.c_str());
  if (bind(listener_, reinterpret_cast<const sockaddr *>(&address),
           sizeof(address)) != 0 ||
      chmod(text.c_str(), S_IRUSR | S_IWUSR) != 0 ||
      listen(listener_, 1) != 0) {
    close(listener_);
    listener_ = -1;
    unlink(text.c_str());
    return;
  }
  worker_ = std::jthread([this](const std::stop_token stop) { receive(stop); });
}

FrameSocketReceiver::~FrameSocketReceiver() {
  worker_.request_stop();
  shutdown_client();
  if (listener_ >= 0)
    shutdown(listener_, SHUT_RDWR);
  if (worker_.joinable())
    worker_.join();
  if (listener_ >= 0)
    close(listener_);
  if (!path_.empty())
    unlink(path_.c_str());
}

void FrameSocketReceiver::shutdown_client() {
  std::lock_guard lock(client_mutex_);
  if (client_ >= 0)
    shutdown(client_, SHUT_RDWR);
}

void FrameSocketReceiver::receive(const std::stop_token stop) {
  while (!stop.stop_requested()) {
    pollfd descriptor{listener_, POLLIN, 0};
    if (poll(&descriptor, 1, 100) <= 0)
      continue;
    if ((descriptor.revents & POLLIN) == 0)
      continue;
    const auto client =
        accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client < 0)
      continue;
    {
      std::lock_guard lock(client_mutex_);
      if (client_ >= 0)
        shutdown(client_, SHUT_RDWR);
      client_ = client;
    }
    if (debug_logging_enabled())
      log(LogLevel::debug) << "Bridge daemon: connected to CarPlay " << name_
                           << " IPC\n";
    while (!stop.stop_requested()) {
      std::array<std::uint8_t, 4> header{};
      if (!receive_all(client, header, stop))
        break;
      const auto size = (static_cast<std::size_t>(header[0]) << 24) |
                        (static_cast<std::size_t>(header[1]) << 16) |
                        (static_cast<std::size_t>(header[2]) << 8) | header[3];
      if (size == 0 || size > kMaximumFrameSize) {
        log(LogLevel::warning) << "Bridge daemon: invalid CarPlay " << name_
                               << " IPC frame size " << size << '\n';
        break;
      }
      std::vector<std::uint8_t> frame(size);
      if (!receive_all(client, frame, stop))
        break;
      if (callback_)
        callback_(frame);
    }
    {
      std::lock_guard lock(client_mutex_);
      if (client_ == client) {
        client_ = -1;
        close(client);
      }
    }
  }
}

FrameSocketWriter::FrameSocketWriter(const std::filesystem::path &path,
                                     std::string name)
    : path_(path), name_(std::move(name)),
      worker_([this](const std::stop_token stop) { write(stop); }) {}

FrameSocketWriter::~FrameSocketWriter() {
  worker_.request_stop();
  frames_ready_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

bool FrameSocketWriter::send(const std::span<const std::uint8_t> frame) {
  if (frame.empty() || frame.size() > kMaximumFrameSize ||
      frame.size() > kMaximumQueuedBytes)
    return false;
  {
    std::lock_guard lock(mutex_);
    if (frames_.size() >= kMaximumQueuedFrames ||
        queued_bytes_ + frame.size() > kMaximumQueuedBytes)
      return false;
    frames_.emplace_back(frame.begin(), frame.end());
    queued_bytes_ += frame.size();
  }
  frames_ready_.notify_one();
  return true;
}

void FrameSocketWriter::write(const std::stop_token stop) {
  int socket_fd = -1;
  while (!stop.stop_requested()) {
    std::vector<std::uint8_t> frame;
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
    const auto requeue = [&] {
      if (stop.stop_requested())
        return;
      std::lock_guard lock(mutex_);
      if (frames_.size() < kMaximumQueuedFrames &&
          queued_bytes_ + frame.size() <= kMaximumQueuedBytes) {
        queued_bytes_ += frame.size();
        frames_.push_front(std::move(frame));
        frames_ready_.notify_one();
      } else {
        log(LogLevel::warning) << "CarPlay worker: unable to requeue " << name_
                               << " IPC frame after a transport failure\n";
      }
    };
    if (socket_fd < 0) {
      socket_fd = connect_unix(path_);
      if (socket_fd < 0) {
        if (debug_logging_enabled())
          log(LogLevel::debug) << "CarPlay worker: unable to connect " << name_
                               << " IPC socket\n";
        requeue();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      if (debug_logging_enabled())
        log(LogLevel::debug)
            << "CarPlay worker: connected " << name_ << " IPC socket\n";
    }
    const auto size = frame.size();
    const std::array<std::uint8_t, 4> header{
        static_cast<std::uint8_t>(size >> 24),
        static_cast<std::uint8_t>(size >> 16),
        static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
    if (!send_all(socket_fd, header, stop) ||
        !send_all(socket_fd, frame, stop)) {
      close(socket_fd);
      socket_fd = -1;
      requeue();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (socket_fd >= 0)
    close(socket_fd);
}

} // namespace aa2acp::bridge
