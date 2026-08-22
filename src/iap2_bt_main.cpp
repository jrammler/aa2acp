#include "aa2acp/airplay/session.hpp"
#include "aa2acp/bridge/logging.hpp"
#include "aa2acp/iap2/bluetooth_worker.hpp"
#include "aa2acp/iap2/bluez_pairing.hpp"
#include "aa2acp/iap2/bootstrap.hpp"
#include "aa2acp/iap2/carplay_probe.hpp"
#include "aa2acp/iap2/link_layer.hpp"

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

#include <csignal>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t shutdown_requested = 0;
std::mutex pairing_confirmation_mutex;
int pairing_confirmation_control_fd = -1;
std::uint64_t next_pairing_confirmation_id = 1;
std::uint64_t pending_pairing_confirmation_id{};
int pending_pairing_confirmation_result = -1;
void request_shutdown(int) { shutdown_requested = 1; }

void install_shutdown_handlers() {
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  // The bridge daemon blocks shutdown signals in its parent thread before
  // spawning this worker.  Unblock them here so the handler can run.
  sigprocmask(SIG_UNBLOCK, &signals, nullptr);

  struct sigaction action{};
  action.sa_handler = request_shutdown;
  sigemptyset(&action.sa_mask);
  // Deliberately omit SA_RESTART so SIGTERM interrupts blocking socket I/O
  // and lets the worker unwind through its cleanup guards.
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
}

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

class WifiCleanup final {
public:
  WifiCleanup(std::string interface_name, std::string management_ssid,
              std::string management_passphrase)
      : interface_name_(std::move(interface_name)),
        management_ssid_(std::move(management_ssid)),
        management_passphrase_(std::move(management_passphrase)) {}

  ~WifiCleanup() {
    if (!joined_)
      return;
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
        << "Bridge: cleaning up accessory Wi-Fi\n";
    if (!aa2acp::iap2::leave_with_networkmanager(interface_name_))
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Bridge: unable to disconnect accessory Wi-Fi\n";
    if (!management_ssid_.empty() &&
        !aa2acp::iap2::start_management_hotspot(
            interface_name_, management_ssid_, management_passphrase_))
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Bridge: unable to restore management hotspot\n";
  }

  void mark_joined() { joined_ = true; }

private:
  std::string interface_name_;
  std::string management_ssid_;
  std::string management_passphrase_;
  bool joined_{};
};

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
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
            << "Unable to connect to Android Auto video socket " << path_
            << '\n';
        return std::nullopt;
      }
      if (aa2acp::bridge::debug_logging_enabled())
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
            << "Bridge: connected to Android Auto video source\n";
    }
    std::array<std::uint8_t, 4> header{};
    if (!receive_all(socket_fd_, header))
      return std::nullopt;
    const auto size = (static_cast<std::size_t>(header[0]) << 24) |
                      (static_cast<std::size_t>(header[1]) << 16) |
                      (static_cast<std::size_t>(header[2]) << 8) | header[3];
    if (size == 0 || size > 4 * 1024 * 1024) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Invalid Android Auto H.264 access-unit size " << size << '\n';
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
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
            << "Unable to connect to Android Auto audio socket " << path_
            << '\n';
        return std::nullopt;
      }
      if (aa2acp::bridge::debug_logging_enabled())
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
            << "Bridge: connected to Android Auto media audio source\n";
    }
    std::array<std::uint8_t, 4> header{};
    if (!receive_all(socket_fd_, header))
      return std::nullopt;
    const auto size = (static_cast<std::size_t>(header[0]) << 24) |
                      (static_cast<std::size_t>(header[1]) << 16) |
                      (static_cast<std::size_t>(header[2]) << 8) | header[3];
    if (size == 0 || size > 64 * 1024) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Invalid Android Auto PCM packet size " << size << '\n';
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

