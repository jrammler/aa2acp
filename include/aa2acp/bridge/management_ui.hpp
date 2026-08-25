#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "aa2acp/bridge/bluez_inventory.hpp"
#include "aa2acp/bridge/config.hpp"
#include "aa2acp/iap2/bluetooth_worker.hpp"

namespace aa2acp::bridge::management {

// State shown by the management UI, updated by background workers.
struct Snapshot {
  std::vector<BluetoothDevice> bluetooth_devices;
  std::vector<std::string> wifi_interfaces;
  bool show_unnamed_bluetooth_devices{};
  bool bluetooth_scan_running{};
  int bluetooth_scan_phase_id{};
  std::string bluetooth_scan_phase{"idle"};
  std::string bluetooth_error;
  bool carplay_preflight_running{};
  // Bumped on every preflight state change (start, pairing prompt,
  // confirmation resolved, finished) so the UI can reload only on change.
  std::uint64_t carplay_preflight_phase_id{};
  std::string carplay_preflight_status;
  std::optional<iap2::PairingConfirmationMessage> pairing_confirmation;
  bool management_hotspot_password_pending{};
  std::string pending_management_hotspot_ssid;
};

std::string html_escape(const std::string &value);
std::string random_token();
std::string url_decode(const std::string &value);
std::optional<std::string> form_field(const std::string &body,
                                      const std::string &wanted);
std::optional<std::string> query_field(const std::string &request,
                                       const std::string &wanted);

bool send_response(int client, int status, const char *type,
                   const std::string &body, const std::string &extra = {});

std::vector<std::string> wifi_interfaces();

// Renders the management configuration page.
std::string render_page(const Config &config, const Snapshot &snapshot,
                        bool hotspot_needs_setup, bool saved,
                        bool carplay_error, const std::string &csrf_token);

// Renders the recent-logs page from a raw log tail.
std::string render_logs_page(const std::string &recent_logs,
                             std::size_t retained_kib);

} // namespace aa2acp::bridge::management
