#include "aa2acp/airplay/session.hpp"
#include "aa2acp/bridge/logging.hpp"
#include "aa2acp/iap2/bluetooth_worker.hpp"
#include "aa2acp/iap2/bluez_pairing.hpp"
#include "aa2acp/iap2/bootstrap.hpp"
#include "aa2acp/iap2/carplay_probe.hpp"
#include "aa2acp/iap2/link_layer.hpp"

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/rfcomm.h>

#include <csignal>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t signal_shutdown_requested = 0;
std::atomic_bool worker_stop_requested{};
std::mutex pairing_confirmation_mutex;
int pairing_confirmation_control_fd = -1;
std::uint64_t next_pairing_confirmation_id = 1;
std::uint64_t pending_pairing_confirmation_id{};
int pending_pairing_confirmation_result = -1;
void request_shutdown(int) { signal_shutdown_requested = 1; }

bool shutdown_requested() {
  return signal_shutdown_requested != 0 || worker_stop_requested.load();
}

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

// Connects to an iAP2 endpoint over L2CAP (used when SDP advertises the
// service with a PSM instead of an RFCOMM channel).
int connect_l2cap(const std::string &address, const std::uint16_t psm,
                  int *error) {
  sockaddr_l2 remote{};
  remote.l2_family = AF_BLUETOOTH;
  remote.l2_psm = htobs(psm);
  if (str2ba(address.c_str(), &remote.l2_bdaddr) != 0) {
    if (error != nullptr)
      *error = EINVAL;
    return -1;
  }
  const auto socket_fd =
      socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
  if (socket_fd < 0) {
    if (error != nullptr)
      *error = errno;
    return -1;
  }
  if (connect(socket_fd, reinterpret_cast<sockaddr *>(&remote),
              sizeof(remote)) != 0) {
    const int connect_error = errno;
    close(socket_fd);
    if (error != nullptr)
      *error = connect_error;
    return -1;
  }
  return socket_fd;
}

int connect_rfcomm(const std::string &address, const std::uint8_t channel,
                   int *error) {
  sockaddr_rc remote{};
  remote.rc_family = AF_BLUETOOTH;
  remote.rc_channel = channel;
  if (str2ba(address.c_str(), &remote.rc_bdaddr) != 0) {
    if (error != nullptr)
      *error = EINVAL;
    return -1;
  }
  const auto socket_fd =
      socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_CLOEXEC, BTPROTO_RFCOMM);
  if (socket_fd < 0) {
    if (error != nullptr)
      *error = errno;
    return -1;
  }
  if (connect(socket_fd, reinterpret_cast<sockaddr *>(&remote),
              sizeof(remote)) != 0) {
    const int connect_error = errno;
    if (socket_fd >= 0) {
      close(socket_fd);
    }
    if (error != nullptr)
      *error = connect_error;
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

bool receive_all(const int socket_fd, std::span<std::uint8_t> bytes,
                 const std::atomic_bool *stop_streams = nullptr) {
  std::size_t offset{};
  while (offset < bytes.size() && !shutdown_requested() &&
         (stop_streams == nullptr || !stop_streams->load())) {
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
  AudioSocketReader(std::string path, std::shared_ptr<std::atomic_bool> stop)
      : path_(std::move(path)), stop_(std::move(stop)) {}
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
    if (!receive_all(socket_fd_, header, stop_.get()))
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
    return receive_all(socket_fd_, frame, stop_.get())
               ? std::optional(std::move(frame))
               : std::nullopt;
  }

private:
  std::string path_;
  std::shared_ptr<std::atomic_bool> stop_;
  int socket_fd_{-1};
};

} // namespace

