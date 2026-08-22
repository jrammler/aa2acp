#include "aa2acp/bridge/config.hpp"

#include <fstream>
#include <sys/stat.h>

#include <cstdlib>

namespace aa2acp::bridge {
namespace {

constexpr char kMagic[] = "AA2ACP-1";

bool valid_key(const std::string_view key) {
  return key == "head_unit_mac" || key == "wifi_interface" ||
         key == "management_hotspot_ssid" ||
         key == "management_hotspot_passphrase" ||
         key == "airplay_pairing_store";
}

} // namespace

std::filesystem::path default_state_directory() {
  if (const char *state_home = std::getenv("XDG_STATE_HOME");
      state_home != nullptr && *state_home != '\0')
    return std::filesystem::path(state_home) / "aa2acp";
  if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0')
    return std::filesystem::path(home) / ".local" / "state" / "aa2acp";
  return std::filesystem::temp_directory_path() / "aa2acp";
}

std::filesystem::path default_config_path() {
  return default_state_directory() / "config";
}

std::filesystem::path default_airplay_pairing_store() {
  return default_state_directory() / "airplay-pairing.bin";
}

std::filesystem::path default_head_unit_capabilities_store() {
  return default_state_directory() / "head-unit-capabilities";
}

std::optional<Config> load_config(const std::filesystem::path &path) {
  std::ifstream stream(path);
  std::string line;
  if (!std::getline(stream, line) || line != kMagic)
    return std::nullopt;
  Config config;
  while (std::getline(stream, line)) {
    const auto separator = line.find('=');
    if (separator == std::string::npos || !valid_key(line.substr(0, separator)))
      return std::nullopt;
    const auto value = line.substr(separator + 1);
    if (line.starts_with("head_unit_mac="))
      config.head_unit_mac = value;
    else if (line.starts_with("wifi_interface="))
      config.wifi_interface = value;
    else if (line.starts_with("management_hotspot_ssid="))
      config.management_hotspot_ssid = value;
    else if (line.starts_with("management_hotspot_passphrase="))
      config.management_hotspot_passphrase = value;
    else
      config.airplay_pairing_store = value;
  }
  // Older configurations predate the management hotspot. The daemon migrates
  // them by generating and persisting credentials on its next start.
  if (config.wifi_interface.empty())
    return std::nullopt;
  return config;
}

bool save_config(const std::filesystem::path &path, const Config &config) {
  if (config.wifi_interface.empty() || config.management_hotspot_ssid.empty() ||
      config.management_hotspot_passphrase.size() < 8 ||
      config.head_unit_mac.find_first_of("\r\n=") != std::string::npos ||
      config.head_unit_mac.find('\0') != std::string::npos ||
      config.wifi_interface.find_first_of("\r\n=") != std::string::npos ||
      config.wifi_interface.find('\0') != std::string::npos ||
      config.management_hotspot_ssid.find_first_of("\r\n=") !=
          std::string::npos ||
      config.management_hotspot_ssid.find('\0') != std::string::npos ||
      config.management_hotspot_passphrase.find_first_of("\r\n=") !=
          std::string::npos ||
      config.management_hotspot_passphrase.find('\0') != std::string::npos ||
      config.airplay_pairing_store.string().find_first_of("\r\n=") !=
          std::string::npos ||
      config.airplay_pairing_store.string().find('\0') != std::string::npos)
    return false;
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error)
    return false;
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::trunc);
    if (!stream)
      return false;
    stream << kMagic << '\n'
           << "head_unit_mac=" << config.head_unit_mac << '\n'
           << "wifi_interface=" << config.wifi_interface << '\n'
           << "management_hotspot_ssid=" << config.management_hotspot_ssid
           << '\n'
           << "management_hotspot_passphrase="
           << config.management_hotspot_passphrase << '\n';
    if (!config.airplay_pairing_store.empty())
      stream << "airplay_pairing_store="
             << config.airplay_pairing_store.string() << '\n';
    if (!stream)
      return false;
  }
  return chmod(temporary.c_str(), S_IRUSR | S_IWUSR) == 0 &&
         std::rename(temporary.c_str(), path.c_str()) == 0;
}

} // namespace aa2acp::bridge
