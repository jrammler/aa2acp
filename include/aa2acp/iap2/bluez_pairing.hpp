#pragma once

#include "aa2acp/bridge/logging.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace aa2acp::iap2 {

using PairingLogFunction =
    std::function<void(aa2acp::bridge::LogLevel, const std::string &)>;

// Ensures BlueZ knows, pairs, and trusts a device using a DisplayYesNo agent.
// Numeric-comparison requests are accepted automatically. It persists the bond
// in BlueZ; it does not open iAP2.
struct DiscoveredEndpoint {
  // At most one of these is set after a successful discovery.
  std::optional<std::uint8_t> rfcomm_channel;  // classic SPP/iAP2 over RFCOMM
  std::optional<std::uint16_t> l2cap_psm;      // iAP2 over L2CAP
};

// Queries the remote device's SDP records for an iAP2/CarPlay accessory
// service (Apple vendor UUID) or, failing that, generic SPP, and returns the
// endpoint it listens on.
DiscoveredEndpoint discover_endpoint(std::string_view mac);

bool ensure_bluez_pairing(std::string_view mac, int timeout_seconds,
                          const PairingLogFunction &log = {});

} // namespace aa2acp::iap2
