#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace aa2acp::airplay {

using Bytes = std::vector<std::uint8_t>;

// RFC 5054 3072-bit, SHA-512 SRP-6a for AirPlay Pair-Setup. The caller sends
// public_key()/client_proof() in M3, then validates M4 with verify_server().
class SrpClient {
public:
  bool process_challenge(std::span<const std::uint8_t> salt,
                         std::span<const std::uint8_t> server_public_key);
  [[nodiscard]] const Bytes &public_key() const { return public_key_; }
  [[nodiscard]] const Bytes &client_proof() const { return client_proof_; }
  [[nodiscard]] const Bytes &session_key() const { return session_key_; }
  [[nodiscard]] bool
  verify_server(std::span<const std::uint8_t> server_proof) const;

private:
  Bytes public_key_;
  Bytes client_proof_;
  Bytes session_key_;
};

} // namespace aa2acp::airplay
