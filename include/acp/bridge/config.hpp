#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace acp::bridge {

// Configuration deliberately contains references to OS-owned credentials only:
// BlueZ owns the BT bond and NetworkManager owns the Wi-Fi secret/profile.
struct Config {
  std::string head_unit_mac;
  std::string wifi_interface{"wlan0"};
  std::filesystem::path airplay_pairing_store;
};

std::optional<Config> load_config(const std::filesystem::path &path);
bool save_config(const std::filesystem::path &path, const Config &config);

} // namespace acp::bridge