int aa2acp::iap2::run_bluetooth_worker(int argc, char **argv) {
  shutdown_requested = 0;
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
  std::string guidance_audio_socket;
  std::string system_audio_socket;
  std::string pairing_store;
  std::string head_unit_capabilities_store;
  std::string wifi_interface;
  std::string management_hotspot_ssid;
  std::string management_hotspot_passphrase;
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
    } else if (argument == "--preflight") {
      bootstrap = true;
      carplay = true;
      wifi_config = true;
      join_wifi = true;
      bridge = true;
    } else if (argument == "--video" && index + 1 < argc) {
      video_path = argv[++index];
    } else if (argument == "--head-unit-capabilities-store" &&
               index + 1 < argc) {
      head_unit_capabilities_store = argv[++index];
    } else if (argument == "--video-socket" && index + 1 < argc) {
      video_socket = argv[++index];
    } else if (argument == "--audio-socket" && index + 1 < argc) {
      audio_socket = argv[++index];
    } else if (argument == "--guidance-audio-socket" && index + 1 < argc) {
      guidance_audio_socket = argv[++index];
    } else if (argument == "--system-audio-socket" && index + 1 < argc) {
      system_audio_socket = argv[++index];
    } else if (argument == "--pairing-store" && index + 1 < argc) {
      pairing_store = argv[++index];
    } else if (argument == "--wifi-interface" && index + 1 < argc) {
      wifi_interface = argv[++index];
    } else if (argument == "--management-hotspot-ssid" && index + 1 < argc) {
      management_hotspot_ssid = argv[++index];
    } else if (argument == "--management-hotspot-passphrase" &&
               index + 1 < argc) {
      management_hotspot_passphrase = argv[++index];
    } else {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "usage: aa2acp-iap2-bt [--mac MAC] [--channel N] [--timeout "
             "SECONDS] "
             "[--bootstrap] [--carplay] [--wifi-config] [--join-wifi] "
             "[--leave-wifi] [--wifi-interface IFACE] [--bridge] [--preflight] "
             "[--video "
             "H264_FILE] [--video-socket PATH] [--audio-socket PATH] "
             "[--guidance-audio-socket PATH] [--system-audio-socket PATH] "
             "[--pairing-store FILE] [--head-unit-capabilities-store FILE]\n";
      return 2;
    }
  }

  if (leave_wifi) {
    if (wifi_interface.empty()) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "--leave-wifi requires --wifi-interface\n";
      return 2;
    }
    return aa2acp::iap2::leave_with_networkmanager(wifi_interface) ? 0 : 1;
  }

  if (address.empty()) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "--mac is required\n";
    return 2;
  }
  if ((wifi_config || join_wifi || bridge) && wifi_interface.empty()) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "--wifi-interface is required for a CarPlay Wi-Fi session\n";
    return 2;
  }

  WifiCleanup wifi_cleanup(wifi_interface, management_hotspot_ssid,
                           management_hotspot_passphrase);

  if (bridge) {
    install_shutdown_handlers();
  }

  // Every Bluetooth connection has a usable bond: this is an immediate no-op
  // for an existing BlueZ record and performs discovery/pairing only when it
  // is absent.
  if (!aa2acp::iap2::ensure_bluez_pairing(address, timeout_seconds)) {
    return 1;
  }

  int socket_fd = -1;
  constexpr int kRfcommAttempts = 6;
  for (int attempt = 1; attempt <= kRfcommAttempts; ++attempt) {
    socket_fd = connect_rfcomm(address, channel);
    if (socket_fd >= 0)
      break;
    if (attempt < kRfcommAttempts) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Bluetooth: RFCOMM channel " << static_cast<int>(channel)
          << " unavailable (attempt " << attempt << "/" << kRfcommAttempts
          << "); retrying in 500 ms\n";
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }
  if (socket_fd < 0) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to open RFCOMM channel " << static_cast<int>(channel)
        << " to " << address
        << "; ensure the device is paired and the head unit is advertising "
           "iAP2\n";
    return 1;
  }
  if (aa2acp::bridge::debug_logging_enabled())
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
        << "RFCOMM connected to " << address << ':' << static_cast<int>(channel)
        << '\n';
  aa2acp::iap2::BootstrapSession session;
  aa2acp::iap2::CarPlayProbe carplay_probe(address);
  aa2acp::iap2::PhoneLink link(
      [socket_fd](const std::span<const std::uint8_t> bytes) {
        return send_all(socket_fd, bytes);
      },
      [](const char *message) {
        if (aa2acp::bridge::debug_logging_enabled())
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
              << message << '\n';
      },
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
        [&wifi_interface, &wifi_cleanup](
            const aa2acp::iap2::AccessoryWifiConfiguration &configuration) {
          const auto joined = aa2acp::iap2::join_with_networkmanager(
              configuration, wifi_interface);
          if (joined)
            wifi_cleanup.mark_joined();
          return joined;
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
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
            << "RFCOMM connection closed by accessory\n";
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
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
        << "Bridge: shutdown requested during iAP2 phase\n";
    return 128 + SIGTERM;
  }
  if (bridge && carplay_probe.done()) {
    // In a Bluetooth-originated wireless CarPlay session, the StartSession
    // device identifier is the head unit's BT identity, not an IP address.
    // AirPlay moves to its advertised AP gateway after WirelessCarPlayUpdate.
    const std::string host = "10.10.0.1";
    if (carplay_probe.airplay_port() == 0) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "CarPlayStartSession did not provide an AirPlay port\n";
      return 1;
    }
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
        << "Bridge: starting AirPlay on " << host << ':'
        << carplay_probe.airplay_port() << '\n';
    const auto run_airplay =
        [&] {
          const auto live_video =
              video_socket.empty()
                  ? std::shared_ptr<VideoSocketReader>{}
                  : std::make_shared<VideoSocketReader>(video_socket);
          const auto live_audio =
              audio_socket.empty()
                  ? std::shared_ptr<AudioSocketReader>{}
                  : std::make_shared<AudioSocketReader>(audio_socket);
          const auto live_guidance_audio =
              guidance_audio_socket.empty()
                  ? std::shared_ptr<AudioSocketReader>{}
                  : std::make_shared<AudioSocketReader>(guidance_audio_socket);
          const auto live_system_audio =
              system_audio_socket.empty()
                  ? std::shared_ptr<AudioSocketReader>{}
                  : std::make_shared<AudioSocketReader>(system_audio_socket);
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
          .next_guidance_audio =
              live_guidance_audio
                  ? [live_guidance_audio] { return live_guidance_audio->next(); }
                  : std::function<std::optional<std::vector<std::uint8_t>>()>{},
          .next_system_audio =
              live_system_audio
                  ? [live_system_audio] { return live_system_audio->next(); }
                  : std::function<std::optional<std::vector<std::uint8_t>>()>{},
          .pairing_store = pairing_store,
          .head_unit_capabilities_store = head_unit_capabilities_store,
          .head_unit_mac = address,
          .stop_requested = [] { return shutdown_requested != 0; },
      };
          return aa2acp::airplay::run_session(options);
        };
    auto result = run_airplay();
    if (result != 0 && shutdown_requested == 0) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Bridge: AirPlay attempt failed; retrying once\n";
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (shutdown_requested == 0)
        result = run_airplay();
    }
    if (shutdown_requested != 0) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
          << "Bridge: shutdown requested during AirPlay phase\n";
      return 128 + SIGTERM;
    }
    if (result != 0) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Bridge: AirPlay phase failed\n";
    }
    return result != 0 ? 1 : 0;
  }
  if (carplay) {
    return carplay_probe.done() ? 0 : 1;
  }
  return bootstrap ? (session.done() ? 0 : 1)
                   : (link.state() == aa2acp::iap2::State::Normal ? 0 : 1);
}

