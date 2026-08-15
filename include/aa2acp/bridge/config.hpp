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
  // Empty selects the daemon's deterministic state-directory default. This is
  // intentionally an advanced override, not a normal UI setting.
  std::filesystem::path airplay_pairing_store;
};

std::filesystem::path default_state_directory();
std::filesystem::path default_config_path();
std::filesystem::path default_airplay_pairing_store();
std::filesystem::path default_display_profile_store();
std::optional<Config> load_config(const std::filesystem::path &path);
bool save_config(const std::filesystem::path &path, const Config &config);

} // namespace aa2acp::bridge
