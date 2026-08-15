#pragma once

#include "aa2acp/airplay/crypto.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace aa2acp::airplay {

struct PairingRecord {
  std::string controller_id;
  Ed25519 controller;
  Bytes accessory_public_key;
};

// Stores the controller long-term key and the accessory LTPK with 0600
// permissions. The Pair-Verify phase can reuse this record after the first
// Pair-Setup exchange.
std::optional<PairingRecord>
load_pairing_record(const std::filesystem::path &path);
bool save_pairing_record(const std::filesystem::path &path,
                         const PairingRecord &record);

} // namespace aa2acp::airplay
