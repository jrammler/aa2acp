#pragma once

#include "aa2acp/bridge/logging.hpp"

#include <functional>
#include <cstdint>
#include <optional>
#include <string_view>

namespace aa2acp::iap2 {

using PairingLogFunction =
    std::function<void(aa2acp::bridge::LogLevel, const std::string &)>;

// Ensures BlueZ knows, pairs, and trusts a device using a DisplayYesNo agent.
// Numeric-comparison requests are accepted automatically. It persists the bond
// in BlueZ; it does not open iAP2.
// Queries the remote device's SDP records for its Serial Port service and
// returns the RFCOMM channel it listens on.
std::optional<std::uint8_t> discover_spp_channel(std::string_view mac);

bool ensure_bluez_pairing(std::string_view mac, int timeout_seconds,
                          const PairingLogFunction &log = {});

} // namespace aa2acp::iap2
