#pragma once

#include <cstdint>
#include <optional>
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
// Returns the accessory endpoint for a Wi-Fi handoff. Prefer an advertised
// default gateway; some head units omit it, so fall back to the subnet's first
// host until mDNS endpoint discovery is available.
std::optional<std::string>
accessory_ipv4_endpoint_for_interface(const std::string &interface_name);
bool leave_with_networkmanager(const std::string &interface_name);

// Keep the local management UI reachable while CarPlay is idle. The AP profile
// is deterministic so it can be recreated safely after an interrupted session.
bool start_management_hotspot(const std::string &interface_name,
                              const std::string &ssid,
                              const std::string &passphrase);
bool stop_management_hotspot(const std::string &interface_name);

} // namespace aa2acp::iap2
