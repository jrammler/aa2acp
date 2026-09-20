#include "aa2acp/iap2/network_manager.hpp"
#include "aa2acp/bridge/logging.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace aa2acp::iap2 {
namespace {

constexpr char kManagementProfile[] = "aa2acp-management";
constexpr auto kNmcliDeadline = std::chrono::seconds(60);
std::mutex network_manager_mutex;

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
               const bool allow_inactive = false, const bool quiet = false,
               const std::string &stdin_data = {}) {
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
  int input_pipe[2]{-1, -1};
  const bool has_stdin = !stdin_data.empty();
  if (quiet && pipe2(output_pipe, O_CLOEXEC) != 0) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Wi-Fi: unable to capture nmcli diagnostics\n";
    return false;
  }
  if (has_stdin && pipe2(input_pipe, O_CLOEXEC) != 0) {
    if (quiet) {
      close(output_pipe[0]);
      close(output_pipe[1]);
    }
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Wi-Fi: unable to provide nmcli secrets\n";
    return false;
  }
  if (quiet || has_stdin) {
    posix_spawn_file_actions_init(&actions);
    if (quiet) {
      posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
      posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
      posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
      posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
    }
    if (has_stdin) {
      posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO);
      posix_spawn_file_actions_addclose(&actions, input_pipe[0]);
      posix_spawn_file_actions_addclose(&actions, input_pipe[1]);
    }
    action_pointer = &actions;
  }
  const auto result = posix_spawnp(&child, argv.front(), action_pointer,
                                   nullptr, argv.data(), environ);
  if (quiet || has_stdin)
    posix_spawn_file_actions_destroy(&actions);
  if (quiet) {
    close(output_pipe[1]);
    const auto flags = fcntl(output_pipe[0], F_GETFL);
    if (flags >= 0)
      fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
  }
  if (has_stdin)
    close(input_pipe[0]);
  if (result != 0) {
    if (quiet)
      close(output_pipe[0]);
    if (has_stdin)
      close(input_pipe[1]);
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Wi-Fi: unable to run nmcli (error " << result << ")\n";
    return false;
  }
  if (has_stdin) {
    std::size_t offset{};
    while (offset < stdin_data.size()) {
      const auto count = write(input_pipe[1], stdin_data.data() + offset,
                               stdin_data.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
        continue;
      }
      if (count < 0 && errno == EINTR)
        continue;
      close(input_pipe[1]);
      (void)kill(child, SIGTERM);
      for (;;) {
        const auto waited = waitpid(child, nullptr, 0);
        if (waited >= 0 || errno != EINTR)
          break;
      }
      if (quiet)
        close(output_pipe[0]);
      return false;
    }
    close(input_pipe[1]);
  }
  int status{};
  // nmcli exits 6 when disconnecting an interface that is already inactive.
  // Leaving the car AP must be idempotent, so that state is a success here.
  std::string diagnostics;
  pid_t waited{};
  bool timed_out = false;
  const auto drain_diagnostics = [&] {
    if (!quiet)
      return;
    std::array<char, 4096> buffer{};
    for (;;) {
      const auto count = read(output_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        diagnostics.append(buffer.data(), static_cast<std::size_t>(count));
        continue;
      }
      break;
    }
  };
  const auto terminate_and_reap = [&] {
    (void)kill(child, SIGTERM);
    const auto terminate_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < terminate_deadline) {
      waited = waitpid(child, &status, WNOHANG);
      if (waited == child)
        return;
      if (waited < 0 && errno != EINTR)
        break;
      if (quiet) {
        drain_diagnostics();
        pollfd descriptor{output_pipe[0], POLLIN, 0};
        (void)poll(&descriptor, 1, 50);
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
    (void)kill(child, SIGKILL);
    do {
      waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
  };
  for (;;) {
    drain_diagnostics();
    waited = waitpid(child, &status, WNOHANG);
    if (waited < 0 && errno == EINTR)
      continue;
    if (waited < 0) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Wi-Fi: waitpid failed for nmcli; terminating child\n";
      terminate_and_reap();
      break;
    }
    if (waited != 0)
      break;
    if (std::chrono::steady_clock::now() - started >= kNmcliDeadline) {
      timed_out = true;
      terminate_and_reap();
      break;
    }
    if (quiet) {
      pollfd descriptor{output_pipe[0], POLLIN, 0};
      poll(&descriptor, 1, 50);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  if (quiet) {
    drain_diagnostics();
    close(output_pipe[0]);
  }
  const bool success = waited >= 0 && WIFEXITED(status) &&
                       (WEXITSTATUS(status) == 0 ||
                        (allow_inactive && WEXITSTATUS(status) == 6));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  if (success && elapsed >= 1000)
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
        << "Wi-Fi: NetworkManager command succeeded after " << elapsed
        << " ms\n";
  if (!success) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
        << "Wi-Fi: NetworkManager command "
        << (timed_out ? "timed out" : "failed") << " (exit "
        << (waited >= 0 && WIFEXITED(status)
                ? std::to_string(WEXITSTATUS(status))
                : "abnormal termination")
        << ", after " << elapsed << " ms)\n";
    if (!diagnostics.empty())
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Wi-Fi: nmcli: " << diagnostics;
    return false;
  }
  return true;
}

} // namespace