// Runs the iAP2 marker-detection phase on an open RFCOMM socket for up to
// five seconds. Returns true only when the accessory marker is recognized
// (state advanced past Detect); a dead link or silence fails the probe so
// the sweep can try the next candidate.
bool probe_iap2_detect(const int socket_fd, const std::string &address,
                       const std::uint8_t channel) {
  aa2acp::iap2::PhoneLink link(
      [socket_fd](const std::span<const std::uint8_t> bytes) {
        return send_all(socket_fd, bytes);
      },
      [](const char *message) {
        if (aa2acp::bridge::debug_logging_enabled())
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
              << message << '\n';
      },
      [](const std::span<const std::uint8_t>) {});
  const auto start = std::chrono::steady_clock::now();
  constexpr auto kDetectWindow = std::chrono::seconds(5);
  link.start(start);
  std::array<std::uint8_t, 1024> buffer{};
  while (std::chrono::steady_clock::now() - start < kDetectWindow) {
    if (shutdown_requested()) {
      return false;
    }
    pollfd descriptor{socket_fd, POLLIN, 0};
    const auto result = poll(&descriptor, 1, 100);
    if (result < 0) {
      continue; // EINTR etc.; re-check shutdown above
    }
    if ((descriptor.revents & (POLLHUP | POLLERR)) != 0 &&
        (descriptor.revents & POLLIN) == 0) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Bluetooth: RFCOMM channel " << static_cast<int>(channel)
          << " error/hangup during detection\n";
      return false;
    }
    if (result > 0 && (descriptor.revents & POLLIN) != 0) {
      const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
      if (count <= 0) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
            << "Bluetooth: RFCOMM channel " << static_cast<int>(channel)
            << " closed by accessory during detection\n";
        return false;
      }
      link.receive(std::span(buffer).first(static_cast<std::size_t>(count)),
                   std::chrono::steady_clock::now());
    }
    link.tick(std::chrono::steady_clock::now());
    // Only a real state advance counts as success; State::Dead means the
    // link layer gave up and this channel must not be selected.
    if (link.state() == aa2acp::iap2::State::Dead) {
      return false;
    }
    if (link.state() != aa2acp::iap2::State::Detect) {
      return true;
    }
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
      << "Bluetooth: no iAP2 response on RFCOMM channel "
      << static_cast<int>(channel) << " to " << address
      << "; trying next channel\n";
  (void)address;
  return false;
}

