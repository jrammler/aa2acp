#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace aa2acp::bridge {

// Configuration deliberately contains references to OS-owned credentials only:
// BlueZ owns the BT bond and NetworkManager owns the Wi-Fi secret/profile.
struct Config {
  std::string head_unit_mac;
  std::string wifi_interface;
  // The same radio hosts this WPA2 management AP while the bridge is idle.
  std::string management_hotspot_ssid;
  std::string management_hotspot_passphrase;
  // Empty selects the daemon's deterministic state-directory default. An
  // override must remain under that directory so it is writable by the
  // restricted daemon service.
  std::filesystem::path airplay_pairing_store;
};

std::filesystem::path default_state_directory();
std::filesystem::path default_config_path();
std::filesystem::path default_airplay_pairing_store();
std::filesystem::path default_head_unit_capabilities_store();
// Returns false for malformed serialized values or incomplete required
// settings. Legacy files may omit both management-hotspot fields.
bool validate_config(const Config &config, bool allow_legacy_hotspot = false);
std::optional<Config> load_config(const std::filesystem::path &path);
bool save_config(const std::filesystem::path &path, const Config &config);

} // namespace aa2acp::bridge
