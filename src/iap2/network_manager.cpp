#include "aa2acp/iap2/network_manager.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace aa2acp::iap2 {
namespace {

constexpr char kManagementProfile[] = "aa2acp-management";

std::optional<std::string> ipv4_address(const std::string &interface_name) {
  ifaddrs *addresses{};
  if (getifaddrs(&addresses) != 0)
    return std::nullopt;
  std::optional<std::string> address;
  for (auto *entry = addresses; entry != nullptr; entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET ||
        interface_name != entry->ifa_name)
      continue;
    std::array<char, INET_ADDRSTRLEN> text{};
    const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
    if (inet_ntop(AF_INET, &ipv4->sin_addr, text.data(), text.size()) !=
        nullptr) {
      address = text.data();
      break;
    }
  }
  freeifaddrs(addresses);
  return address;
}

bool run_nmcli(std::vector<std::string> arguments,
               const bool allow_inactive = false, const bool quiet = false) {
  const auto started = std::chrono::steady_clock::now();
  std::vector<char *> argv;
  argv.reserve(arguments.size() + 1);
  for (auto &argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);
  pid_t child{};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_t *action_pointer = nullptr;
  int output_pipe[2]{-1, -1};
  if (quiet) {
    if (pipe2(output_pipe, O_CLOEXEC) != 0) {
      std::cerr << "Wi-Fi: unable to capture nmcli diagnostics\n";
      return false;
    }
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
    action_pointer = &actions;
  }
  const auto result = posix_spawnp(&child, argv.front(), action_pointer,
                                   nullptr, argv.data(), environ);
  if (quiet) {
    posix_spawn_file_actions_destroy(&actions);
    close(output_pipe[1]);
  }
  if (result != 0) {
    if (quiet)
      close(output_pipe[0]);
    std::cerr << "Wi-Fi: unable to run nmcli (error " << result << ")\n";
    return false;
  }
  int status{};
  // nmcli exits 6 when disconnecting an interface that is already inactive.
  // Leaving the car AP must be idempotent, so that state is a success here.
  const auto waited = waitpid(child, &status, 0);
  std::string diagnostics;
  if (quiet) {
    std::array<char, 4096> buffer{};
    for (;;) {
      const auto count = read(output_pipe[0], buffer.data(), buffer.size());
      if (count <= 0)
        break;
      diagnostics.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(output_pipe[0]);
  }
  const bool success = waited >= 0 && WIFEXITED(status) &&
                       (WEXITSTATUS(status) == 0 ||
                        (allow_inactive && WEXITSTATUS(status) == 6));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  if (success && elapsed >= 1000)
    std::cout << "Wi-Fi: NetworkManager command succeeded after " << elapsed
              << " ms\n";
  if (!success) {
    std::cerr << "Wi-Fi: NetworkManager command failed (exit "
              << (waited >= 0 && WIFEXITED(status)
                      ? std::to_string(WEXITSTATUS(status))
                      : "abnormal termination")
              << ", after " << elapsed << " ms)\n";
    if (!diagnostics.empty())
      std::cerr << "Wi-Fi: nmcli: " << diagnostics;
    return false;
  }
  return true;
}

} // namespace

