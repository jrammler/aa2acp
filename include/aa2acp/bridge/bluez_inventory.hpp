#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aa2acp::bridge {

struct BluetoothDevice {
  std::string address;
  std::string name;
  bool paired{};
  bool connected{};
};

// Reads BlueZ's ObjectManager cache. This contains both bonded devices and
// devices discovered by an active/recent scan, so one source of truth is used
// for both lists.
std::vector<BluetoothDevice> list_bluez_devices(std::string *error = nullptr);
bool bluez_device_is_paired(std::string_view address);

// Removes a device and its bond from the local BlueZ adapter. The remote
// device's pairing state is outside BlueZ's control.
bool forget_bluez_device(std::string_view address,
                         std::string *error = nullptr);

// Populate BlueZ's cache with a bounded scan. BlueZ only permits one discovery
// transport at a time, so the caller may invoke this once for LE and once for
// BR/EDR. The function is intended to run on a background worker.
bool discover_bluez_devices(
    const std::string &transport, int seconds,
    const std::function<void(const std::string &)> &log);

} // namespace aa2acp::bridge