bool join_with_networkmanager(const AccessoryWifiConfiguration &configuration,
                              const std::string &interface_name) {
  std::lock_guard lock(network_manager_mutex);
  if (configuration.ssid.empty()) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Wi-Fi: accessory sent an empty SSID\n";
    return false;
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Wi-Fi: joining configured accessory SSID on " << interface_name
      << " (channel " << static_cast<int>(configuration.channel) << ")\n";
  // Reuse the saved AP profile first. It avoids a fresh scan race while the
  // head unit is bringing its AP up and is the desired fast-reconnect behaviour
  // after a completed session.
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Wi-Fi: checking for a saved NetworkManager accessory profile\n";
  if (run_nmcli({"nmcli", "--wait", "30", "connection", "up", "id",
                 configuration.ssid, "ifname", interface_name},
                false, true)) {
    return true;
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
      << "Wi-Fi: no usable saved profile; requesting a NetworkManager Wi-Fi "
         "rescan\n";
  run_nmcli({"nmcli", "--wait", "10", "device", "wifi", "rescan", "ifname",
             interface_name},
            false, true);
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Wi-Fi: waiting for accessory SSID to appear after rescan\n";
  std::this_thread::sleep_for(std::chrono::seconds(2));
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Wi-Fi: preparing a fresh NetworkManager profile\n";
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
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Wi-Fi: unsupported security type "
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
  if (!run_nmcli(modify, false, true)) {
    std::vector<std::string> add{"nmcli",
                                 "connection",
                                 "add",
                                 "type",
                                 "wifi",
                                 "ifname",
                                 interface_name,
                                 "con-name",
                                 configuration.ssid,
                                 "ssid",
                                 configuration.ssid,
                                 "wifi-sec.key-mgmt",
                                 key_management};
    if (configuration.security_type == 2 || configuration.security_type == 3) {
      add.emplace_back("802-11-wireless-security.proto");
      add.emplace_back("rsn");
    }
    if (!run_nmcli(std::move(add), false, true))
      return false;
  }
  std::vector<std::string> up{
      "nmcli", "--wait",           "30",     "connection",  "up",
      "id",    configuration.ssid, "ifname", interface_name};
  std::string password_file;
  if (!configuration.passphrase.empty()) {
    up.emplace_back("passwd-file");
    up.emplace_back("/dev/stdin");
    password_file =
        "802-11-wireless-security.psk:" + configuration.passphrase + '\n';
  }
  return run_nmcli(std::move(up), false, true, password_file);
}

std::optional<std::string>
accessory_ipv4_endpoint_for_interface(const std::string &interface_name) {
  std::ifstream routes("/proc/net/route");
  std::string line;
  std::getline(routes, line);
  while (std::getline(routes, line)) {
    std::istringstream fields(line);
    std::string interface;
    std::string destination;
    std::string gateway;
    std::string flags;
    if (!(fields >> interface >> destination >> gateway >> flags) ||
        interface != interface_name || destination != "00000000") {
      continue;
    }
    const auto route_flags = std::stoul(flags, nullptr, 16);
    if ((route_flags & 0x2U) == 0) {
      continue;
    }
    in_addr address{};
    address.s_addr = static_cast<in_addr_t>(std::stoul(gateway, nullptr, 16));
    std::array<char, INET_ADDRSTRLEN> text{};
    if (inet_ntop(AF_INET, &address, text.data(), text.size()) != nullptr) {
      return text.data();
    }
  }

  ifaddrs *addresses{};
  if (getifaddrs(&addresses) != 0) {
    return std::nullopt;
  }
  std::optional<std::string> endpoint;
  for (auto *entry = addresses; entry != nullptr; entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || entry->ifa_netmask == nullptr ||
        entry->ifa_addr->sa_family != AF_INET ||
        interface_name != entry->ifa_name) {
      continue;
    }
    const auto *address =
        reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
    const auto *netmask =
        reinterpret_cast<const sockaddr_in *>(entry->ifa_netmask);
    in_addr first_host{};
    first_host.s_addr =
        (address->sin_addr.s_addr & netmask->sin_addr.s_addr) | htonl(1);
    std::array<char, INET_ADDRSTRLEN> text{};
    if (inet_ntop(AF_INET, &first_host, text.data(), text.size()) != nullptr) {
      endpoint = text.data();
      break;
    }
  }
  freeifaddrs(addresses);
  return endpoint;
}

