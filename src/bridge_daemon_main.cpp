#include "acp/aa/wired_receiver.hpp"
#include "acp/bridge/bluez_inventory.hpp"
#include "acp/bridge/config.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace {

struct ManagementSnapshot {
  std::vector<acp::bridge::BluetoothDevice> bluetooth_devices;
  std::vector<std::string> wifi_interfaces;
  bool show_unnamed_bluetooth_devices{};
  bool bluetooth_scan_running{};
  int bluetooth_scan_phase_id{};
  std::string bluetooth_scan_phase{"idle"};
  std::string bluetooth_error;
};

struct ManagementState {
  std::mutex mutex;
  ManagementSnapshot snapshot;
};

ManagementState management_state;

std::string normalized_bluetooth_address(const std::string &value) {
  std::string normalized;
  for (const unsigned char character : value) {
    if (std::isxdigit(character))
      normalized += static_cast<char>(std::toupper(character));
  }
  return normalized;
}

bool is_unnamed(const acp::bridge::BluetoothDevice &device) {
  // BlueZ commonly uses the address itself as Alias when no advertised name is
  // available, with either colons or hyphens as separators. Treat that as
  // unnamed rather than displaying the address twice.
  return device.name.empty() ||
         normalized_bluetooth_address(device.name) ==
             normalized_bluetooth_address(device.address);
}

std::string html_escape(const std::string &value) {
  std::string escaped;
  for (const char character : value) {
    switch (character) {
    case '&':
      escaped += "&amp;";
      break;
    case '<':
      escaped += "&lt;";
      break;
    case '>':
      escaped += "&gt;";
      break;
    case '\"':
      escaped += "&quot;";
      break;
    case '\'':
      escaped += "&#39;";
      break;
    default:
      escaped += character;
      break;
    }
  }
  return escaped;
}

std::string url_decode(const std::string &value) {
  std::string decoded;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '+') {
      decoded += ' ';
    } else if (value[index] == '%' && index + 2 < value.size() &&
               std::isxdigit(static_cast<unsigned char>(value[index + 1])) &&
               std::isxdigit(static_cast<unsigned char>(value[index + 2]))) {
      const auto hex = value.substr(index + 1, 2);
      decoded += static_cast<char>(std::stoi(hex, nullptr, 16));
      index += 2;
    } else {
      decoded += value[index];
    }
  }
  return decoded;
}

