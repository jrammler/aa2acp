#include "aa2acp/airplay/session.hpp"
#include "aa2acp/iap2/bluez_pairing.hpp"
#include "aa2acp/iap2/bootstrap.hpp"
#include "aa2acp/iap2/carplay_probe.hpp"
#include "aa2acp/iap2/link_layer.hpp"

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t shutdown_requested = 0;

void request_shutdown(int) { shutdown_requested = 1; }

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

int connect_unix(const std::string &path) {
  if (path.size() >= sizeof(sockaddr_un::sun_path))
    return -1;
  const auto socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (socket_fd < 0)
    return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::copy(path.begin(), path.end(), address.sun_path);
  if (connect(socket_fd, reinterpret_cast<sockaddr *>(&address),
              sizeof(address)) != 0) {
    close(socket_fd);
    return -1;
  }
  return socket_fd;
}

bool receive_all(const int socket_fd, std::span<std::uint8_t> bytes) {
  std::size_t offset{};
  while (offset < bytes.size() && shutdown_requested == 0) {
    pollfd descriptor{socket_fd, POLLIN, 0};
    if (poll(&descriptor, 1, 100) <= 0)
      continue;
    const auto count =
        recv(socket_fd, bytes.data() + offset, bytes.size() - offset, 0);
    if (count <= 0)
      return false;
    offset += static_cast<std::size_t>(count);
  }
  return offset == bytes.size();
}

class VideoSocketReader {
public:
  explicit VideoSocketReader(std::string path) : path_(std::move(path)) {}
  ~VideoSocketReader() {
    if (socket_fd_ >= 0)
      close(socket_fd_);
  }

  std::optional<std::vector<std::uint8_t>> next() {
    if (socket_fd_ < 0) {
      socket_fd_ = connect_unix(path_);
      if (socket_fd_ < 0) {
        std::cerr << "Unable to connect to Android Auto video socket " << path_
                  << '\n';
        return std::nullopt;
      }
      std::cout << "Bridge: connected to Android Auto video source\n";
    }
    std::array<std::uint8_t, 4> header{};
    if (!receive_all(socket_fd_, header))
      return std::nullopt;
    const auto size = (static_cast<std::size_t>(header[0]) << 24) |
                      (static_cast<std::size_t>(header[1]) << 16) |
                      (static_cast<std::size_t>(header[2]) << 8) | header[3];
    if (size == 0 || size > 4 * 1024 * 1024) {
      std::cerr << "Invalid Android Auto H.264 access-unit size " << size
                << '\n';
      return std::nullopt;
    }
    std::vector<std::uint8_t> frame(size);
    return receive_all(socket_fd_, frame) ? std::optional(std::move(frame))
                                          : std::nullopt;
  }

private:
  std::string path_;
  int socket_fd_{-1};
};

class AudioSocketReader {
public:
  explicit AudioSocketReader(std::string path) : path_(std::move(path)) {}
  ~AudioSocketReader() {
    if (socket_fd_ >= 0)
      close(socket_fd_);
  }

  std::optional<std::vector<std::uint8_t>> next() {
    if (socket_fd_ < 0) {
      socket_fd_ = connect_unix(path_);
      if (socket_fd_ < 0) {
        std::cerr << "Unable to connect to Android Auto audio socket " << path_
                  << '\n';
        return std::nullopt;
      }
      std::cout << "Bridge: connected to Android Auto media audio source\n";
    }
    std::array<std::uint8_t, 4> header{};
    if (!receive_all(socket_fd_, header))
      return std::nullopt;
    const auto size = (static_cast<std::size_t>(header[0]) << 24) |
                      (static_cast<std::size_t>(header[1]) << 16) |
                      (static_cast<std::size_t>(header[2]) << 8) | header[3];
    if (size == 0 || size > 64 * 1024) {
      std::cerr << "Invalid Android Auto PCM packet size " << size << '\n';
      return std::nullopt;
    }
    std::vector<std::uint8_t> frame(size);
    return receive_all(socket_fd_, frame) ? std::optional(std::move(frame))
                                          : std::nullopt;
  }

private:
  std::string path_;
  int socket_fd_{-1};
};

} // namespace

