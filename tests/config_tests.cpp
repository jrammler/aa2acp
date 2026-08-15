#include "acp/bridge/config.hpp"

#include <cassert>
#include <filesystem>

int main() {
  const auto path = std::filesystem::temp_directory_path() / "acp-bridge.conf";
  const acp::bridge::Config expected{"[redacted-device-address]", "wlan0",
                                     "/var/lib/acp-aa/airplay.bin"};
  assert(acp::bridge::save_config(path, expected));
  const auto restored = acp::bridge::load_config(path);
  assert(restored && restored->head_unit_mac == expected.head_unit_mac &&
         restored->wifi_interface == expected.wifi_interface &&
         restored->airplay_pairing_store == expected.airplay_pairing_store);
  std::filesystem::remove(path);
}
