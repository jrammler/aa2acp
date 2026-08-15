#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aa2acp::airplay {

// Performs one authenticated AirPlay control and optional H.264 screen session.
// Returns zero only after the encrypted RECORD phase (and optional video)
// succeeds.
struct SessionOptions {
  std::string host{"10.10.0.1"};
  std::uint16_t port{7000};
  int timeout_seconds{10};
  std::string video_path;
  // Supplies one Annex-B H.264 access unit at a time. Returning std::nullopt
  // ends the live stream. The callback may block while waiting for a frame.
  std::function<std::optional<std::vector<std::uint8_t>>()> next_video_frame;
  std::string pairing_store;
  std::function<bool()> stop_requested;
};

int run_session(const SessionOptions &options);

} // namespace aa2acp::airplay
