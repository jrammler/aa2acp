#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace aa2acp::bridge {

// Common UNIX-socket frame server: accepts a single consumer and forwards
// length-prefixed frames from a bounded queue without blocking producers.
class MediaSocketForwarder {
public:
  ~MediaSocketForwarder();

  MediaSocketForwarder(const MediaSocketForwarder &) = delete;
  MediaSocketForwarder &operator=(const MediaSocketForwarder &) = delete;

  bool ready() const { return listener_ >= 0; }

protected:
  using Bytes = std::vector<std::uint8_t>;

  static constexpr std::size_t kMaximumQueuedFrames = 600;

  MediaSocketForwarder(const std::filesystem::path &path, std::string name);

  // Queues a frame for the consumer, dropping the oldest frame when full.
  void enqueue(Bytes frame);

  std::size_t maximum_queued_frames_{kMaximumQueuedFrames};

  // Queue state shared with the forwarding thread.
  std::mutex mutex_;
  std::condition_variable frames_ready_;
  std::deque<Bytes> frames_;

  Bytes keyframe_;

  static bool send_frame(int socket_fd, const Bytes &frame);

  const std::string name_;

private:
  // Called on the forwarding thread after a client connects. Return false to
  // disconnect the client immediately.
  virtual bool on_client_connected(int client);

  // Called on the forwarding thread after a client connects.
  virtual void log_client_connected();

  static bool send_all(int socket_fd,
                       const std::span<const std::uint8_t> bytes);

  void forward(const std::stop_token stop);
  void close_listener();
  void shutdown_client();

  int listener_{-1};
  std::atomic<int> client_{-1};
  std::mutex client_mutex_;
  std::jthread worker_;

  const std::filesystem::path path_;
};

// Forwards Android Auto H.264 access units, retaining the latest SPS/PPS and
// keyframe sequence so consumers joining mid-stream can start decoding.
class VideoSocketForwarder final : public MediaSocketForwarder {
public:
  explicit VideoSocketForwarder(const std::filesystem::path &path);

  void push(const std::span<const std::uint8_t> access_unit);

private:
  static std::vector<Bytes> nalus(const Bytes &input);
  bool on_client_connected(int client) override;
  void log_client_connected() override;

  std::size_t received_video_count_{};
  Bytes sps_;
  Bytes pps_;
  std::ofstream dump_;
};

// Moves PCM off AASDK's I/O thread without allowing a slow CarPlay consumer
// to block Android Auto acknowledgements.
class AudioSocketForwarder final : public MediaSocketForwarder {
public:
  AudioSocketForwarder(const std::filesystem::path &path, std::string name);

  void push(const std::span<const std::uint8_t> pcm);

private:
  void log_client_connected() override;
  std::size_t received_packets_{};
};

} // namespace aa2acp::bridge
