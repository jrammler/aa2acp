#include "acp/iap2/bluez_pairing.hpp"
#include "acp/iap2/bootstrap.hpp"
#include "acp/iap2/carplay_probe.hpp"
#include "acp/iap2/link_layer.hpp"

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

extern char **environ;

namespace {

bool send_all(const int socket_fd, const std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto written = send(socket_fd, bytes.data() + offset,
                              bytes.size() - offset, MSG_NOSIGNAL);
    if (written <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

int connect_rfcomm(const std::string &address, const std::uint8_t channel) {
  sockaddr_rc remote{};
  remote.rc_family = AF_BLUETOOTH;
  remote.rc_channel = channel;
  if (str2ba(address.c_str(), &remote.rc_bdaddr) != 0) {
    return -1;
  }
  const auto socket_fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
  if (socket_fd < 0 || connect(socket_fd, reinterpret_cast<sockaddr *>(&remote),
                               sizeof(remote)) != 0) {
    if (socket_fd >= 0) {
      close(socket_fd);
    }
    return -1;
  }
  return socket_fd;
}

bool run_airplay_probe(const std::string &program_path, const std::string &host,
                       const std::uint32_t port, const std::string &video_path,
                       const std::string &pairing_store,
                       const int timeout_seconds) {
  const auto probe_path = (std::filesystem::path(program_path).parent_path() /
                           "airplay-pair-setup-probe")
                              .string();
  std::vector<std::string> arguments{probe_path,
                                     "--host",
                                     host,
                                     "--port",
                                     std::to_string(port),
                                     "--timeout",
                                     std::to_string(timeout_seconds)};
  if (!video_path.empty()) {
    arguments.emplace_back("--video");
    arguments.push_back(video_path);
  }
  if (!pairing_store.empty()) {
    arguments.emplace_back("--pairing-store");
    arguments.push_back(pairing_store);
  }
  std::vector<char *> argv;
  for (auto &argument : arguments)
    argv.push_back(argument.data());
  argv.push_back(nullptr);
  pid_t child{};
  if (posix_spawn(&child, argv.front(), nullptr, nullptr, argv.data(),
                  environ) != 0) {
    std::cerr << "Unable to launch AirPlay bridge phase at " << probe_path
              << '\n';
    return false;
  }
  int status{};
  return waitpid(child, &status, 0) >= 0 && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0;
}

} // namespace

int main(int argc, char **argv) {
  std::string address = "[redacted-device-address]";
  std::uint8_t channel = 3;
  int timeout_seconds = 15;
  bool bootstrap = false;
  bool carplay = false;
  bool pair = false;
  bool wifi_config = false;
  bool join_wifi = false;
  bool leave_wifi = false;
  bool bridge = false;
  std::string video_path;
  std::string pairing_store;
  std::string wifi_interface = "wlp15s0";
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--mac" && index + 1 < argc) {
      address = argv[++index];
    } else if (argument == "--channel" && index + 1 < argc) {
      channel = static_cast<std::uint8_t>(std::stoi(argv[++index]));
    } else if (argument == "--timeout" && index + 1 < argc) {
      timeout_seconds = std::stoi(argv[++index]);
    } else if (argument == "--bootstrap") {
      bootstrap = true;
    } else if (argument == "--carplay") {
      bootstrap = true;
      carplay = true;
    } else if (argument == "--pair") {
      pair = true;
    } else if (argument == "--wifi-config") {
      bootstrap = true;
      carplay = true;
      wifi_config = true;
    } else if (argument == "--join-wifi") {
      bootstrap = true;
      carplay = true;
      wifi_config = true;
      join_wifi = true;
    } else if (argument == "--leave-wifi") {
      leave_wifi = true;
    } else if (argument == "--bridge") {
      bootstrap = true;
      carplay = true;
      wifi_config = true;
      join_wifi = true;
      bridge = true;
    } else if (argument == "--video" && index + 1 < argc) {
      video_path = argv[++index];
    } else if (argument == "--pairing-store" && index + 1 < argc) {
      pairing_store = argv[++index];
    } else if (argument == "--wifi-interface" && index + 1 < argc) {
      wifi_interface = argv[++index];
    } else {
      std::cerr
          << "usage: iap2-bt [--mac MAC] [--channel N] [--timeout SECONDS] "
             "[--pair] [--bootstrap] [--carplay] [--wifi-config] [--join-wifi] "
             "[--leave-wifi] [--wifi-interface IFACE] [--bridge] [--video "
             "H264_FILE] [--pairing-store FILE]\n";
      return 2;
    }
  }

