#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace acp::airplay {

// Performs one authenticated AirPlay control and optional H.264 screen session.
// Returns zero only after the encrypted RECORD phase (and optional video)
// succeeds.
struct SessionOptions {
  std::string host{"10.10.0.1"};
  std::uint16_t port{7000};
  int timeout_seconds{10};
  std::string video_path;
  std::string pairing_store;
  std::function<bool()> stop_requested;
};

int run_session(const SessionOptions &options);

} // namespace acp::airplay