int main(int argc, char **argv) {
  // bridge-daemon captures this process through a pipe. Keep protocol progress
  // visible in the daemon log instead of waiting for process exit to flush it.
  std::cout.setf(std::ios::unitbuf);
  std::cerr.setf(std::ios::unitbuf);
  std::string address;
  std::uint8_t channel = 3;
  int timeout_seconds = 15;
  bool bootstrap = false;
  bool carplay = false;
  bool wifi_config = false;
  bool join_wifi = false;
  bool leave_wifi = false;
  bool bridge = false;
  std::string video_path;
  std::string video_socket;
  std::string audio_socket;
  std::string pairing_store;
  std::string display_profile_store;
  std::string wifi_interface;
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
    } else if (argument == "--display-profile-store" && index + 1 < argc) {
      display_profile_store = argv[++index];
    } else if (argument == "--video-socket" && index + 1 < argc) {
      video_socket = argv[++index];
    } else if (argument == "--audio-socket" && index + 1 < argc) {
      audio_socket = argv[++index];
    } else if (argument == "--pairing-store" && index + 1 < argc) {
      pairing_store = argv[++index];
    } else if (argument == "--wifi-interface" && index + 1 < argc) {
      wifi_interface = argv[++index];
    } else {
      std::cerr
          << "usage: aa2acp-iap2-bt [--mac MAC] [--channel N] [--timeout "
             "SECONDS] "
             "[--bootstrap] [--carplay] [--wifi-config] [--join-wifi] "
             "[--leave-wifi] [--wifi-interface IFACE] [--bridge] [--video "
             "H264_FILE] [--video-socket PATH] [--audio-socket PATH] "
             "[--pairing-store FILE]\n";
      return 2;
    }
  }

  if (leave_wifi) {
    if (wifi_interface.empty()) {
      std::cerr << "--leave-wifi requires --wifi-interface\n";
      return 2;
    }
    return aa2acp::iap2::leave_with_networkmanager(wifi_interface) ? 0 : 1;
  }

  if (address.empty()) {
    std::cerr << "--mac is required\n";
    return 2;
  }
  if ((wifi_config || join_wifi || bridge) && wifi_interface.empty()) {
    std::cerr << "--wifi-interface is required for a CarPlay Wi-Fi session\n";
    return 2;
  }

  if (bridge) {
    std::signal(SIGINT, request_shutdown);
    std::signal(SIGTERM, request_shutdown);
  }

  // Every Bluetooth connection has a usable bond: this is an immediate no-op
  // for an existing BlueZ record and performs discovery/pairing only when it
  // is absent.
  if (!aa2acp::iap2::ensure_bluez_pairing(address, timeout_seconds)) {
    return 1;
  }

  const auto socket_fd = connect_rfcomm(address, channel);
  if (socket_fd < 0) {
    std::cerr << "Unable to open RFCOMM channel " << static_cast<int>(channel)
              << " to " << address
              << "; ensure the device is paired and the head unit is "
                 "advertising iAP2\n";
    return 1;
  }
  std::cout << "RFCOMM connected to " << address << ':'
            << static_cast<int>(channel) << '\n';
  aa2acp::iap2::BootstrapSession session;
  aa2acp::iap2::CarPlayProbe carplay_probe(address);
  aa2acp::iap2::PhoneLink link(
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
            const aa2acp::iap2::AccessoryWifiConfiguration &configuration) {
          return aa2acp::iap2::join_with_networkmanager(configuration,
                                                        wifi_interface);
        });
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  link.start(std::chrono::steady_clock::now());

  std::array<std::uint8_t, 1024> buffer{};
  while (std::chrono::steady_clock::now() < deadline &&
         shutdown_requested == 0 && link.state() != aa2acp::iap2::State::Dead &&
         (!bootstrap ||
          (carplay ? (!carplay_probe.done() && !carplay_probe.failed())
                   : (!session.done() && !session.failed()))) &&
         (bootstrap || link.state() != aa2acp::iap2::State::Normal)) {
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
    if (bootstrap && link.state() == aa2acp::iap2::State::Normal &&
        !session.started()) {
      session.begin();
    }
    if (carplay && session.done() && !carplay_probe.started()) {
      carplay_probe.begin();
    }
  }
  close(socket_fd);
  if (shutdown_requested != 0) {
    if (join_wifi)
      aa2acp::iap2::leave_with_networkmanager(wifi_interface);
    std::cerr << "Bridge: shutdown requested during iAP2 phase\n";
    return 128 + SIGTERM;
  }
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
    const auto run_airplay = [&] {
      const auto live_video =
          video_socket.empty()
              ? std::shared_ptr<VideoSocketReader>{}
              : std::make_shared<VideoSocketReader>(video_socket);
      const auto live_audio =
          audio_socket.empty()
              ? std::shared_ptr<AudioSocketReader>{}
              : std::make_shared<AudioSocketReader>(audio_socket);
      const aa2acp::airplay::SessionOptions options{
          .host = host,
          .port = static_cast<std::uint16_t>(carplay_probe.airplay_port()),
          .timeout_seconds = timeout_seconds,
          .video_path = video_path,
          .next_video_frame =
              live_video
                  ? [live_video] { return live_video->next(); }
                  : std::function<std::optional<std::vector<std::uint8_t>>()>{},
          .next_media_audio =
              live_audio
                  ? [live_audio] { return live_audio->next(); }
                  : std::function<std::optional<std::vector<std::uint8_t>>()>{},
          .pairing_store = pairing_store,
          .display_profile_store = display_profile_store,
          .head_unit_mac = address,
          .stop_requested = [] { return shutdown_requested != 0; },
      };
      return aa2acp::airplay::run_session(options);
    };
    auto result = run_airplay();
    if (result != 0 && shutdown_requested == 0) {
      std::cerr << "Bridge: AirPlay attempt failed; retrying once\n";
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (shutdown_requested == 0)
        result = run_airplay();
    }
    // The profile and credentials remain in NetworkManager for fast reconnect,
    // but the car AP must not remain the active idle network.
    const auto left_wifi =
        aa2acp::iap2::leave_with_networkmanager(wifi_interface);
    if (shutdown_requested != 0) {
      std::cerr << "Bridge: shutdown requested during AirPlay phase\n";
      return 128 + SIGTERM;
    }
    if (result != 0) {
      std::cerr << "Bridge: AirPlay phase failed\n";
    }
    return result != 0 || !left_wifi ? 1 : 0;
  }
  if (carplay) {
    return carplay_probe.done() ? 0 : 1;
  }
  return bootstrap ? (session.done() ? 0 : 1)
                   : (link.state() == aa2acp::iap2::State::Normal ? 0 : 1);
}
