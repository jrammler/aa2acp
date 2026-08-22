#pragma once

#include "aa2acp/bridge/logging.hpp"

#include <functional>
#include <string_view>

namespace aa2acp::iap2 {

using PairingLogFunction =
    std::function<void(aa2acp::bridge::LogLevel, const std::string &)>;

// Ensures BlueZ knows, pairs, and trusts a device using a DisplayYesNo agent.
// Numeric-comparison requests are accepted automatically. It persists the bond
// in BlueZ; it does not open iAP2.
bool ensure_bluez_pairing(std::string_view mac, int timeout_seconds,
                          const PairingLogFunction &log = {});

} // namespace aa2acp::iap2
