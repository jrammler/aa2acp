#include "aa2acp/iap2/network_manager.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <vector>

extern char **environ;

namespace aa2acp::iap2 {
namespace {

bool run_nmcli(std::vector<std::string> arguments,
               const bool allow_inactive = false, const bool quiet = false) {
  std::vector<char *> argv;
  argv.reserve(arguments.size() + 1);
  for (auto &argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);
  pid_t child{};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_t *action_pointer = nullptr;
  if (quiet) {
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    action_pointer = &actions;
  }
  const auto result = posix_spawnp(&child, argv.front(), action_pointer,
                                   nullptr, argv.data(), environ);
  if (quiet) {
    posix_spawn_file_actions_destroy(&actions);
  }
  if (result != 0) {
    std::cerr << "Wi-Fi: unable to run nmcli (error " << result << ")\n";
    return false;
  }
  int status{};
  // nmcli exits 6 when disconnecting an interface that is already inactive.
  // Leaving the car AP must be idempotent, so that state is a success here.
  if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
      (WEXITSTATUS(status) != 0 &&
       !(allow_inactive && WEXITSTATUS(status) == 6))) {
    std::cerr << "Wi-Fi: NetworkManager connection command failed\n";
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
  // Reuse the saved AP profile first. It avoids a fresh scan race while test head unit is
  // bringing its AP up and is the desired fast-reconnect behaviour after a
  // completed session.
  if (run_nmcli({"nmcli", "--wait", "30", "connection", "up", "id",
                 configuration.ssid, "ifname", interface_name},
                false, true)) {
    return true;
  }
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

} // namespace aa2acp::iap2
