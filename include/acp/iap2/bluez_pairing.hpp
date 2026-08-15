#pragma once

#include <functional>
#include <string_view>

namespace acp::iap2 {

using PairingLogFunction = std::function<void(const std::string &)>;

// Ensures BlueZ knows, pairs, and trusts a device using a Just-Works
// NoInputNoOutput agent. It persists the bond in BlueZ; it does not open iAP2.
bool ensure_bluez_pairing(std::string_view mac, int timeout_seconds,
                          const PairingLogFunction &log = {});

} // namespace acp::iap2
