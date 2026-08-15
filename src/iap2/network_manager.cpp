#include "acp/iap2/network_manager.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <vector>

extern char** environ;

namespace acp::iap2 {
namespace {

bool run_nmcli(std::vector<std::string> arguments, const bool allow_inactive = false,
               const bool quiet = false) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    pid_t child{};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_t* action_pointer = nullptr;
    if (quiet) {
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        action_pointer = &actions;
    }
    const auto result = posix_spawnp(&child, argv.front(), action_pointer, nullptr, argv.data(), environ);
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
        (WEXITSTATUS(status) != 0 && !(allow_inactive && WEXITSTATUS(status) == 6))) {
        std::cerr << "Wi-Fi: NetworkManager connection command failed\n";
        return false;
    }
    return true;
}

}  // namespace

bool join_with_networkmanager(const AccessoryWifiConfiguration& configuration,
                              const std::string& interface_name) {
    if (configuration.ssid.empty()) {
        std::cerr << "Wi-Fi: accessory sent an empty SSID\n";
        return false;
    }
    std::cout << "Wi-Fi: joining SSID '" << configuration.ssid << "' on " << interface_name
              << " (channel " << static_cast<int>(configuration.channel) << ")\n";
    std::vector<std::string> arguments{"nmcli", "--wait", "30", "device", "wifi", "connect",
                                       configuration.ssid};
    if (!configuration.passphrase.empty()) {
        arguments.emplace_back("password");
        arguments.push_back(configuration.passphrase);
    }
    arguments.emplace_back("ifname");
    arguments.push_back(interface_name);
    return run_nmcli(std::move(arguments));
}

bool leave_with_networkmanager(const std::string& interface_name) {
    std::cout << "Wi-Fi: disconnecting " << interface_name << " while retaining its saved profile\n";
    return run_nmcli({"nmcli", "device", "disconnect", interface_name}, true, true);
}

}  // namespace acp::iap2
