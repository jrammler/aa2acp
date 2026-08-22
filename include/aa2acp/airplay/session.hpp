#pragma once

#include <cstdint>
#include <filesystem>
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
  // Supplies one 48 kHz stereo S16LE Android Auto media-audio packet at a
  // time. Returning std::nullopt ends the live stream.
  std::function<std::optional<std::vector<std::uint8_t>>()> next_media_audio;
  std::function<std::optional<std::vector<std::uint8_t>>()> next_guidance_audio;
  std::function<std::optional<std::vector<std::uint8_t>>()> next_system_audio;
  std::string pairing_store;
  std::filesystem::path head_unit_capabilities_store;
  std::string head_unit_mac;
  std::function<bool()> stop_requested;
  // Called before the audio sender threads are joined, so a callback blocked
  // waiting for Android Auto data can be interrupted during teardown.
  std::function<void()> stop_streams;
};

int run_session(const SessionOptions &options);

} // namespace aa2acp::airplay
