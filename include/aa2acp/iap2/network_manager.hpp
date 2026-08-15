#pragma once

#include <cstdint>
#include <string>

namespace aa2acp::iap2 {

struct AccessoryWifiConfiguration {
  std::string ssid;
  std::string passphrase;
  std::uint8_t security_type{};
  std::uint8_t channel{};
};

// Development/test backend. The production appliance will replace this with
// its own network manager, but nmcli lets us validate the iAP2 handover flow
// without changing Ethernet connectivity on the development host.
bool join_with_networkmanager(const AccessoryWifiConfiguration &configuration,
                              const std::string &interface_name);
bool leave_with_networkmanager(const std::string &interface_name);

} // namespace aa2acp::iap2
