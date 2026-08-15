#pragma once

#include "aa2acp/airplay/rtsp.hpp"

#include <cstdint>
#include <optional>

namespace aa2acp::airplay {

// AirPlay control messages after Pair-Verify: LE length, encrypted RTSP, tag.
class ControlCipher {
public:
  ControlCipher(Bytes read_key, Bytes write_key);

  std::optional<Bytes> encrypt(std::span<const std::uint8_t> plaintext);
  std::optional<Bytes> decrypt(std::span<const std::uint8_t> frame);
  std::optional<Bytes> decrypt_one(Bytes &buffer);

private:
  Bytes read_key_;
  Bytes write_key_;
  std::uint64_t read_counter_{};
  std::uint64_t write_counter_{};
};

} // namespace aa2acp::airplay
