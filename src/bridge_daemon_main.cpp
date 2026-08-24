#include "aa2acp/aa/wired_receiver.hpp"
#include "aa2acp/airplay/head_unit_capabilities.hpp"
#include "aa2acp/bridge/bluez_inventory.hpp"
#include "aa2acp/bridge/carplay_worker.hpp"
#include "aa2acp/bridge/config.hpp"
#include "aa2acp/bridge/daemon_log.hpp"
#include "aa2acp/bridge/h264_normalizer.hpp"
#include "aa2acp/bridge/logging.hpp"
#include "aa2acp/bridge/management_ui.hpp"
#include "aa2acp/bridge/media_forwarders.hpp"
#include "aa2acp/iap2/bluetooth_worker.hpp"
#include "aa2acp/iap2/network_manager.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using aa2acp::bridge::AudioSocketForwarder;
using aa2acp::bridge::CarPlayWorker;
using aa2acp::bridge::DaemonLog;
using aa2acp::bridge::next_daemon_log_path;
using aa2acp::bridge::RecentLog;
using aa2acp::bridge::VideoSocketForwarder;
using aa2acp::bridge::management::form_field;
using aa2acp::bridge::management::html_escape;
using aa2acp::bridge::management::query_field;
using aa2acp::bridge::management::random_token;
using aa2acp::bridge::management::render_logs_page;
using aa2acp::bridge::management::render_page;
using aa2acp::bridge::management::send_response;
using aa2acp::bridge::management::wifi_interfaces;

std::unique_ptr<CarPlayWorker> carplay_worker;

using ManagementSnapshot = aa2acp::bridge::management::Snapshot;

struct ManagementState {
  std::mutex mutex;
  ManagementSnapshot snapshot;
};

ManagementState management_state;
std::mutex management_hotspot_password_mutex;
struct PendingManagementHotspotUpdate {
  std::string passphrase;
  std::string ssid;
};
std::optional<PendingManagementHotspotUpdate> pending_management_hotspot_update;

constexpr char kDefaultManagementHotspotPassphrase[] = "changeme";

std::string management_hotspot_ssid(const std::string &interface_name) {
  std::ifstream stream("/sys/class/net/" + interface_name + "/address");
  std::string address;
  std::getline(stream, address);
  std::string suffix;
  for (const char character : address)
    if (std::isxdigit(static_cast<unsigned char>(character)))
      suffix.push_back(static_cast<char>(
          std::toupper(static_cast<unsigned char>(character))));
  if (suffix.size() < 6)
    return "AA2ACP-SETUP-1";
  return "AA2ACP-" + suffix.substr(suffix.size() - 6) + "-1";
}

std::string next_management_hotspot_ssid(const std::string &ssid) {
  const auto separator = ssid.rfind('-');
  if (separator == std::string::npos || separator + 1 == ssid.size())
    return ssid + "-2";
  unsigned int counter{};
  for (std::size_t index = separator + 1; index < ssid.size(); ++index) {
    const auto character = ssid[index];
    if (!std::isdigit(static_cast<unsigned char>(character)) ||
        counter > 99999999U)
      return ssid + "-2";
    counter = counter * 10 + static_cast<unsigned int>(character - '0');
  }
  return ssid.substr(0, separator + 1) + std::to_string(counter + 1);
}

void ensure_management_hotspot_settings(aa2acp::bridge::Config &config) {
  if (config.management_hotspot_ssid.empty() && !config.wifi_interface.empty())
    config.management_hotspot_ssid =
        management_hotspot_ssid(config.wifi_interface);
  if (config.management_hotspot_passphrase.size() < 8)
    config.management_hotspot_passphrase = kDefaultManagementHotspotPassphrase;
}

bool management_hotspot_needs_setup(const aa2acp::bridge::Config &config) {
  return config.management_hotspot_passphrase ==
         kDefaultManagementHotspotPassphrase;
}

void refresh_bluetooth_inventory(ManagementState &state) {
  std::string error;
  const auto devices = aa2acp::bridge::list_bluez_devices(&error);
  std::lock_guard lock(state.mutex);
  if (!error.empty()) {
    state.snapshot.bluetooth_error = error;
    return;
  }
  std::map<std::string, aa2acp::bridge::BluetoothDevice> merged;
  for (const auto &device : state.snapshot.bluetooth_devices)
    merged.emplace(device.address, device);
  for (const auto &device : devices)
    merged.insert_or_assign(device.address, device);
  state.snapshot.bluetooth_devices.clear();
  for (auto &[address, device] : merged)
    state.snapshot.bluetooth_devices.push_back(std::move(device));
  std::sort(state.snapshot.bluetooth_devices.begin(),
            state.snapshot.bluetooth_devices.end(),
            [](const auto &left, const auto &right) {
              if (left.paired != right.paired)
                return left.paired > right.paired;
              return left.address < right.address;
            });
  state.snapshot.bluetooth_error.clear();
}

void refresh_wifi_inventory(ManagementState &state) {
  const auto interfaces = wifi_interfaces();
  if (interfaces.empty())
    return;
  std::lock_guard lock(state.mutex);
  state.snapshot.wifi_interfaces = interfaces;
}

