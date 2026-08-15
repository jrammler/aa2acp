#pragma once
#include "aa2acp/airplay/rtsp.hpp"
#include <optional>
#include <string_view>
namespace aa2acp::airplay {
Bytes hkdf_sha512(std::span<const std::uint8_t> ikm, std::string_view salt,
                  std::string_view info, std::size_t length);
std::optional<Bytes> seal_with_nonce(std::span<const std::uint8_t> key,
                                     std::span<const std::uint8_t> nonce,
                                     std::span<const std::uint8_t> plain,
                                     std::span<const std::uint8_t> aad = {});
std::optional<Bytes> open_with_nonce(std::span<const std::uint8_t> key,
                                     std::span<const std::uint8_t> nonce,
                                     std::span<const std::uint8_t> cipher,
                                     std::span<const std::uint8_t> aad = {});
std::optional<Bytes> seal(std::span<const std::uint8_t> key,
                          std::string_view label,
                          std::span<const std::uint8_t> plain);
std::optional<Bytes> open(std::span<const std::uint8_t> key,
                          std::string_view label,
                          std::span<const std::uint8_t> cipher);
struct Ed25519 {
  Bytes private_key;
  Bytes public_key;
};
struct X25519 {
  Bytes private_key;
  Bytes public_key;
};
std::optional<Ed25519> ed25519_generate();
std::optional<Bytes> ed25519_sign(std::span<const std::uint8_t> private_key,
                                  std::span<const std::uint8_t> data);
bool ed25519_verify(std::span<const std::uint8_t> public_key,
                    std::span<const std::uint8_t> data,
                    std::span<const std::uint8_t> signature);
std::optional<X25519> x25519_generate();
std::optional<Bytes> x25519_shared(std::span<const std::uint8_t> private_key,
                                   std::span<const std::uint8_t> public_key);
} // namespace aa2acp::airplay