std::optional<std::string> form_field(const std::string &body,
                                      const std::string &wanted) {
  std::size_t start = 0;
  while (start <= body.size()) {
    const auto end = body.find('&', start);
    const auto field = body.substr(start, end - start);
    const auto separator = field.find('=');
    if (url_decode(field.substr(0, separator)) == wanted)
      return separator == std::string::npos
                 ? ""
                 : url_decode(field.substr(separator + 1));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return std::nullopt;
}

std::optional<std::string> query_field(const std::string &request,
                                       const std::string &wanted) {
  const auto method_end = request.find(' ');
  const auto path_end = method_end == std::string::npos
                            ? std::string::npos
                            : request.find(' ', method_end + 1);
  const auto query_start = request.find('?');
  if (method_end == std::string::npos || path_end == std::string::npos ||
      query_start == std::string::npos || query_start >= path_end)
    return std::nullopt;
  return form_field(request.substr(query_start + 1, path_end - query_start - 1),
                    wanted);
}

bool send_response(const int client, const int status, const char *type,
                   const std::string &body, const std::string &extra = {}) {
  const std::string response =
      "HTTP/1.1 " + std::to_string(status) +
      (status == 200   ? " OK\r\n"
       : status == 204 ? " No Content\r\n"
       : status == 205 ? " Reset Content\r\n"
       : status == 303 ? " See Other\r\n"
                       : " Bad Request\r\n") +
      "Content-Type: " + type + "\r\n" + extra +
      "Content-Length: " + std::to_string(body.size()) +
      "\r\nConnection: close\r\n\r\n" + body;
  return send(client, response.data(), response.size(), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(response.size());
}

std::vector<std::string> wifi_interfaces() {
  std::vector<std::string> interfaces;
  FILE *stream = popen("nmcli -t -f DEVICE,TYPE device status", "r");
  std::array<char, 256> line{};
  while (stream != nullptr &&
         fgets(line.data(), line.size(), stream) != nullptr) {
    std::string value(line.data());
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
      value.pop_back();
    const auto separator = value.find(':');
    if (separator != std::string::npos && value.substr(separator + 1) == "wifi")
      interfaces.push_back(value.substr(0, separator));
  }
  if (stream != nullptr)
    pclose(stream);
  return interfaces;
}

void refresh_bluetooth_inventory(ManagementState &state) {
  std::string error;
  const auto devices = acp::bridge::list_bluez_devices(&error);
  std::lock_guard lock(state.mutex);
  if (!error.empty()) {
    state.snapshot.bluetooth_error = error;
    return;
  }
  std::map<std::string, acp::bridge::BluetoothDevice> merged;
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

void run_bluetooth_scan(ManagementState &state) {
  const auto set_phase = [&state](const int phase_id,
                                  const std::string &phase) {
    std::lock_guard lock(state.mutex);
    state.snapshot.bluetooth_scan_phase_id = phase_id;
    state.snapshot.bluetooth_scan_phase = phase;
  };
  const auto log = [](const std::string &message) {
    std::cout << "Management Bluetooth: " << message << '\n';
  };
  set_phase(2, "LE discovery in progress");
  acp::bridge::discover_bluez_devices("le", 30, log);
  refresh_bluetooth_inventory(state);
  set_phase(3, "classic discovery in progress");
  acp::bridge::discover_bluez_devices("bredr", 15, log);
  refresh_bluetooth_inventory(state);
  {
    std::lock_guard lock(state.mutex);
    state.snapshot.bluetooth_scan_running = false;
    state.snapshot.bluetooth_scan_phase_id = 4;
    state.snapshot.bluetooth_scan_phase = "last discovery completed";
  }
  std::cout << "Management Bluetooth: discovery finished\n";
}

std::string page(const acp::bridge::Config &config,
                 const ManagementSnapshot &snapshot, const bool saved) {
  std::string output =
      "<!doctype html><html><head><meta name=\"viewport\" "
      "content=\"width=device-width,initial-scale=1\">"
      "<title>ACP-AA Bridge</title><style>body{font:16px "
      "sans-serif;max-width:38rem;margin:3rem auto;padding:0 1rem}"
      "label,select,input:not([type=checkbox]){display:block;width:100%;box-"
      "sizing:border-box;"
      "margin:.5rem 0}button{padding:.6rem 1rem;margin:.25rem 0}"
      "input[type=checkbox]{width:auto;margin:0 .4rem 0 0}"
      ".hint{color:#555}.status{padding:.6rem;background:#eef7ee}</style></"
      "head><body data-scan-running=\"" +
      std::string(snapshot.bluetooth_scan_running ? "1" : "0") +
      "\" data-scan-phase=\"" +
      std::to_string(snapshot.bluetooth_scan_phase_id) +
      "\">"
      "<h1>ACP-AA Bridge</h1><p>Configure the pinned CarPlay head unit.</p>";
  if (saved)
    output += "<p class=status>Configuration saved.</p>";
  if (!snapshot.bluetooth_error.empty())
    output += "<p class=hint>Bluetooth refresh failed: " +
              html_escape(snapshot.bluetooth_error) + "</p>";
  output +=
      "<form id=\"scan-form\" method=post action=\"/scan\"></form>"
      "<form id=\"display-form\" method=post action=\"/display\"></form>"
      "<form method=post action=\"/config\"><div id=\"bluetooth-picker\">";
  if (snapshot.bluetooth_scan_running) {
    output += "<input type=hidden name=\"head_unit_mac\" value=\"\"><p "
              "class=status>Bluetooth " +
              html_escape(snapshot.bluetooth_scan_phase) + "…</p>";
  } else {
    output += "<label>Known or discovered Bluetooth device<select "
              "name=\"head_unit_mac\">"
              "<option value=\"\">Keep the configured device (use manual field "
              "below)</option>";
    for (const auto &device : snapshot.bluetooth_devices) {
      if (is_unnamed(device) && !snapshot.show_unnamed_bluetooth_devices)
        continue;
      auto name = is_unnamed(device) ? "Unnamed device" : device.name;
      output += "<option value=\"" + html_escape(device.address) + "\">" +
                html_escape(name) + " — " + html_escape(device.address) +
                (device.paired ? " (paired)" : "") +
                (device.connected ? " (connected)" : "") + "</option>";
    }
    output +=
        "</select></label><label><input id=\"show-unnamed\" "
        "form=\"display-form\" "
        "type=checkbox name=\"show_unnamed\" value=\"1\"" +
        std::string(snapshot.show_unnamed_bluetooth_devices ? " checked" : "") +
        ">Show unnamed Bluetooth devices</label><button form=\"scan-form\" "
        "type=submit>Rescan Bluetooth devices</button>";
  }
  output +=
      "</div><label>Manual Bluetooth MAC<input name=\"manual_mac\" value=\"" +
      html_escape(config.head_unit_mac) +
      "\" placeholder=\"[redacted-device-address]\"></label>"
      "<label>Wi-Fi interface<select name=\"wifi_interface\">";
  bool current_interface_present = false;
  if (config.wifi_interface.empty())
    output += "<option selected disabled value=\"\">Select a Wi-Fi "
              "interface</option>";
  for (const auto &interface : snapshot.wifi_interfaces) {
    const auto selected = interface == config.wifi_interface ? " selected" : "";
    current_interface_present =
        current_interface_present || interface == config.wifi_interface;
    output += "<option value=\"" + html_escape(interface) + "\"" + selected +
              ">" + html_escape(interface) + "</option>";
  }
  if (!config.wifi_interface.empty() && !current_interface_present)
    output += "<option selected value=\"" + html_escape(config.wifi_interface) +
              "\">" + html_escape(config.wifi_interface) +
              " (configured)</option>";
  output +=
      "</select></label><button type=submit>Save configuration</button></form>"
      "<script>(()=>{const filter=document.querySelector('#show-unnamed');"
      "if(filter)filter.addEventListener('change',()=>filter.form."
      "requestSubmit());"
      "if(document.body.dataset.scanRunning!=='1')return;"
      "const phase=encodeURIComponent(document.body.dataset.scanPhase);"
      "const poll=async()=>{try{const response=await "
      "fetch('/scan-status?phase='+phase);"
      "if(response.status===205){location.reload();return;}setTimeout(poll,"
      "1000);"
      "}catch{setTimeout(poll,2000);}};poll();})();</script></"
      "body></html>";
  return output;
}

class VideoSocketForwarder {
public:
  explicit VideoSocketForwarder(const std::filesystem::path &path)
      : path_(path) {
    listener_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener_ < 0 ||
        path_.string().size() >= sizeof(sockaddr_un::sun_path)) {
      close_listener();
      return;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto value = path_.string();
    std::copy(value.begin(), value.end(), address.sun_path);
    unlink(value.c_str());
    if (bind(listener_, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) != 0 ||
        listen(listener_, 1) != 0) {
      close_listener();
      unlink(value.c_str());
      return;
    }
    worker_ = std::jthread([this](std::stop_token stop) { forward(stop); });
  }

  ~VideoSocketForwarder() {
    worker_.request_stop();
    close_listener();
    frames_ready_.notify_all();
    if (!path_.empty())
      unlink(path_.c_str());
  }

  bool ready() const { return listener_ >= 0; }

  void push(const std::span<const std::uint8_t> access_unit) {
    Bytes frame(access_unit.begin(), access_unit.end());
    {
      std::lock_guard lock(mutex_);
      for (const auto &nalu : nalus(frame)) {
        if (nalu.empty())
          continue;
        const auto type = nalu[0] & 0x1f;
        if (type == 7)
          sps_ = nalu;
        else if (type == 8)
          pps_ = nalu;
      }
      if (frames_.size() == 120)
        frames_.pop_front();
      frames_.push_back(std::move(frame));
    }
    frames_ready_.notify_one();
  }

private:
  using Bytes = std::vector<std::uint8_t>;

  static std::vector<Bytes> nalus(const Bytes &input) {
    std::vector<Bytes> result;
    for (std::size_t offset = 0; offset + 3 <= input.size();) {
      const auto three = input[offset] == 0 && input[offset + 1] == 0 &&
                         input[offset + 2] == 1;
      const auto four = offset + 4 <= input.size() && input[offset] == 0 &&
                        input[offset + 1] == 0 && input[offset + 2] == 0 &&
                        input[offset + 3] == 1;
      if (!three && !four) {
        ++offset;
        continue;
      }
      const auto start = offset + (four ? 4U : 3U);
      auto end = start;
      while (end + 3 <= input.size()) {
        if ((end + 4 <= input.size() && input[end] == 0 &&
             input[end + 1] == 0 && input[end + 2] == 0 &&
             input[end + 3] == 1) ||
            (input[end] == 0 && input[end + 1] == 0 && input[end + 2] == 1))
          break;
        ++end;
      }
      if (start < end)
        result.emplace_back(input.begin() + static_cast<std::ptrdiff_t>(start),
                            input.begin() + static_cast<std::ptrdiff_t>(end));
      offset = end;
    }
    return result;
  }

  static bool send_all(const int socket_fd,
                       const std::span<const std::uint8_t> bytes) {
    for (std::size_t offset = 0; offset < bytes.size();) {
      const auto count = send(socket_fd, bytes.data() + offset,
                              bytes.size() - offset, MSG_NOSIGNAL);
      if (count <= 0)
        return false;
      offset += static_cast<std::size_t>(count);
    }
    return true;
  }

  static bool send_frame(const int socket_fd, const Bytes &frame) {
    const auto size = frame.size();
    const std::array<std::uint8_t, 4> header{
        static_cast<std::uint8_t>(size >> 24),
        static_cast<std::uint8_t>(size >> 16),
        static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
    return send_all(socket_fd, header) && send_all(socket_fd, frame);
  }

  void forward(const std::stop_token stop) {
    while (!stop.stop_requested()) {
      pollfd descriptor{listener_, POLLIN, 0};
      if (poll(&descriptor, 1, 100) <= 0 || (descriptor.revents & POLLIN) == 0)
        continue;
      const auto client = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
      if (client < 0)
        continue;
      Bytes config;
      {
        std::lock_guard lock(mutex_);
        if (!sps_.empty() && !pps_.empty()) {
          config = {0, 0, 0, 1};
          config.insert(config.end(), sps_.begin(), sps_.end());
          config.insert(config.end(), {0, 0, 0, 1});
          config.insert(config.end(), pps_.begin(), pps_.end());
        }
      }
      if (!config.empty() && !send_frame(client, config)) {
        close(client);
        continue;
      }
      while (!stop.stop_requested()) {
        Bytes frame;
        {
          std::unique_lock lock(mutex_);
          frames_ready_.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return stop.stop_requested() || !frames_.empty();
          });
          if (frames_.empty())
            continue;
          frame = std::move(frames_.front());
          frames_.pop_front();
        }
        if (!send_frame(client, frame))
          break;
      }
      close(client);
    }
  }

  void close_listener() {
    if (listener_ >= 0) {
      close(listener_);
      listener_ = -1;
    }
  }

  std::filesystem::path path_;
  int listener_{-1};
  std::jthread worker_;
  std::mutex mutex_;
  std::condition_variable frames_ready_;
  std::deque<Bytes> frames_;
  Bytes sps_;
  Bytes pps_;
};

int run_carplay_session(const char *program_path,
                        const acp::bridge::Config &config,
                        const std::function<bool()> &stop_requested,
                        const std::string &video_socket = {}) {
  if (config.head_unit_mac.empty()) {
    std::cerr << "Bridge daemon: configure a head-unit MAC first\n";
    return 2;
  }
  const auto executable =
      std::filesystem::path(program_path).parent_path() / "iap2-bt";
  std::vector<std::string> arguments{executable.string(),
                                     "--bridge",
                                     "--mac",
                                     config.head_unit_mac,
                                     "--wifi-interface",
                                     config.wifi_interface,
                                     "--pairing-store",
                                     config.airplay_pairing_store.string(),
                                     "--timeout",
                                     "60"};
  if (!video_socket.empty()) {
    arguments.push_back("--video-socket");
    arguments.push_back(video_socket);
  }
  std::vector<char *> argv;
  for (auto &argument : arguments)
    argv.push_back(argument.data());
  argv.push_back(nullptr);
  pid_t child{};
  if (posix_spawn(&child, argv.front(), nullptr, nullptr, argv.data(),
                  environ) != 0) {
    std::cerr << "Bridge daemon: unable to start CarPlay session\n";
    return 1;
  }
  std::cout << "Bridge daemon: CarPlay session started\n";
  for (;;) {
    int status{};
    if (waitpid(child, &status, WNOHANG) == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (stop_requested()) {
      std::cout << "Bridge daemon: stopping active CarPlay session\n";
      kill(child, SIGTERM);
      waitpid(child, &status, 0);
      return 128 + SIGTERM;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

int run_wired_android_auto_receiver(
    const char *program_path,
    const std::function<acp::bridge::Config()> &config_provider,
    const std::stop_token stop) {
  const auto video_socket =
      std::filesystem::path("/tmp") /
      ("acp-aa-bridge-video-" + std::to_string(getpid()) + ".sock");
  VideoSocketForwarder forwarder(video_socket);
  if (!forwarder.ready()) {
    std::cerr << "Bridge daemon: unable to listen for Android Auto video\n";
    return 1;
  }
  std::atomic_bool carplay_start_requested{};
  std::atomic_bool phone_disconnected{};
  acp::aa::WiredReceiver receiver(
      [&carplay_start_requested, &phone_disconnected](const auto &event) {
        switch (event.type) {
        case acp::aa::WiredReceiverEventType::waiting_for_phone:
          std::cout << "Bridge daemon: Android Auto USB idle: " << event.detail
                    << '\n';
          break;
        case acp::aa::WiredReceiverEventType::aoap_transport_ready:
          phone_disconnected = false;
          std::cout << "Bridge daemon: Android Auto USB transport ready: "
                    << event.detail << '\n';
          break;
        case acp::aa::WiredReceiverEventType::control_session_ready:
          std::cout << "Bridge daemon: Android Auto control session ready: "
                    << event.detail << '\n';
          break;
        case acp::aa::WiredReceiverEventType::video_stream_configured:
          std::cout << "Bridge daemon: Android Auto video configured: "
                    << event.detail << '\n';
          carplay_start_requested = true;
          break;
        case acp::aa::WiredReceiverEventType::video_stream_started:
          std::cout << "Bridge daemon: Android Auto video stream started\n";
          break;
        case acp::aa::WiredReceiverEventType::disconnected:
          std::cout << "Bridge daemon: Android Auto USB disconnected\n";
          phone_disconnected = true;
          break;
        case acp::aa::WiredReceiverEventType::error:
          std::cerr << "Bridge daemon: " << event.detail << '\n';
          break;
        }
      },
      [&forwarder](const std::span<const std::uint8_t> frame) {
        forwarder.push(frame);
      });
  std::string error;
  if (!receiver.start(&error)) {
    std::cerr << "Bridge daemon: unable to start Android Auto USB receiver: "
              << error << '\n';
    return 1;
  }
  std::cout << "Bridge daemon: wired Android Auto receiver started\n";
  while (!stop.stop_requested()) {
    if (!carplay_start_requested.exchange(false)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    std::cout << "Bridge daemon: starting CarPlay for Android Auto video\n";
    const auto config = config_provider();
    if (config.head_unit_mac.empty() || config.wifi_interface.empty()) {
      std::cerr << "Bridge daemon: configure a head-unit MAC and Wi-Fi "
                   "interface first\n";
      continue;
    }
    const auto result = run_carplay_session(
        program_path, config,
        [stop, &phone_disconnected] {
          return stop.stop_requested() || phone_disconnected.load();
        },
        video_socket);
    if (stop.stop_requested())
      break;
    if (phone_disconnected.load()) {
      std::cout << "Bridge daemon: CarPlay session stopped after Android Auto "
                   "disconnect\n";
      continue;
    }
    if (result != 0)
      std::cerr << "Bridge daemon: CarPlay video session ended with " << result
                << '\n';
    else
      std::cout << "Bridge daemon: CarPlay video session ended\n";
  }
  std::cout << "Bridge daemon: stopping wired Android Auto receiver\n";
  receiver.stop();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  std::filesystem::path config_path = acp::bridge::default_config_path();
  int port = 8080;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--config" && index + 1 < argc)
      config_path = argv[++index];
    else if (argument == "--port" && index + 1 < argc)
      port = std::stoi(argv[++index]);
    else {
      std::cerr << "usage: bridge-daemon [--config PATH] [--port PORT]\n";
      return 2;
    }
  }
  auto config = acp::bridge::load_config(config_path)
                    .value_or(acp::bridge::Config{
                        "", "", acp::bridge::default_airplay_pairing_store()});
  if (config.airplay_pairing_store.empty())
    config.airplay_pairing_store = acp::bridge::default_airplay_pairing_store();
  std::mutex config_mutex;
  refresh_bluetooth_inventory(management_state);
  refresh_wifi_inventory(management_state);
  std::jthread wifi_refresh_worker([](std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      refresh_wifi_inventory(management_state);
      for (int count = 0; count < 20 && !stop_token.stop_requested(); ++count)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  });
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) {
    std::cerr << "Unable to block shutdown signals\n";
    return 1;
  }
  const int signal_fd = signalfd(-1, &signals, SFD_CLOEXEC);
  if (signal_fd < 0) {
    std::cerr << "Unable to create shutdown signal descriptor\n";
    return 1;
  }
  std::jthread android_auto_worker([program_path = argv[0], &config,
                                    &config_mutex](const std::stop_token stop) {
    run_wired_android_auto_receiver(
        program_path,
        [&config, &config_mutex] {
          std::lock_guard lock(config_mutex);
          return config;
        },
        stop);
  });
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  int enabled = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (listener < 0 ||
      bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
          0 ||
      listen(listener, 8) != 0) {
    std::cerr << "Unable to listen on 127.0.0.1:" << port << '\n';
    close(signal_fd);
    return 1;
  }
  std::cout << "Bridge management UI listening on http://127.0.0.1:" << port
            << '\n';
  for (;;) {
    pollfd descriptors[]{{listener, POLLIN, 0}, {signal_fd, POLLIN, 0}};
    if (poll(descriptors, 2, -1) <= 0)
      continue;
    if ((descriptors[1].revents & POLLIN) != 0) {
      signalfd_siginfo signal_info{};
      if (read(signal_fd, &signal_info, sizeof(signal_info)) ==
          static_cast<ssize_t>(sizeof(signal_info))) {
        std::cout << "Bridge daemon: graceful shutdown requested\n";
        break;
      }
      continue;
    }
    if ((descriptors[0].revents & POLLIN) == 0)
      continue;
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0)
      continue;
    std::array<char, 4096> buffer{};
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos &&
           request.size() <= 16 * 1024) {
      const auto count = recv(client, buffer.data(), buffer.size(), 0);
      if (count <= 0)
        break;
      request.append(buffer.data(), static_cast<std::size_t>(count));
    }
    const auto header_end = request.find("\r\n\r\n");
    const auto content_marker = request.find("Content-Length: ");
    if (header_end != std::string::npos &&
        content_marker != std::string::npos) {
      const auto start = content_marker + 16;
      const auto end = request.find("\r\n", start);
      const auto length = end == std::string::npos
                              ? 0U
                              : static_cast<std::size_t>(std::stoul(
                                    request.substr(start, end - start)));
      while (request.size() < header_end + 4 + length) {
        const auto count = recv(client, buffer.data(), buffer.size(), 0);
        if (count <= 0)
          break;
        request.append(buffer.data(), static_cast<std::size_t>(count));
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
      std::cout << "Management: " << request_line << " -> " << status << '\n';
      return send_response(client, status, type, response_body, extra);
    };
    if (request.starts_with("GET / ") || request.starts_with("GET /?")) {
      const bool saved = request.find("saved=1") != std::string::npos;
      const auto snapshot = management_snapshot(management_state);
      const auto configuration = [&] {
        std::lock_guard lock(config_mutex);
        return config;
      }();
      respond(200, "text/html; charset=utf-8",
              page(configuration, snapshot, saved));
    } else if (request.starts_with("GET /scan-status")) {
      const auto snapshot = management_snapshot(management_state);
      const auto phase = query_field(request, "phase");
      const int status =
          snapshot.bluetooth_scan_running && phase &&
                  *phase == std::to_string(snapshot.bluetooth_scan_phase_id)
              ? 204
              : 205;
      respond(status, "text/plain", "");
    } else if (request.starts_with("POST /scan ")) {
      std::cout << "Management: Bluetooth scan requested\n";
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
      if (start_scan)
        std::thread(run_bluetooth_scan, std::ref(management_state)).detach();
      respond(303, "text/plain", "", "Location: /\r\n");
    } else if (request.starts_with("POST /display ")) {
      const bool show_unnamed = form_field(body, "show_unnamed").has_value();
      {
        std::lock_guard lock(management_state.mutex);
        management_state.snapshot.show_unnamed_bluetooth_devices = show_unnamed;
      }
      std::cout << "Management: unnamed Bluetooth devices "
                << (show_unnamed ? "shown" : "hidden") << '\n';
      respond(303, "text/plain", "", "Location: /\r\n");
    } else if (request.starts_with("POST /config ")) {
      const auto manual = form_field(body, "manual_mac");
      const auto selected = form_field(body, "head_unit_mac");
      const auto wifi = form_field(body, "wifi_interface");
      const auto previous = [&] {
        std::lock_guard lock(config_mutex);
        return config;
      }();
      const auto mac = selected && !selected->empty() ? *selected
                       : manual && !manual->empty()   ? *manual
                                                      : previous.head_unit_mac;
      if (wifi && !mac.empty() &&
          acp::bridge::save_config(
              config_path, {mac, *wifi, previous.airplay_pairing_store})) {
        {
          std::lock_guard lock(config_mutex);
          config = {mac, *wifi, previous.airplay_pairing_store};
        }
        std::cout << "Management: saved head unit " << mac << " on " << *wifi
                  << '\n';
        respond(303, "text/plain", "", "Location: /?saved=1\r\n");
      } else {
        std::cout << "Management: rejected invalid configuration\n";
        respond(400, "text/plain", "Invalid configuration\n");
      }
    } else {
      std::cout << "Management: unknown request\n";
      respond(400, "text/plain", "Unknown endpoint\n");
    }
    close(client);
  }
  close(listener);
  android_auto_worker.request_stop();
  android_auto_worker.join();
  close(signal_fd);
  std::cout << "Bridge daemon: stopped\n";
}