ManagementSnapshot management_snapshot(ManagementState &state) {
  std::lock_guard lock(state.mutex);
  return state.snapshot;
}

void run_bluetooth_scan(ManagementState &state, const std::stop_token stop) {
  const auto set_phase = [&state](const int phase_id,
                                  const std::string &phase) {
    std::lock_guard lock(state.mutex);
    state.snapshot.bluetooth_scan_phase_id = phase_id;
    state.snapshot.bluetooth_scan_phase = phase;
  };
  const auto log = [](const std::string &message) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
        << "Management Bluetooth: " << message << '\n';
  };
  set_phase(2, "LE discovery in progress");
  const auto stop_requested = [stop] { return stop.stop_requested(); };
  aa2acp::bridge::discover_bluez_devices("le", 30, log, stop_requested);
  refresh_bluetooth_inventory(state);
  if (!stop.stop_requested()) {
    set_phase(3, "classic discovery in progress");
    aa2acp::bridge::discover_bluez_devices("bredr", 15, log, stop_requested);
    refresh_bluetooth_inventory(state);
  }
  {
    std::lock_guard lock(state.mutex);
    state.snapshot.bluetooth_scan_running = false;
    state.snapshot.bluetooth_scan_phase_id = 4;
    state.snapshot.bluetooth_scan_phase = "last discovery completed";
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Management Bluetooth: discovery finished\n";
}

int run_carplay_session(const aa2acp::bridge::Config &config,
                        const std::stop_token stop,
                        const std::atomic_bool &phone_disconnected,
                        std::atomic<pid_t> &active_child,
                        const std::string &video_socket = {},
                        const std::string &media_audio_socket = {},
                        const std::string &guidance_audio_socket = {},
                        const std::string &system_audio_socket = {},
                        const bool preflight = false) {
  if (config.head_unit_mac.empty()) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Bridge daemon: configure a head-unit MAC first\n";
    return 2;
  }
  std::vector<std::string> arguments{
      "aa2acp-iap2-bt",
      "--bridge",
      "--mac",
      config.head_unit_mac,
      "--wifi-interface",
      config.wifi_interface,
      "--management-hotspot-ssid",
      config.management_hotspot_ssid,
      "--management-hotspot-passphrase",
      config.management_hotspot_passphrase,
      "--pairing-store",
      config.airplay_pairing_store.string(),
      "--head-unit-capabilities-store",
      aa2acp::bridge::default_head_unit_capabilities_store().string(),
      "--timeout",
      "60"};
  if (preflight)
    arguments.push_back("--preflight");
  if (!video_socket.empty()) {
    arguments.push_back("--video-socket");
    arguments.push_back(video_socket);
  }
  if (!media_audio_socket.empty()) {
    arguments.push_back("--audio-socket");
    arguments.push_back(media_audio_socket);
  }
  if (!guidance_audio_socket.empty()) {
    arguments.push_back("--guidance-audio-socket");
    arguments.push_back(guidance_audio_socket);
  }
  if (!system_audio_socket.empty()) {
    arguments.push_back("--system-audio-socket");
    arguments.push_back(system_audio_socket);
  }
  if (!carplay_worker) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Bridge daemon: CarPlay worker is unavailable\n";
    return 1;
  }
  return carplay_worker->run(
      std::move(arguments), stop, phone_disconnected, active_child,
      {.on_connection_lost =
           [] {
             std::lock_guard state_lock(management_state.mutex);
             management_state.snapshot.pairing_confirmation.reset();
           },
       .on_pairing_confirmation =
           [](const aa2acp::iap2::PairingConfirmationMessage &confirmation) {
             std::lock_guard state_lock(management_state.mutex);
             management_state.snapshot.pairing_confirmation = confirmation;
             management_state.snapshot.carplay_preflight_status =
                 "compare the Bluetooth pairing code in the management UI";
           },
       .on_pairing_reset =
           [] {
             std::lock_guard state_lock(management_state.mutex);
             management_state.snapshot.pairing_confirmation.reset();
           }});
}

void run_carplay_preflight(const aa2acp::bridge::Config config,
                           ManagementState &management,
                           const std::stop_token stop) {
  {
    std::lock_guard lock(management.mutex);
    management.snapshot.carplay_preflight_running = true;
    management.snapshot.carplay_preflight_status = "starting";
  }
  std::atomic<pid_t> active_child{-1};
  std::atomic_bool phone_disconnected{false};
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Management: starting CarPlay preflight for " << config.head_unit_mac
      << '\n';
  const auto result = run_carplay_session(config, stop, phone_disconnected,
                                          active_child, {}, {}, {}, {}, true);
  {
    std::lock_guard lock(management.mutex);
    management.snapshot.carplay_preflight_running = false;
    management.snapshot.carplay_preflight_status =
        result == 0 ? "ready; capabilities cached"
                    : "failed (see daemon log for details)";
  }
  aa2acp::bridge::log(result == 0 ? aa2acp::bridge::LogLevel::info
                                  : aa2acp::bridge::LogLevel::warning)
      << "Management: CarPlay preflight "
      << (result == 0 ? "succeeded" : "failed") << '\n';
}