bool leave_with_networkmanager(const std::string &interface_name) {
  std::lock_guard lock(network_manager_mutex);
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Wi-Fi: disconnecting " << interface_name
      << " while retaining its saved profile\n";
  return run_nmcli({"nmcli", "device", "disconnect", interface_name}, true,
                   true);
}

bool start_management_hotspot(const std::string &interface_name,
                              const std::string &ssid,
                              const std::string &passphrase) {
  std::lock_guard lock(network_manager_mutex);
  if (interface_name.empty() || ssid.empty() || passphrase.size() < 8) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Wi-Fi: invalid management hotspot configuration\n";
    return false;
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Wi-Fi: starting configured management hotspot on " << interface_name
      << '\n';
  const std::vector<std::string> modify{"nmcli",
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
                                        "wpa-psk"};
  if (!run_nmcli(modify, false, true)) {
    if (!run_nmcli({"nmcli", "connection", "add", "type", "wifi", "ifname",
                    interface_name, "con-name", kManagementProfile, "ssid",
                    ssid, "802-11-wireless.mode", "ap", "ipv4.method", "shared",
                    "ipv6.method", "disabled", "wifi-sec.key-mgmt", "wpa-psk"},
                   false, true))
      return false;
  }
  const auto password_file =
      std::string("802-11-wireless-security.psk:") + passphrase + '\n';
  if (!run_nmcli({"nmcli", "--wait", "30", "connection", "up", "id",
                  kManagementProfile, "ifname", interface_name, "passwd-file",
                  "/dev/stdin"},
                 false, true, password_file))
    return false;
  if (const auto address = ipv4_address(interface_name))
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
        << "Wi-Fi: management hotspot gateway is " << *address << '\n';
  return true;
}

bool stop_management_hotspot(const std::string &interface_name) {
  std::lock_guard lock(network_manager_mutex);
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Wi-Fi: stopping management hotspot on " << interface_name << '\n';
  return run_nmcli({"nmcli", "connection", "down", "id", kManagementProfile,
                    "ifname", interface_name},
                   true, true);
}

} // namespace aa2acp::iap2