bool join_with_networkmanager(const AccessoryWifiConfiguration &configuration,
                              const std::string &interface_name) {
  if (configuration.ssid.empty()) {
    std::cerr << "Wi-Fi: accessory sent an empty SSID\n";
    return false;
  }
  std::cout << "Wi-Fi: joining SSID '" << configuration.ssid << "' on "
            << interface_name << " (channel "
            << static_cast<int>(configuration.channel) << ")\n";
  // Reuse the saved AP profile first. It avoids a fresh scan race while the
  // head unit is bringing its AP up and is the desired fast-reconnect behaviour
  // after a completed session.
  std::cout << "Wi-Fi: checking for saved NetworkManager profile for '"
            << configuration.ssid << "'\n";
  if (run_nmcli({"nmcli", "--wait", "30", "connection", "up", "id",
                 configuration.ssid, "ifname", interface_name},
                false, true)) {
    return true;
  }
  std::cout << "Wi-Fi: no usable saved profile; requesting a NetworkManager "
               "Wi-Fi rescan\n";
  run_nmcli({"nmcli", "--wait", "10", "device", "wifi", "rescan", "ifname",
             interface_name},
            false, true);
  std::cout << "Wi-Fi: waiting for accessory SSID to appear after rescan\n";
  std::this_thread::sleep_for(std::chrono::seconds(2));
  std::cout << "Wi-Fi: trying a fresh connection (NetworkManager may wait up "
               "to 30 seconds)\n";
  std::vector<std::string> arguments{
      "nmcli", "--wait", "30", "device", "wifi", "connect", configuration.ssid};
  if (!configuration.passphrase.empty()) {
    arguments.emplace_back("password");
    arguments.push_back(configuration.passphrase);
  }
  arguments.emplace_back("ifname");
  arguments.push_back(interface_name);
  if (run_nmcli(std::move(arguments), false, true)) {
    return true;
  }
  std::cerr << "Wi-Fi: fresh NetworkManager connection failed; attempting "
               "security-profile repair\n";
  // NetworkManager sometimes creates a profile before refusing the initial
  // connect because WPA parameters were implicit. Repair it using the iAP2
  // security type, then bring the profile up explicitly.
  std::string key_management;
  switch (configuration.security_type) {
  case 0:
    key_management = "none";
    break;
  case 1:
    key_management = "wep";
    break;
  case 2:
  case 3:
    key_management = "wpa-psk";
    break;
  case 4:
    key_management = "sae";
    break;
  default:
    std::cerr << "Wi-Fi: unsupported security type "
              << static_cast<int>(configuration.security_type) << '\n';
    return false;
  }
  std::vector<std::string> modify{"nmcli",
                                  "connection",
                                  "modify",
                                  configuration.ssid,
                                  "802-11-wireless-security.key-mgmt",
                                  key_management};
  if (configuration.security_type == 2 || configuration.security_type == 3) {
    modify.emplace_back("802-11-wireless-security.proto");
    modify.emplace_back("rsn");
  }
  if (!configuration.passphrase.empty()) {
    modify.emplace_back("wifi-sec.psk");
    modify.push_back(configuration.passphrase);
  }
  return run_nmcli(std::move(modify), false, true) &&
         run_nmcli({"nmcli", "--wait", "30", "connection", "up", "id",
                    configuration.ssid, "ifname", interface_name},
                   false, true);
}

bool leave_with_networkmanager(const std::string &interface_name) {
  std::cout << "Wi-Fi: disconnecting " << interface_name
            << " while retaining its saved profile\n";
  return run_nmcli({"nmcli", "device", "disconnect", interface_name}, true,
                   true);
}

bool start_management_hotspot(const std::string &interface_name,
                              const std::string &ssid,
                              const std::string &passphrase) {
  if (interface_name.empty() || ssid.empty() || passphrase.size() < 8) {
    std::cerr << "Wi-Fi: invalid management hotspot configuration\n";
    return false;
  }
  std::cout << "Wi-Fi: starting management hotspot '" << ssid << "' on "
            << interface_name << '\n';
  // Updating is idempotent. A failed modify means this is the first launch.
  if (!run_nmcli({"nmcli",
                  "connection",
                  "modify",
                  kManagementProfile,
                  "connection.interface-name",
                  interface_name,
                  "connection.autoconnect",
                  "yes",
                  "connection.autoconnect-priority",
                  "100",
                  "802-11-wireless.mode",
                  "ap",
                  "802-11-wireless.ssid",
                  ssid,
                  "ipv4.method",
                  "shared",
                  "ipv6.method",
                  "disabled",
                  "802-11-wireless-security.key-mgmt",
                  "wpa-psk",
                  "802-11-wireless-security.psk",
                  passphrase},
                 false, true) &&
      !run_nmcli({"nmcli",        "connection",   "add",
                  "type",         "wifi",         "ifname",
                  interface_name, "con-name",     kManagementProfile,
                  "ssid",         ssid,           "802-11-wireless.mode",
                  "ap",           "ipv4.method",  "shared",
                  "ipv6.method",  "disabled",     "wifi-sec.key-mgmt",
                  "wpa-psk",      "wifi-sec.psk", passphrase},
                 false, true))
    return false;
  if (!run_nmcli({"nmcli", "--wait", "30", "connection", "up", "id",
                  kManagementProfile, "ifname", interface_name},
                 false, true))
    return false;
  if (const auto address = ipv4_address(interface_name))
    std::cout << "Wi-Fi: management hotspot gateway is " << *address << '\n';
  return true;
}

bool stop_management_hotspot(const std::string &interface_name) {
  std::cout << "Wi-Fi: stopping management hotspot on " << interface_name
            << '\n';
  return run_nmcli({"nmcli", "connection", "down", "id", kManagementProfile,
                    "ifname", interface_name},
                   true, true);
}

} // namespace aa2acp::iap2
