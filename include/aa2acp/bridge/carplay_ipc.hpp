#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace aa2acp::bridge {

// A bounded, length-prefixed UNIX stream used between the daemon and the
// isolated CarPlay worker. Frames are independent messages; the callback span
// is valid only for the duration of the callback.
class FrameSocketReceiver final {
public:
  using Callback = std::function<void(std::span<const std::uint8_t>)>;

  FrameSocketReceiver(const std::filesystem::path &path, std::string name,
                      Callback callback);
  ~FrameSocketReceiver();

  FrameSocketReceiver(const FrameSocketReceiver &) = delete;
  FrameSocketReceiver &operator=(const FrameSocketReceiver &) = delete;

  [[nodiscard]] bool ready() const { return listener_ >= 0; }

private:
  void receive(const std::stop_token stop);
  void shutdown_client();

  const std::filesystem::path path_;
  const std::string name_;
  const Callback callback_;
  int listener_{-1};
  std::atomic<int> client_{-1};
  std::jthread worker_;
};

// Worker-side writer. send() only copies/enqueues the frame and never waits
// for the daemon or the socket; this is required for AirPlay's event thread.
class FrameSocketWriter final {
public:
  FrameSocketWriter(const std::filesystem::path &path, std::string name);
  ~FrameSocketWriter();

  FrameSocketWriter(const FrameSocketWriter &) = delete;
  FrameSocketWriter &operator=(const FrameSocketWriter &) = delete;

  bool send(std::span<const std::uint8_t> frame);

private:
  void write(const std::stop_token stop);

  const std::filesystem::path path_;
  const std::string name_;
  std::mutex mutex_;
  std::condition_variable frames_ready_;
  std::deque<std::vector<std::uint8_t>> frames_;
  std::size_t queued_bytes_{};
  std::jthread worker_;
};

} // namespace aa2acp::bridge