int aa2acp::iap2::run_bluetooth_worker(int argc, char **argv) {
  // bridge-daemon captures this process through a pipe. Keep protocol progress
  // visible in the daemon log instead of waiting for process exit to flush it.
  std::cout.setf(std::ios::unitbuf);
  std::cerr.setf(std::ios::unitbuf);
  std::string address;
  std::uint8_t channel = 3;
  // RFCOMM channels are not negotiated; some head units expose SPP on a
  // different channel than the default. Overridable for experiments.
  if (const auto *env_channel = std::getenv("AA2ACP_IAP2_CHANNEL")) {
    try {
      const auto parsed = std::stoi(env_channel);
      if (parsed >= 1 && parsed <= 30) {
        channel = static_cast<std::uint8_t>(parsed);
      } else {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
            << "Ignoring AA2ACP_IAP2_CHANNEL outside 1-30: " << parsed << '\n';
      }
    } catch (const std::exception &) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Ignoring invalid AA2ACP_IAP2_CHANNEL value\n";
    }
  }
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
      try {
        const auto parsed = std::stoi(argv[++index]);
        if (parsed >= 1 && parsed <= 30) {
          channel = static_cast<std::uint8_t>(parsed);
        }
      } catch (const std::exception &) {
      }
    } else if (argument == "--timeout" && index + 1 < argc) {
      try {
        timeout_seconds = std::stoi(argv[++index]);
      } catch (const std::exception &) {
        timeout_seconds = 15;
      }
      if (timeout_seconds <= 0)
        timeout_seconds = 15;
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

  // Candidate endpoints: SDP-discovered first (authoritative), then the
  // configured/default RFCOMM channel, then common low channels as a
  // last-resort sweep.
  const auto discovered = aa2acp::iap2::discover_endpoint(address);
  if (discovered.l2cap_psm) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
        << "Bluetooth: SDP advertises iAP2 on L2CAP PSM "
        << *discovered.l2cap_psm << '\n';
  }
  std::vector<std::uint8_t> channel_candidates;
  if (discovered.rfcomm_channel && *discovered.rfcomm_channel >= 1 &&
      *discovered.rfcomm_channel <= 30) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
        << "Bluetooth: SDP advertises SPP on RFCOMM channel "
        << static_cast<int>(*discovered.rfcomm_channel) << '\n';
    channel_candidates.push_back(*discovered.rfcomm_channel);
  } else if (discovered.rfcomm_channel) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
        << "Bluetooth: ignoring nonsensical SDP channel "
        << static_cast<int>(*discovered.rfcomm_channel) << '\n';
  } else {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
        << "Bluetooth: SDP discovery found no endpoint; sweeping\n";
  }
  // Fallback sweep: default channel plus common low channels. The
  // SDP-discovered channel (already pushed first, above) keeps its priority -
  // sorting only the sweep entries preserves that ordering.
  std::vector<std::uint8_t> sweep_candidates{channel};
  for (std::uint8_t candidate = 1; candidate <= 8; ++candidate) {
    sweep_candidates.push_back(candidate);
  }
  std::sort(sweep_candidates.begin(), sweep_candidates.end());
  sweep_candidates.erase(
      std::unique(sweep_candidates.begin(), sweep_candidates.end()),
      sweep_candidates.end());
  for (const auto candidate : sweep_candidates) {
    if (channel_candidates.empty() || channel_candidates.back() != candidate)
      channel_candidates.push_back(candidate);
  }

  int socket_fd = -1;
  int last_rfcomm_error{};
  std::optional<std::uint8_t> selected_channel;
  // L2CAP endpoint from SDP takes priority over the RFCOMM sweep.
  if (discovered.l2cap_psm) {
    socket_fd =
        connect_l2cap(address, *discovered.l2cap_psm, &last_rfcomm_error);
    if (socket_fd >= 0) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
          << "Bluetooth: connected L2CAP PSM " << *discovered.l2cap_psm << '\n';
    } else {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Bluetooth: L2CAP PSM " << *discovered.l2cap_psm
          << " connection failed (errno " << last_rfcomm_error
          << "); falling back to RFCOMM sweep\n";
    }
  }
  if (socket_fd >= 0) {
    // Probe the L2CAP endpoint before committing to it.
    if (probe_iap2_detect(socket_fd, address, 0)) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
          << "Bluetooth: iAP2 marker exchange succeeded on L2CAP PSM "
          << *discovered.l2cap_psm << '\n';
    } else {
      close(socket_fd);
      socket_fd = -1;
    }
  }
  if (socket_fd < 0) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
        << "Bluetooth: probing " << channel_candidates.size()
        << " candidate RFCOMM channel(s) for iAP2\n";
    for (const auto candidate : channel_candidates) {
      if (shutdown_requested()) {
        return 128 + SIGTERM;
      }
      int rfcomm_error{};
      constexpr int kRfcommAttempts = 3;
      bool connected = false;
      for (int attempt = 1; attempt <= kRfcommAttempts; ++attempt) {
        socket_fd = connect_rfcomm(address, candidate, &rfcomm_error);
        last_rfcomm_error = rfcomm_error;
        if (socket_fd >= 0) {
          connected = true;
          break;
        }
        if (shutdown_requested()) {
          return 128 + SIGTERM;
        }
        if (attempt < kRfcommAttempts) {
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
              << "Bluetooth: unable to connect RFCOMM channel "
              << static_cast<int>(candidate) << " ("
              << std::strerror(rfcomm_error) << ", errno " << rfcomm_error
              << "; attempt " << attempt << "/" << kRfcommAttempts
              << "); retrying in 500 ms\n";
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
      }
      if (!connected || socket_fd < 0) {
        continue;
      }
      if (probe_iap2_detect(socket_fd, address, candidate)) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
            << "Bluetooth: iAP2 marker exchange succeeded on RFCOMM channel "
            << static_cast<int>(candidate) << '\n';
        selected_channel = candidate;
        break;
      }
      close(socket_fd);
      socket_fd = -1;
    }
  } // RFCOMM sweep (skipped when an L2CAP endpoint already connected)
  if (socket_fd < 0) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to establish an iAP2 RFCOMM connection to " << address
        << (last_rfcomm_error != 0
                ? " (last connect error: " +
                      std::string(std::strerror(last_rfcomm_error)) + ")"
                : " (channels were reachable but no iAP2 response)")
        << "; ensure the device is in range, paired, and advertising iAP2\n";
    return 1;
  }
  if (aa2acp::bridge::debug_logging_enabled())
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
        << "RFCOMM connected to " << address << ':'
        << static_cast<int>(selected_channel.value_or(channel)) << '\n';
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
  while (std::chrono::steady_clock::now() < deadline && !shutdown_requested() &&
         link.state() != aa2acp::iap2::State::Dead &&
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
  if (shutdown_requested()) {
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
          const auto stop_streams = std::make_shared<std::atomic_bool>();
          const auto live_video =
              video_socket.empty()
                  ? std::shared_ptr<VideoSocketReader>{}
                  : std::make_shared<VideoSocketReader>(video_socket);
          const auto live_audio = audio_socket.empty()
                                      ? std::shared_ptr<AudioSocketReader>{}
                                      : std::make_shared<AudioSocketReader>(
                                            audio_socket, stop_streams);
          const auto live_guidance_audio =
              guidance_audio_socket.empty()
                  ? std::shared_ptr<AudioSocketReader>{}
                  : std::make_shared<AudioSocketReader>(guidance_audio_socket,
                                                        stop_streams);
          const auto live_system_audio =
              system_audio_socket.empty()
                  ? std::shared_ptr<AudioSocketReader>{}
                  : std::make_shared<AudioSocketReader>(system_audio_socket,
                                                        stop_streams);
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
          .stop_requested = [] { return shutdown_requested(); },
          .stop_streams = [stop_streams] { stop_streams->store(true); },
      };
          return aa2acp::airplay::run_session(options);
        };
    auto result = run_airplay();
    if (result != 0 && !shutdown_requested()) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Bridge: AirPlay attempt failed; retrying once\n";
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!shutdown_requested())
        result = run_airplay();
    }
    if (shutdown_requested()) {
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

void aa2acp::iap2::request_bluetooth_worker_stop() {
  worker_stop_requested.store(true);
}

void aa2acp::iap2::reset_bluetooth_worker_stop() {
  worker_stop_requested.store(false);
}

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