  if (leave_wifi) {
    return acp::iap2::leave_with_networkmanager(wifi_interface) ? 0 : 1;
  }

  if (pair && !acp::iap2::ensure_bluez_pairing(address, timeout_seconds)) {
    return 1;
  }

  const auto socket_fd = connect_rfcomm(address, channel);
  if (socket_fd < 0) {
    std::cerr << "Unable to open RFCOMM channel " << static_cast<int>(channel)
              << " to " << address
              << "; ensure the device is paired and test head unit is advertising iAP2\n";
    return 1;
  }
  std::cout << "RFCOMM connected to " << address << ':'
            << static_cast<int>(channel) << '\n';
  acp::iap2::BootstrapSession session;
  acp::iap2::CarPlayProbe carplay_probe(address);
  acp::iap2::PhoneLink link(
      [socket_fd](const std::span<const std::uint8_t> bytes) {
        return send_all(socket_fd, bytes);
      },
      [](const char *message) { std::cout << message << '\n'; },
      [&session, &carplay_probe,
       &carplay](const std::span<const std::uint8_t> bytes) {
        if (carplay && session.done()) {
          carplay_probe.receive(bytes);
        } else {
          session.receive(bytes);
        }
      });
  session.attach(link);
  carplay_probe.attach(link);
  carplay_probe.request_wifi_configuration(wifi_config);
  if (join_wifi) {
    carplay_probe.set_wifi_join_handler(
        [&wifi_interface](
            const acp::iap2::AccessoryWifiConfiguration &configuration) {
          return acp::iap2::join_with_networkmanager(configuration,
                                                     wifi_interface);
        });
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  link.start(std::chrono::steady_clock::now());

  std::array<std::uint8_t, 1024> buffer{};
  while (std::chrono::steady_clock::now() < deadline &&
         link.state() != acp::iap2::State::Dead &&
         (!bootstrap ||
          (carplay ? (!carplay_probe.done() && !carplay_probe.failed())
                   : (!session.done() && !session.failed()))) &&
         (bootstrap || link.state() != acp::iap2::State::Normal)) {
    pollfd descriptor{socket_fd, POLLIN, 0};
    const auto result = poll(&descriptor, 1, 100);
    const auto now = std::chrono::steady_clock::now();
    if (result > 0 && (descriptor.revents & POLLIN) != 0) {
      const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
      if (count <= 0) {
        std::cerr << "RFCOMM connection closed by accessory\n";
        break;
      }
      link.receive(std::span(buffer).first(static_cast<std::size_t>(count)),
                   now);
    }
    link.tick(now);
    if (bootstrap && link.state() == acp::iap2::State::Normal &&
        !session.started()) {
      session.begin();
    }
    if (carplay && session.done() && !carplay_probe.started()) {
      carplay_probe.begin();
    }
  }
  close(socket_fd);
  if (bridge && carplay_probe.done()) {
    // In a Bluetooth-originated wireless CarPlay session, the StartSession
    // device identifier is the head unit's BT identity, not an IP address.
    // AirPlay moves to its advertised AP gateway after WirelessCarPlayUpdate.
    const std::string host = "10.10.0.1";
    if (carplay_probe.airplay_port() == 0) {
      std::cerr << "CarPlayStartSession did not provide an AirPlay port\n";
      return 1;
    }
    std::cout << "Bridge: starting AirPlay on " << host << ':'
              << carplay_probe.airplay_port() << '\n';
    const auto result =
        run_airplay_probe(argv[0], host, carplay_probe.airplay_port(),
                          video_path, pairing_store, timeout_seconds);
    if (!result) {
      std::cerr << "Bridge: AirPlay phase failed\n";
    }
    return result ? 0 : 1;
  }
  if (carplay) {
    return carplay_probe.done() ? 0 : 1;
  }
  return bootstrap ? (session.done() ? 0 : 1)
                   : (link.state() == acp::iap2::State::Normal ? 0 : 1);
}
