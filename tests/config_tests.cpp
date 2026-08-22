#include "aa2acp/bridge/config.hpp"

#include <cassert>
#include <filesystem>

int main() {
  const auto path = std::filesystem::temp_directory_path() / "acp-bridge.conf";
  const aa2acp::bridge::Config expected{"head-unit-address", "wlan0",
                                        "AA2ACP-test", "test-password",
                                        "/var/lib/aa2acp/airplay.bin"};
  assert(aa2acp::bridge::save_config(path, expected));
  const auto restored = aa2acp::bridge::load_config(path);
  assert(restored && restored->head_unit_mac == expected.head_unit_mac &&
         restored->wifi_interface == expected.wifi_interface &&
         restored->management_hotspot_ssid ==
             expected.management_hotspot_ssid &&
         restored->management_hotspot_passphrase ==
             expected.management_hotspot_passphrase &&
         restored->airplay_pairing_store == expected.airplay_pairing_store);
  auto malformed = expected;
  malformed.management_hotspot_ssid += '\0';
  malformed.management_hotspot_ssid += "suffix";
  assert(!aa2acp::bridge::save_config(path, malformed));
  std::filesystem::remove(path);
  assert(aa2acp::bridge::default_config_path().filename() == "config");
  assert(aa2acp::bridge::default_airplay_pairing_store().filename() ==
         "airplay-pairing.bin");
  assert(aa2acp::bridge::default_head_unit_capabilities_store().filename() ==
         "head-unit-capabilities");
}
