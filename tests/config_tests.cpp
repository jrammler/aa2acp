#include "aa2acp/bridge/config.hpp"

#include <cassert>
#include <filesystem>

int main() {
  const auto path = std::filesystem::temp_directory_path() / "acp-bridge.conf";
  const aa2acp::bridge::Config expected{"[redacted-device-address]", "wlan0",
                                        "/var/lib/aa2acp/airplay.bin"};
  assert(aa2acp::bridge::save_config(path, expected));
  const auto restored = aa2acp::bridge::load_config(path);
  assert(restored && restored->head_unit_mac == expected.head_unit_mac &&
         restored->wifi_interface == expected.wifi_interface &&
         restored->airplay_pairing_store == expected.airplay_pairing_store);
  std::filesystem::remove(path);
  assert(aa2acp::bridge::default_config_path().filename() == "config");
  assert(aa2acp::bridge::default_airplay_pairing_store().filename() ==
         "airplay-pairing.bin");
}