void aa2acp::iap2::request_bluetooth_worker_stop() { shutdown_requested = 1; }

void aa2acp::iap2::set_pairing_confirmation_control_fd(const int fd) {
  std::lock_guard lock(pairing_confirmation_mutex);
  pairing_confirmation_control_fd = fd;
}

bool aa2acp::iap2::request_pairing_confirmation(const std::string_view address,
                                                const std::uint32_t passkey,
                                                std::uint64_t *id) {
  std::lock_guard lock(pairing_confirmation_mutex);
  if (pairing_confirmation_control_fd < 0 ||
      pending_pairing_confirmation_id != 0)
    return false;
  PairingConfirmationMessage message{next_pairing_confirmation_id++, passkey};
  // BlueZ Agent1 supplies a device object path here, not necessarily a MAC
  // address. The UI only needs the numeric code, so retain an address only
  // when it fits the optional display field.
  if (address.size() < sizeof(message.address))
    std::copy(address.begin(), address.end(), message.address);
  std::array<std::byte, 1 + sizeof(message)> packet{};
  packet[0] = std::byte{3};
  std::memcpy(packet.data() + 1, &message, sizeof(message));
  if (send(pairing_confirmation_control_fd, packet.data(), packet.size(),
           MSG_NOSIGNAL) != static_cast<ssize_t>(packet.size()))
    return false;
  pending_pairing_confirmation_id = message.id;
  pending_pairing_confirmation_result = -1;
  if (id != nullptr)
    *id = message.id;
  return true;
}

bool aa2acp::iap2::pairing_confirmation_result(const std::uint64_t id,
                                               bool *confirmed) {
  std::lock_guard lock(pairing_confirmation_mutex);
  if (pending_pairing_confirmation_id != id ||
      pending_pairing_confirmation_result < 0)
    return false;
  if (confirmed != nullptr)
    *confirmed = pending_pairing_confirmation_result == 1;
  pending_pairing_confirmation_id = 0;
  pending_pairing_confirmation_result = -1;
  return true;
}

void aa2acp::iap2::answer_pairing_confirmation(const std::uint64_t id,
                                               const bool confirmed) {
  std::lock_guard lock(pairing_confirmation_mutex);
  if (pending_pairing_confirmation_id == id)
    pending_pairing_confirmation_result = confirmed ? 1 : 0;
}

void aa2acp::iap2::cancel_pairing_confirmation(const std::uint64_t id) {
  std::lock_guard lock(pairing_confirmation_mutex);
  if (pending_pairing_confirmation_id == id) {
    pending_pairing_confirmation_id = 0;
    pending_pairing_confirmation_result = -1;
  }
}
