#include "aa2acp/airplay/control_cipher.hpp"

#include "aa2acp/airplay/crypto.hpp"

#include <array>

namespace aa2acp::airplay {
namespace {

std::array<std::uint8_t, 12> nonce_for_counter(const std::uint64_t counter) {
  std::array<std::uint8_t, 12> nonce{};
  for (std::size_t index = 0; index < 8; ++index) {
    nonce[4 + index] = static_cast<std::uint8_t>(counter >> (index * 8));
  }
  return nonce;
}

} // namespace

ControlCipher::ControlCipher(Bytes read_key, Bytes write_key)
    : read_key_(std::move(read_key)), write_key_(std::move(write_key)) {}

std::optional<Bytes>
ControlCipher::encrypt(const std::span<const std::uint8_t> plaintext) {
  if (plaintext.size() > UINT16_MAX)
    return std::nullopt;
  const Bytes header{static_cast<std::uint8_t>(plaintext.size()),
                     static_cast<std::uint8_t>(plaintext.size() >> 8)};
  const auto encrypted = seal_with_nonce(
      write_key_, nonce_for_counter(write_counter_), plaintext, header);
  if (!encrypted)
    return std::nullopt;
  ++write_counter_;
  Bytes frame(header);
  frame.insert(frame.end(), encrypted->begin(), encrypted->end());
  return frame;
}

std::optional<Bytes>
ControlCipher::decrypt(const std::span<const std::uint8_t> frame) {
  if (frame.size() < 18)
    return std::nullopt;
  const auto length = static_cast<std::size_t>(frame[0]) |
                      (static_cast<std::size_t>(frame[1]) << 8);
  if (frame.size() != length + 18)
    return std::nullopt;
  const auto plaintext =
      open_with_nonce(read_key_, nonce_for_counter(read_counter_),
                      frame.subspan(2), frame.first(2));
  if (!plaintext)
    return std::nullopt;
  ++read_counter_;
  return plaintext;
}

std::optional<Bytes> ControlCipher::decrypt_one(Bytes &buffer) {
  if (buffer.size() < 2)
    return Bytes{};
  const auto length = static_cast<std::size_t>(buffer[0]) |
                      (static_cast<std::size_t>(buffer[1]) << 8);
  const auto frame_size = length + 18;
  if (buffer.size() < frame_size)
    return Bytes{};
  const auto plaintext = decrypt(std::span(buffer.data(), frame_size));
  if (!plaintext)
    return std::nullopt;
  buffer.erase(buffer.begin(),
               buffer.begin() + static_cast<std::ptrdiff_t>(frame_size));
  return plaintext;
}

} // namespace aa2acp::airplay