int run_wired_android_auto_receiver(
    const std::function<aa2acp::bridge::Config()> &config_provider,
    std::shared_ptr<std::atomic_bool> preparation_failed,
    const std::stop_token stop) {
  bool configuration_warning_logged = false;
  while (!stop.stop_requested()) {
    const auto config = config_provider();
    if (!config.head_unit_mac.empty() && !config.wifi_interface.empty())
      break;
    if (!configuration_warning_logged) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Bridge daemon: wired Android Auto is disabled until a head-unit "
             "MAC and Wi-Fi interface are configured\n";
      configuration_warning_logged = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (stop.stop_requested())
    return 0;

  // Snapshot for the session; also captured by value into the detached
  // capabilities thread so nothing references daemon locals.
  const auto session_config = config_provider();

  // Runtime IPC sockets: prefer the runtime directory (systemd services get
  // a private, writable /run/aa2acp via RuntimeDirectory=); fall back to
  // /tmp for plain user runs.
  const auto *runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  const std::filesystem::path socket_dir =
      runtime_dir && *runtime_dir ? std::filesystem::path(runtime_dir)
                                  : std::filesystem::path("/tmp");
  const auto video_socket =
      socket_dir / ("aa2acp-video-" + std::to_string(getpid()) + ".sock");
  const auto media_audio_socket =
      socket_dir / ("aa2acp-media-audio-" + std::to_string(getpid()) + ".sock");
  const auto guidance_audio_socket =
      socket_dir /
      ("aa2acp-guidance-audio-" + std::to_string(getpid()) + ".sock");
  const auto system_audio_socket =
      socket_dir /
      ("aa2acp-system-audio-" + std::to_string(getpid()) + ".sock");
  VideoSocketForwarder forwarder(video_socket);
  if (!forwarder.ready()) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Bridge daemon: unable to listen for Android Auto video\n";
    return 1;
  }
  AudioSocketForwarder media_audio_forwarder(media_audio_socket, "media");
  AudioSocketForwarder guidance_audio_forwarder(guidance_audio_socket,
                                                "guidance");
  AudioSocketForwarder system_audio_forwarder(system_audio_socket, "system");
  if (!media_audio_forwarder.ready() || !guidance_audio_forwarder.ready() ||
      !system_audio_forwarder.ready()) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Bridge daemon: unable to listen for Android Auto audio\n";
    return 1;
  }
  std::atomic_bool carplay_start_requested{};
  std::atomic_bool phone_disconnected{};
  std::atomic<pid_t> active_carplay_child{-1};
  aa2acp::bridge::H264Normalizer h264_normalizer;
  aa2acp::aa::WiredReceiver receiver(
      [&carplay_start_requested, &phone_disconnected, &preparation_failed,
       &active_carplay_child](const auto &event) {
        switch (event.type) {
        case aa2acp::aa::WiredReceiverEventType::waiting_for_phone:
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
              << "Bridge daemon: Android Auto USB idle: " << event.detail
              << '\n';
          break;
        case aa2acp::aa::WiredReceiverEventType::aoap_transport_ready:
          phone_disconnected = false;
          preparation_failed->store(false);
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
              << "Bridge daemon: Android Auto USB transport ready: "
              << event.detail << '\n';
          carplay_start_requested = true;
          break;
        case aa2acp::aa::WiredReceiverEventType::control_session_ready:
          if (aa2acp::bridge::debug_logging_enabled())
            aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
                << "Bridge daemon: Android Auto control session ready: "
                << event.detail << '\n';
          break;
        case aa2acp::aa::WiredReceiverEventType::video_stream_configured:
          if (aa2acp::bridge::debug_logging_enabled())
            aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
                << "Bridge daemon: Android Auto video configured: "
                << event.detail << '\n';
          break;
        case aa2acp::aa::WiredReceiverEventType::video_stream_started:
          if (aa2acp::bridge::debug_logging_enabled())
            aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
                << "Bridge daemon: Android Auto video stream started\n";
          break;
        case aa2acp::aa::WiredReceiverEventType::transport_teardown:
          if (aa2acp::bridge::debug_logging_enabled())
            aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
                << "Bridge daemon: expected Android Auto USB transport "
                   "teardown: "
                << event.detail << '\n';
          break;
        case aa2acp::aa::WiredReceiverEventType::disconnected:
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
              << "Bridge daemon: Android Auto USB disconnected\n";
          phone_disconnected = true;
          if (active_carplay_child.load() > 0) {
            aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
                << "Bridge daemon: stopping CarPlay after Android Auto "
                   "disconnect\n";
          }
          break;
        case aa2acp::aa::WiredReceiverEventType::error:
          if (!phone_disconnected.load())
            aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
                << "Bridge daemon: " << event.detail << '\n';
          else if (aa2acp::bridge::debug_logging_enabled())
            aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
                << "Bridge daemon: ignored Android Auto teardown error: "
                << event.detail << '\n';
          break;
        }
      },
      [&forwarder,
       &h264_normalizer](const std::span<const std::uint8_t> frame) {
        std::string error;
        const auto normalized = h264_normalizer.normalize(frame, &error);
        if (normalized.empty()) {
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
              << "Bridge daemon: " << error << '\n';
          return;
        }
        for (const auto &access_unit : normalized)
          forwarder.push(access_unit);
      },
      [&media_audio_forwarder, &guidance_audio_forwarder,
       &system_audio_forwarder](const aa2acp::aa::AudioStream stream,
                                const std::span<const std::uint8_t> pcm) {
        switch (stream) {
        case aa2acp::aa::AudioStream::media:
          media_audio_forwarder.push(pcm);
          break;
        case aa2acp::aa::AudioStream::guidance:
          guidance_audio_forwarder.push(pcm);
          break;
        case aa2acp::aa::AudioStream::system:
          system_audio_forwarder.push(pcm);
          break;
        }
      },
      [preparation_failed,
       session_config]() -> std::optional<aa2acp::aa::HeadUnitCapabilities> {
        const auto &config = session_config;
        // A cold preflight normally populates this cache. Keep the fallback
        // bounded so a failed CarPlay attempt cannot consume Android Auto's
        // entire connection window. Captures are self-contained (shared_ptr
        // and a config snapshot) because this callable is copied into a
        // detached thread that may outlive the daemon's locals.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
          if (preparation_failed->load())
            return std::nullopt;
          const auto capabilities =
              aa2acp::airplay::load_head_unit_capabilities(
                  aa2acp::bridge::default_head_unit_capabilities_store(),
                  config.head_unit_mac);
          if (capabilities)
            return aa2acp::aa::HeadUnitCapabilities{
                capabilities->width_pixels,
                capabilities->height_pixels,
                capabilities->max_fps,
                capabilities->media_pcm_48k_stereo,
                capabilities->guidance_pcm_16k_mono,
                capabilities->system_pcm_16k_mono};
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (std::chrono::steady_clock::now() < deadline);
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
            << "Bridge daemon: CarPlay capabilities unavailable after 5 "
               "seconds; using 1280x720 fallback\n";
        return std::nullopt;
      });
  std::string error;
  if (!receiver.start(&error)) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Bridge daemon: unable to start Android Auto USB receiver: " << error
        << '\n';
    return 1;
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Bridge daemon: wired Android Auto receiver started\n";
  auto retry_delay = std::chrono::seconds(0);
  while (!stop.stop_requested()) {
    if (!carplay_start_requested.exchange(false)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
        << "Bridge daemon: preparing CarPlay for Android Auto\n";
    const auto config = config_provider();
    if (config.head_unit_mac.empty() || config.wifi_interface.empty()) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Bridge daemon: configure a head-unit MAC and Wi-Fi interface "
             "first\n";
      continue;
    }
    const auto result = run_carplay_session(
        config, stop, phone_disconnected, active_carplay_child, video_socket,
        media_audio_socket, guidance_audio_socket, system_audio_socket);
    if (stop.stop_requested())
      break;
    if (phone_disconnected.load()) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
          << "Bridge daemon: CarPlay session stopped after Android Auto "
             "disconnect\n";
      continue;
    }
    if (result != 0)
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Bridge daemon: CarPlay video session ended with " << result
          << '\n';
    else
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
          << "Bridge daemon: CarPlay video session ended\n";
    if (result == 0) {
      retry_delay = std::chrono::seconds(0);
    } else {
      preparation_failed->store(true);
      if (retry_delay.count() == 0)
        retry_delay = std::chrono::seconds(1);
      else
        retry_delay = std::min(retry_delay * 2, std::chrono::seconds(30));
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Bridge daemon: retrying CarPlay in " << retry_delay.count()
          << " seconds\n";
      for (auto remaining = retry_delay;
           remaining > std::chrono::seconds(0) && !stop.stop_requested() &&
           !phone_disconnected.load();
           remaining -= std::chrono::seconds(1))
        std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!phone_disconnected.load() && !stop.stop_requested())
        carplay_start_requested = true;
    }
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Bridge daemon: stopping wired Android Auto receiver\n";
  receiver.stop();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  auto carplay_preparation_failed = std::make_shared<std::atomic_bool>(false);
  std::cout.setf(std::ios::unitbuf);
  std::cerr.setf(std::ios::unitbuf);
  bool file_logging = true;
  for (int index = 1; index < argc; ++index)
    if (std::string_view(argv[index]) == "--no-file-log")
      file_logging = false;
  // Start this process before installing the daemon tee. Its output is fed
  // through that tee by the parent, so inheriting the tee would prefix it
  // twice.
  carplay_worker = std::make_unique<CarPlayWorker>();
  if (!carplay_worker) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to start CarPlay worker\n";
    return 1;
  }
  // Process-lifetime allocations: detached helper threads (e.g. the
  // capabilities provider) may log during the final seconds of shutdown,
  // after main's locals would already be destroyed.
  auto *recent_log = new RecentLog();
  const auto log_path = file_logging ? next_daemon_log_path()
                                     : std::optional<std::filesystem::path>{};
  auto *daemon_log = new DaemonLog(*recent_log, log_path);
  (void)daemon_log; // process-lifetime by design; reclaimed at exit
  if (log_path)
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
        << "Bridge daemon: logging to " << *log_path << '\n';
  std::filesystem::path config_path = aa2acp::bridge::default_config_path();
  int port = 8080;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--config" && index + 1 < argc)
      config_path = argv[++index];
    else if (argument == "--port" && index + 1 < argc) {
      try {
        port = static_cast<int>(std::stoul(argv[++index]));
        if (port <= 0 || port > 65535)
          port = 8080;
      } catch (const std::exception &) {
        port = 8080;
      }
    } else if (argument == "--no-file-log")
      continue;
    else {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "usage: aa2acp-bridge-daemon [--config PATH] [--port PORT] "
             "[--no-file-log]\n";
      return 2;
    }
  }
  auto config =
      aa2acp::bridge::load_config(config_path)
          .value_or(aa2acp::bridge::Config{
              "", "", "", "", aa2acp::bridge::default_airplay_pairing_store()});
  if (config.airplay_pairing_store.empty())
    config.airplay_pairing_store =
        aa2acp::bridge::default_airplay_pairing_store();
  std::mutex config_mutex;
  refresh_bluetooth_inventory(management_state);
  refresh_wifi_inventory(management_state);
  if (config.wifi_interface.empty()) {
    const auto snapshot = management_snapshot(management_state);
    if (!snapshot.wifi_interfaces.empty()) {
      config.wifi_interface = snapshot.wifi_interfaces.front();
    }
  }
  ensure_management_hotspot_settings(config);
  if (!config.wifi_interface.empty() &&
      !aa2acp::bridge::save_config(config_path, config))
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
        << "Bridge daemon: unable to persist management hotspot settings\n";
  const auto hotspot_started =
      !config.wifi_interface.empty() &&
      aa2acp::iap2::start_management_hotspot(
          config.wifi_interface, config.management_hotspot_ssid,
          config.management_hotspot_passphrase);
  if (!hotspot_started)
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Bridge daemon: unable to start management hotspot\n";
  else if (management_hotspot_needs_setup(config))
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
        << "Bridge daemon: management hotspot default password is '"
        << kDefaultManagementHotspotPassphrase
        << "'; change it at the management UI before using AA2ACP\n";
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to block shutdown signals\n";
    return 1;
  }
  std::jthread wifi_refresh_worker([](std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      refresh_wifi_inventory(management_state);
      for (int count = 0; count < 20 && !stop_token.stop_requested(); ++count)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  });
  const int signal_fd = signalfd(-1, &signals, SFD_CLOEXEC);
  if (signal_fd < 0) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to create shutdown signal descriptor\n";
    return 1;
  }
  std::jthread android_auto_worker(
      [&config, &config_mutex, preparation_failed = carplay_preparation_failed](
          const std::stop_token stop) {
        run_wired_android_auto_receiver(
            [&config, &config_mutex] {
              std::lock_guard lock(config_mutex);
              return config;
            },
            preparation_failed, stop);
      });
  std::jthread carplay_preflight_worker;
  std::jthread bluetooth_scan_worker;
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  int enabled = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (listener < 0 ||
      bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
          0 ||
      listen(listener, 8) != 0) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to listen on 0.0.0.0:" << port << '\n';
    close(signal_fd);
    return 1;
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Bridge management UI listening on http://0.0.0.0:" << port << '\n';
  const auto csrf_token = random_token();
  const auto handle_client = [&](const int client) {
    std::array<char, 4096> buffer{};
    std::string request;
    const auto request_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto receive_more = [&] {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              request_deadline - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds::zero())
        return false;
      pollfd descriptor{client, POLLIN, 0};
      if (poll(&descriptor, 1,
               static_cast<int>(
                   std::min(remaining, std::chrono::milliseconds(5000))
                       .count())) <= 0 ||
          (descriptor.revents & POLLIN) == 0)
        return false;
      const auto count = recv(client, buffer.data(), buffer.size(), 0);
      if (count <= 0)
        return false;
      request.append(buffer.data(), static_cast<std::size_t>(count));
      return true;
    };
    while (request.find("\r\n\r\n") == std::string::npos &&
           request.size() <= 16 * 1024) {
      if (!receive_more()) {
        close(client);
        return;
      }
    }
    const auto header_end = request.find("\r\n\r\n");
    const auto content_marker = request.find("Content-Length: ");
    if (header_end != std::string::npos &&
        content_marker != std::string::npos) {
      const auto start = content_marker + 16;
      const auto end = request.find("\r\n", start);
      std::size_t length{};
      const auto text =
          end == std::string::npos
              ? std::string_view{}
              : std::string_view(request).substr(start, end - start);
      const auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), length);
      if (text.empty() || parsed.ec != std::errc{} ||
          parsed.ptr != text.data() + text.size() || length > 16 * 1024) {
        close(client);
        return;
      }
      while (request.size() < header_end + 4 + length) {
        if (!receive_more()) {
          close(client);
          return;
        }
      }
    }
    const auto split = request.find("\r\n\r\n");
    const auto body =
        split == std::string::npos ? "" : request.substr(split + 4);
    const auto request_line_end = request.find("\r\n");
    const auto request_line = request.substr(0, request_line_end);
    const auto respond = [&](const int status, const char *type,
                             const std::string &response_body,
                             const std::string &extra = {}) {
      if (aa2acp::bridge::debug_logging_enabled())
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
            << "Management: " << request_line << " -> " << status << '\n';
      return send_response(client, status, type, response_body, extra);
    };
    if (request.starts_with("GET / ") || request.starts_with("GET /?")) {
      const bool saved = request.find("saved=1") != std::string::npos;
      const bool carplay_error =
          request.find("carplay_error=1") != std::string::npos;
      const auto snapshot = management_snapshot(management_state);
      const auto configuration = [&] {
        std::lock_guard lock(config_mutex);
        return config;
      }();
      respond(200, "text/html; charset=utf-8",
              render_page(configuration, snapshot,
                          management_hotspot_needs_setup(configuration), saved,
                          carplay_error, csrf_token));
    } else if (request.starts_with("GET /logs")) {
      respond(200, "text/html; charset=utf-8",
              render_logs_page(recent_log->snapshot(),
                               RecentLog::kMaximumBytes / 1024),
              "Cache-Control: no-store\r\n");
    } else if (request.starts_with("POST ") &&
               form_field(body, "csrf") != csrf_token) {
      respond(403, "text/plain", "Invalid CSRF token\n");
    } else if (request.starts_with("GET /scan-status")) {
      const auto snapshot = management_snapshot(management_state);
      const auto phase = query_field(request, "phase");
      const int status =
          snapshot.bluetooth_scan_running && phase &&
                  *phase == std::to_string(snapshot.bluetooth_scan_phase_id)
              ? 204
              : 205;
      respond(status, "text/plain", "");
    } else if (request.starts_with("GET /carplay-prepare-status")) {
      const auto snapshot = management_snapshot(management_state);
      respond(snapshot.carplay_preflight_running &&
                      !snapshot.pairing_confirmation
                  ? 204
                  : 205,
              "text/plain", snapshot.carplay_preflight_status);
    } else if (request.starts_with("POST /management-hotspot ")) {
      const auto passphrase = form_field(body, "management_hotspot_passphrase");
      const auto confirmation =
          form_field(body, "management_hotspot_passphrase_confirm");
      const auto change_ssid =
          form_field(body, "management_hotspot_change_ssid").has_value();
      if (!passphrase || !confirmation || *passphrase != *confirmation) {
        respond(400, "text/plain", "Passwords do not match\n");
      } else if (*passphrase == kDefaultManagementHotspotPassphrase ||
                 passphrase->size() < 8 ||
                 passphrase->find_first_of("\r\n=") != std::string::npos) {
        respond(400, "text/plain", "Choose a different valid password\n");
      } else {
        const auto current = [&] {
          std::lock_guard lock(config_mutex);
          return config;
        }();
        const auto ssid =
            change_ssid
                ? next_management_hotspot_ssid(current.management_hotspot_ssid)
                : current.management_hotspot_ssid;
        {
          std::lock_guard lock(management_hotspot_password_mutex);
          pending_management_hotspot_update = {*passphrase, ssid};
        }
        {
          std::lock_guard lock(management_state.mutex);
          management_state.snapshot.management_hotspot_password_pending = true;
          management_state.snapshot.pending_management_hotspot_ssid = ssid;
        }
        respond(303, "text/plain", "", "Location: /\r\n");
      }
    } else if (request.starts_with("POST /management-hotspot/apply ")) {
      std::optional<PendingManagementHotspotUpdate> update;
      {
        std::lock_guard lock(management_hotspot_password_mutex);
        update = std::move(pending_management_hotspot_update);
        pending_management_hotspot_update.reset();
      }
      const auto previous = [&] {
        std::lock_guard lock(config_mutex);
        return config;
      }();
      if (!update) {
        respond(400, "text/plain", "No password update is pending\n");
      } else {
        auto updated = previous;
        updated.management_hotspot_passphrase = update->passphrase;
        updated.management_hotspot_ssid = update->ssid;
        if (!aa2acp::bridge::save_config(config_path, updated)) {
          respond(400, "text/plain", "Invalid hotspot password\n");
        } else {
          {
            std::lock_guard lock(config_mutex);
            config = updated;
          }
          {
            std::lock_guard lock(management_state.mutex);
            management_state.snapshot.management_hotspot_password_pending =
                false;
            management_state.snapshot.pending_management_hotspot_ssid.clear();
          }
          respond(200, "text/html",
                  "<!doctype html><title>AA2ACP</title><p>Password updated. "
                  "This device will now disconnect from the hotspot.</p><p>"
                  "Reconnect to Wi-Fi network <b>" +
                      html_escape(updated.management_hotspot_ssid) +
                      "</b>, reconnect using the new password, then <a "
                      "href=\"/\">continue</a>.</p>");
          aa2acp::iap2::start_management_hotspot(
              updated.wifi_interface, updated.management_hotspot_ssid,
              updated.management_hotspot_passphrase);
        }
      }
    } else if ([&] {
                 std::lock_guard lock(config_mutex);
                 return management_hotspot_needs_setup(config);
               }()) {
      respond(403, "text/plain",
              "Change the default management hotspot password first\n");
    } else if (request.starts_with("POST /carplay-prepare ")) {
      const auto selected_config = [&] {
        std::lock_guard lock(config_mutex);
        return config;
      }();
      bool start_preflight = false;
      {
        std::lock_guard lock(management_state.mutex);
        if (!management_state.snapshot.carplay_preflight_running &&
            !management_state.snapshot.bluetooth_scan_running) {
          management_state.snapshot.carplay_preflight_running = true;
          management_state.snapshot.carplay_preflight_status = "queued";
          start_preflight = true;
        }
      }
      if (start_preflight && !selected_config.head_unit_mac.empty() &&
          !selected_config.wifi_interface.empty()) {
        // The running flag is cleared by the worker itself before the thread
        // object becomes non-joinable; join first or move-assigning over a
        // joinable jthread calls std::terminate.
        if (carplay_preflight_worker.joinable())
          carplay_preflight_worker.join();
        carplay_preflight_worker =
            std::jthread([selected_config](const std::stop_token stop) {
              run_carplay_preflight(selected_config, management_state, stop);
            });
        respond(303, "text/plain", "", "Location: /\r\n");
      } else {
        {
          std::lock_guard lock(management_state.mutex);
          management_state.snapshot.carplay_preflight_running = false;
          management_state.snapshot.carplay_preflight_status =
              management_state.snapshot.bluetooth_scan_running
                  ? "wait for Bluetooth scanning to finish first"
                  : "configure a head-unit MAC and Wi-Fi interface first";
        }
        const auto location = selected_config.head_unit_mac.empty() ||
                                      selected_config.wifi_interface.empty()
                                  ? "Location: /?carplay_error=1\r\n"
                                  : "Location: /\r\n";
        respond(303, "text/plain", "", location);
      }
    } else if (request.starts_with("POST /bluetooth-confirm ")) {
      const auto id = form_field(body, "id");
      const auto decision = form_field(body, "decision");
      std::optional<aa2acp::iap2::PairingConfirmationMessage> confirmation;
      if (id && decision && (*decision == "confirm" || *decision == "reject")) {
        std::lock_guard lock(management_state.mutex);
        if (management_state.snapshot.pairing_confirmation &&
            *id == std::to_string(
                       management_state.snapshot.pairing_confirmation->id)) {
          confirmation = management_state.snapshot.pairing_confirmation;
          management_state.snapshot.pairing_confirmation.reset();
        }
      }
      bool accepted = false;
      if (confirmation && carplay_worker) {
        accepted = carplay_worker->answer_pairing_confirmation(
            *confirmation, *decision == "confirm");
      }
      if (!accepted) {
        if (confirmation) {
          std::lock_guard lock(management_state.mutex);
          if (!management_state.snapshot.pairing_confirmation)
            management_state.snapshot.pairing_confirmation = *confirmation;
        }
        respond(409, "text/plain",
                "No matching Bluetooth confirmation is pending\n");
      } else {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
            << "Management: Bluetooth pairing confirmation " << *decision
            << " requested\n";
        respond(303, "text/plain", "", "Location: /\r\n");
      }
    } else if (request.starts_with("POST /bluetooth-forget ")) {
      const auto selected_config = [&] {
        std::lock_guard lock(config_mutex);
        return config;
      }();
      const auto snapshot = management_snapshot(management_state);
      std::string error;
      if (selected_config.head_unit_mac.empty()) {
        respond(400, "text/plain", "No configured Bluetooth device\n");
      } else if (snapshot.carplay_preflight_running) {
        respond(409, "text/plain",
                "Cannot forget a Bluetooth bond while CarPlay preparation is "
                "running\n");
      } else if (!aa2acp::bridge::forget_bluez_device(
                     selected_config.head_unit_mac, &error)) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
            << "Management: unable to forget Bluetooth bond for "
            << selected_config.head_unit_mac << ": " << error << '\n';
        respond(400, "text/plain",
                "Unable to forget Bluetooth bond: " + error + "\n");
      } else {
        refresh_bluetooth_inventory(management_state);
        {
          std::lock_guard lock(management_state.mutex);
          management_state.snapshot.carplay_preflight_status =
              "local Bluetooth bond removed; clear or restart pairing on the "
              "head unit if needed";
        }
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
            << "Management: removed Bluetooth bond for "
            << selected_config.head_unit_mac << '\n';
        respond(303, "text/plain", "", "Location: /\r\n");
      }
    } else if (request.starts_with("POST /scan ")) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
          << "Management: Bluetooth scan requested\n";
      bool start_scan = false;
      {
        std::lock_guard lock(management_state.mutex);
        if (!management_state.snapshot.bluetooth_scan_running) {
          management_state.snapshot.bluetooth_scan_running = true;
          management_state.snapshot.bluetooth_scan_phase_id = 1;
          management_state.snapshot.bluetooth_scan_phase = "queued";
          start_scan = true;
        }
      }
      if (start_scan) {
        // Same join-before-assign rationale as carplay_preflight_worker.
        if (bluetooth_scan_worker.joinable())
          bluetooth_scan_worker.join();
        bluetooth_scan_worker = std::jthread([](const std::stop_token stop) {
          run_bluetooth_scan(management_state, stop);
        });
      }
      respond(303, "text/plain", "", "Location: /\r\n");
    } else if (request.starts_with("POST /display ")) {
      const bool show_unnamed = form_field(body, "show_unnamed").has_value();
      {
        std::lock_guard lock(management_state.mutex);
        management_state.snapshot.show_unnamed_bluetooth_devices = show_unnamed;
      }
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
          << "Management: unnamed Bluetooth devices "
          << (show_unnamed ? "shown" : "hidden") << '\n';
      respond(303, "text/plain", "", "Location: /\r\n");
    } else if (request.starts_with("POST /config ")) {
      const auto manual = form_field(body, "manual_mac");
      const auto selected = form_field(body, "head_unit_mac");
      const auto wifi = form_field(body, "wifi_interface");
      const auto hotspot_ssid = form_field(body, "management_hotspot_ssid");
      const auto hotspot_passphrase =
          form_field(body, "management_hotspot_passphrase");
      const auto hotspot_passphrase_confirm =
          form_field(body, "management_hotspot_passphrase_confirm");
      const auto previous = [&] {
        std::lock_guard lock(config_mutex);
        return config;
      }();
      const auto mac = selected && !selected->empty() ? *selected
                       : manual && !manual->empty()   ? *manual
                                                      : previous.head_unit_mac;
      const auto new_hotspot_password =
          hotspot_passphrase && !hotspot_passphrase->empty();
      const auto effective_hotspot_passphrase =
          new_hotspot_password ? *hotspot_passphrase
                               : previous.management_hotspot_passphrase;
      if (wifi && hotspot_ssid &&
          (!new_hotspot_password ||
           (hotspot_passphrase_confirm &&
            *hotspot_passphrase_confirm == *hotspot_passphrase)) &&
          aa2acp::bridge::save_config(config_path,
                                      {mac, *wifi, *hotspot_ssid,
                                       effective_hotspot_passphrase,
                                       previous.airplay_pairing_store})) {
        if (mac != previous.head_unit_mac) {
          std::error_code error;
          const auto capabilities_store =
              aa2acp::bridge::default_head_unit_capabilities_store();
          if (std::filesystem::remove(capabilities_store, error)) {
            aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
                << "Management: invalidated cached head-unit capabilities "
                   "after head-unit change\n";
          } else if (error) {
            aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
                << "Management: unable to invalidate cached head-unit "
                   "capabilities: "
                << error.message() << '\n';
          }
        }
        {
          std::lock_guard lock(config_mutex);
          config = {mac, *wifi, *hotspot_ssid, effective_hotspot_passphrase,
                    previous.airplay_pairing_store};
        }
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
            << "Management: saved head unit " << mac << " on " << *wifi << '\n';
        respond(303, "text/plain", "", "Location: /?saved=1\r\n");
      } else {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
            << "Management: rejected invalid configuration\n";
        respond(400, "text/plain", "Invalid configuration\n");
      }
    } else {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "Management: unknown request\n";
      respond(400, "text/plain", "Unknown endpoint\n");
    }
    close(client);
  };

  constexpr std::size_t kManagementWorkerCount = 4;
  constexpr std::size_t kManagementQueueLimit = 16;
  std::mutex request_queue_mutex;
  std::condition_variable request_queue_ready;
  std::deque<int> request_queue;
  bool request_queue_stopping{};
  std::vector<std::jthread> request_workers;
  std::array<std::atomic<int>, kManagementWorkerCount> active_clients;
  for (auto &client : active_clients)
    client.store(-1);
  request_workers.reserve(kManagementWorkerCount);
  for (std::size_t index = 0; index < kManagementWorkerCount; ++index) {
    request_workers.emplace_back([&, index] {
      for (;;) {
        int client{};
        {
          std::unique_lock lock(request_queue_mutex);
          request_queue_ready.wait(lock, [&] {
            return request_queue_stopping || !request_queue.empty();
          });
          if (request_queue_stopping)
            return;
          client = request_queue.front();
          request_queue.pop_front();
          active_clients[index].store(client);
        }
        handle_client(client);
        active_clients[index].store(-1);
      }
    });
  }

  for (;;) {
    pollfd descriptors[]{{listener, POLLIN, 0}, {signal_fd, POLLIN, 0}};
    if (poll(descriptors, 2, -1) <= 0)
      continue;
    if ((descriptors[1].revents & POLLIN) != 0) {
      signalfd_siginfo signal_info{};
      if (read(signal_fd, &signal_info, sizeof(signal_info)) ==
          static_cast<ssize_t>(sizeof(signal_info))) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
            << "Bridge daemon: graceful shutdown requested\n";
        break;
      }
      continue;
    }
    if ((descriptors[0].revents & POLLIN) == 0)
      continue;
    const int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0)
      continue;
    {
      std::lock_guard lock(request_queue_mutex);
      if (request_queue.size() >= kManagementQueueLimit) {
        close(client);
        continue;
      }
      request_queue.push_back(client);
    }
    request_queue_ready.notify_one();
  }
  {
    std::lock_guard lock(request_queue_mutex);
    request_queue_stopping = true;
    for (const int client : request_queue)
      close(client);
    request_queue.clear();
  }
  request_queue_ready.notify_all();
  for (auto &client : active_clients) {
    const auto fd = client.load();
    if (fd >= 0)
      shutdown(fd, SHUT_RDWR);
  }
  request_workers.clear();
  close(listener);
  carplay_preflight_worker.request_stop();
  bluetooth_scan_worker.request_stop();
  if (carplay_preflight_worker.joinable())
    carplay_preflight_worker.join();
  if (bluetooth_scan_worker.joinable())
    bluetooth_scan_worker.join();
  android_auto_worker.request_stop();
  android_auto_worker.join();
  close(signal_fd);
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Bridge daemon: stopped\n";
}
