#include "aa2acp/aa/wired_receiver.hpp"
#include "aa2acp/airplay/head_unit_capabilities.hpp"
#include "aa2acp/bridge/bluez_inventory.hpp"
#include "aa2acp/bridge/config.hpp"
#include "aa2acp/bridge/h264_normalizer.hpp"
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
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct ManagementSnapshot {
  std::vector<aa2acp::bridge::BluetoothDevice> bluetooth_devices;
  std::vector<std::string> wifi_interfaces;
  bool show_unnamed_bluetooth_devices{};
  bool bluetooth_scan_running{};
  int bluetooth_scan_phase_id{};
  std::string bluetooth_scan_phase{"idle"};
  std::string bluetooth_error;
  bool carplay_preflight_running{};
  std::string carplay_preflight_status;
};

struct ManagementState {
  std::mutex mutex;
  ManagementSnapshot snapshot;
};

ManagementState management_state;

class CarPlayWorker final {
public:
  CarPlayWorker() {
    int control[2];
    int output[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, control) != 0 ||
        pipe2(output, O_CLOEXEC) != 0)
      return;
    pid_ = fork();
    if (pid_ == 0) {
      setpgid(0, 0);
      close(control[0]);
      close(output[0]);
      dup2(output[1], STDOUT_FILENO);
      dup2(output[1], STDERR_FILENO);
      close(output[1]);
      std::array<char, 8192> request{};
      for (;;) {
        const auto count = recv(control[1], request.data(), request.size(), 0);
        if (count <= 0)
          _exit(0);
        std::vector<char *> argv;
        for (char *argument = request.data(); argument < request.data() + count;
             argument += std::strlen(argument) + 1)
          argv.push_back(argument);
        argv.push_back(nullptr);
        const int result = aa2acp::iap2::run_bluetooth_worker(
            static_cast<int>(argv.size() - 1), argv.data());
        send(control[1], &result, sizeof(result), MSG_NOSIGNAL);
      }
    }
    close(control[1]);
    close(output[1]);
    if (pid_ > 0) {
      control_fd_ = control[0];
      output_fd_ = output[0];
    } else {
      close(control[0]);
      close(output[0]);
    }
  }

  ~CarPlayWorker() {
    if (pid_ > 0) {
      kill(pid_, SIGTERM);
      waitpid(pid_, nullptr, 0);
    }
    if (control_fd_ >= 0)
      close(control_fd_);
    if (output_fd_ >= 0)
      close(output_fd_);
  }

  int run(std::vector<std::string> arguments, const std::stop_token stop,
          const std::atomic_bool &phone_disconnected,
          std::atomic<pid_t> &active_child) {
    std::lock_guard lock(mutex_);
    if (pid_ <= 0 || control_fd_ < 0)
      return 1;
    std::string request;
    for (const auto &argument : arguments)
      request.append(argument).push_back('\0');
    if (request.size() > 8192 ||
        send(control_fd_, request.data(), request.size(), MSG_NOSIGNAL) < 0)
      return 1;
    active_child = pid_;
    bool stopping = false;
    for (;;) {
      pollfd descriptors[]{{output_fd_, POLLIN, 0}, {control_fd_, POLLIN, 0}};
      if (poll(descriptors, 2, 100) > 0) {
        if ((descriptors[0].revents & POLLIN) != 0) {
          std::array<char, 4096> output{};
          const auto count = read(output_fd_, output.data(), output.size());
          if (count > 0) {
            std::cout.write(output.data(), count);
            std::cout.flush();
          }
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
          int result{};
          if (recv(control_fd_, &result, sizeof(result), 0) == sizeof(result)) {
            active_child = -1;
            return result;
          }
          active_child = -1;
          return 1;
        }
      }
      if (!stopping && (stop.stop_requested() || phone_disconnected.load())) {
        std::cout << "Bridge daemon: stopping active CarPlay session\n";
        kill(pid_, SIGTERM);
        stopping = true;
      }
    }
  }

private:
  pid_t pid_{-1};
  int control_fd_{-1};
  int output_fd_{-1};
  std::mutex mutex_;
};

std::unique_ptr<CarPlayWorker> carplay_worker;

std::string log_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count() %
      1000;
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
  localtime_r(&time, &local_time);
  std::array<char, 40> text{};
  std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M:%S", &local_time);
  std::snprintf(text.data() + 19, text.size() - 19, ".%03lld ",
                static_cast<long long>(milliseconds));
  return text.data();
}

class TeeBuffer final : public std::streambuf {
public:
  TeeBuffer(std::streambuf *console, std::ofstream &file, std::mutex &mutex)
      : console_(console), file_(file), mutex_(mutex) {}

private:
  int_type overflow(const int_type character) override {
    if (traits_type::eq_int_type(character, traits_type::eof()))
      return traits_type::not_eof(character);
    std::lock_guard lock(mutex_);
    const auto text = traits_type::to_char_type(character);
    return write_locked(std::string_view(&text, 1)) ? character
                                                    : traits_type::eof();
  }

  std::streamsize xsputn(const char *text,
                         const std::streamsize size) override {
    std::lock_guard lock(mutex_);
    return write_locked(std::string_view(text, size)) ? size : 0;
  }

  int sync() override {
    std::lock_guard lock(mutex_);
    const auto console_result = console_->pubsync();
    file_.flush();
    return console_result == 0 && file_ ? 0 : -1;
  }

  bool write_locked(const std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
      if (at_line_start_) {
        const auto timestamp = log_timestamp();
        if (console_->sputn(timestamp.data(), timestamp.size()) !=
                static_cast<std::streamsize>(timestamp.size()) ||
            !file_.write(timestamp.data(), timestamp.size()))
          return false;
        at_line_start_ = false;
      }
      const auto line_end = text.find('\n', offset);
      const auto count = line_end == std::string_view::npos
                             ? text.size() - offset
                             : line_end - offset + 1;
      if (console_->sputn(text.data() + offset, count) !=
              static_cast<std::streamsize>(count) ||
          !file_.write(text.data() + offset, count))
        return false;
      at_line_start_ = text[offset + count - 1] == '\n';
      offset += count;
    }
    return true;
  }

  std::streambuf *console_;
  std::ofstream &file_;
  std::mutex &mutex_;
  bool at_line_start_{true};
};

class DaemonLog final {
public:
  explicit DaemonLog(const std::filesystem::path &path)
      : path_(path), file_(path, std::ios::app),
        cout_buffer_(std::cout.rdbuf(), file_, mutex_),
        cerr_buffer_(std::cerr.rdbuf(), file_, mutex_) {
    if (file_) {
      old_cout_ = std::cout.rdbuf(&cout_buffer_);
      old_cerr_ = std::cerr.rdbuf(&cerr_buffer_);
    }
  }

  ~DaemonLog() {
    if (old_cout_ != nullptr)
      std::cout.rdbuf(old_cout_);
    if (old_cerr_ != nullptr)
      std::cerr.rdbuf(old_cerr_);
  }

  bool active() const { return old_cout_ != nullptr; }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
  std::ofstream file_;
  std::mutex mutex_;
  TeeBuffer cout_buffer_;
  TeeBuffer cerr_buffer_;
  std::streambuf *old_cout_{};
  std::streambuf *old_cerr_{};
};

std::unique_ptr<DaemonLog> start_daemon_log() {
  const auto directory = aa2acp::bridge::default_state_directory() / "logs";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error)
    return {};
  std::vector<std::filesystem::directory_entry> logs;
  for (const auto &entry :
       std::filesystem::directory_iterator(directory, error)) {
    if (entry.is_regular_file() && entry.path().extension() == ".log" &&
        entry.path().filename().string().starts_with("aa2acp-bridge-daemon-"))
      logs.push_back(entry);
  }
  std::sort(logs.begin(), logs.end(), [](const auto &left, const auto &right) {
    return left.last_write_time() > right.last_write_time();
  });
  for (std::size_t index = 29; index < logs.size(); ++index)
    std::filesystem::remove(logs[index], error);
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  const auto path =
      directory / ("aa2acp-bridge-daemon-" + std::to_string(stamp) + "-" +
                   std::to_string(getpid()) + ".log");
  auto log = std::make_unique<DaemonLog>(path);
  return log->active() ? std::move(log) : nullptr;
}

std::string normalized_bluetooth_address(const std::string &value) {
  std::string normalized;
  for (const unsigned char character : value) {
    if (std::isxdigit(character))
      normalized += static_cast<char>(std::toupper(character));
  }
  return normalized;
}

bool is_unnamed(const aa2acp::bridge::BluetoothDevice &device) {
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
  aa2acp::bridge::discover_bluez_devices("le", 30, log);
  refresh_bluetooth_inventory(state);
  set_phase(3, "classic discovery in progress");
  aa2acp::bridge::discover_bluez_devices("bredr", 15, log);
  refresh_bluetooth_inventory(state);
  {
    std::lock_guard lock(state.mutex);
    state.snapshot.bluetooth_scan_running = false;
    state.snapshot.bluetooth_scan_phase_id = 4;
    state.snapshot.bluetooth_scan_phase = "last discovery completed";
  }
  std::cout << "Management Bluetooth: discovery finished\n";
}

std::string page(const aa2acp::bridge::Config &config,
                 const ManagementSnapshot &snapshot, const bool saved,
                 const bool carplay_error) {
  std::string output =
      "<!doctype html><html><head><meta name=\"viewport\" "
      "content=\"width=device-width,initial-scale=1\">"
      "<title>AA2ACP</title><style>body{font:16px "
      "sans-serif;max-width:38rem;margin:3rem auto;padding:0 1rem}"
      "label,select,input:not([type=checkbox]){display:block;width:100%;box-"
      "sizing:border-box;"
      "margin:.5rem 0}button{padding:.6rem 1rem;margin:.25rem 0}"
      "input[type=checkbox]{width:auto;margin:0 .4rem 0 0}"
      ".hint{color:#555}.status{padding:.6rem;background:#eef7ee}"
      ".status.error{background:#fdecec;color:#8a1f1f}</style></"
      "head><body data-scan-running=\"" +
      std::string(snapshot.bluetooth_scan_running ? "1" : "0") +
      "\" data-scan-phase=\"" +
      std::to_string(snapshot.bluetooth_scan_phase_id) +
      "\" data-preflight-running=\"" +
      std::string(snapshot.carplay_preflight_running ? "1" : "0") +
      "\">"
      "<h1>AA2ACP</h1><p>Configure the pinned CarPlay head unit.</p>";
  if (saved || carplay_error) {
    if (saved)
      output += "<p class=status>Configuration saved.</p>";
    if (carplay_error)
      output += "<p class=\"status error\">CarPlay preparation requires "
                "both a Bluetooth device and Wi-Fi interface "
                "configuration.</p>";
    output += "<script>history.replaceState(null,'',location.pathname);"
              "</script>";
  }
  if (config.head_unit_mac.empty() || config.wifi_interface.empty())
    output +=
        "<p class=status>Android Auto is disabled until both settings are "
        "saved.</p>";
  if (!snapshot.carplay_preflight_status.empty())
    output += "<p class=status>CarPlay preparation: " +
              html_escape(snapshot.carplay_preflight_status) + "</p>";
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
      "\" placeholder=\"Bluetooth address\"></label>"
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
      "</select></label><button type=submit>Save configuration</button></form>";
  if (!config.head_unit_mac.empty() && !config.wifi_interface.empty())
    output +=
        "<form method=post action=\"/carplay-prepare\"><button type=submit " +
        std::string(snapshot.carplay_preflight_running ? "disabled" : "") +
        ">Prepare/Test CarPlay</button></form>";
  output +=
      "<script>(()=>{const filter=document.querySelector('#show-unnamed');"
      "if(filter)filter.addEventListener('change',()=>filter.form."
      "requestSubmit());"
      "if(document.body.dataset.scanRunning!=='1')return;"
      "const phase=encodeURIComponent(document.body.dataset.scanPhase);"
      "const poll=async()=>{try{const response=await "
      "fetch('/scan-status?phase='+phase);"
      "if(response.status===205){location.reload();return;}setTimeout(poll,"
      "1000);"
      "}catch{setTimeout(poll,2000);}};poll();})();</script>"
      "<script>(()=>{if(document.body.dataset.preflightRunning!=='1')return;"
      "const poll=async()=>{try{const response=await "
      "fetch('/carplay-prepare-status');"
      "if(response.status===205){location.reload();return;}setTimeout(poll,"
      "1000);}catch{setTimeout(poll,2000);}};poll();})();</script></"
      "body></html>";
  return output;
}

class VideoSocketForwarder {
public:
  explicit VideoSocketForwarder(const std::filesystem::path &path)
      : path_(path) {
    if (const char *dump_path = std::getenv("AA2ACP_DUMP_H264");
        dump_path != nullptr && *dump_path != '\0') {
      dump_.open(dump_path, std::ios::binary | std::ios::trunc);
      if (dump_)
        std::cout << "Bridge daemon: capturing Android Auto H.264 to "
                  << dump_path << '\n';
      else
        std::cerr << "Bridge daemon: unable to capture Android Auto H.264 to "
                  << dump_path << '\n';
    }
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
      bool keyframe = false;
      const auto frame_nalus = nalus(frame);
      std::string nalu_summary;
      for (const auto &nalu : frame_nalus) {
        if (nalu.empty())
          continue;
        const auto type = nalu[0] & 0x1f;
        if (!nalu_summary.empty())
          nalu_summary += ',';
        nalu_summary +=
            std::to_string(type) + ":" + std::to_string(nalu.size());
        if (type == 7)
          sps_ = nalu;
        else if (type == 8)
          pps_ = nalu;
        else if (type == 5)
          keyframe = true;
      }
      ++received_video_count_;
      if (dump_) {
        dump_.write(reinterpret_cast<const char *>(frame.data()),
                    static_cast<std::streamsize>(frame.size()));
      }
      if (received_video_count_ <= 5 || received_video_count_ % 60 == 0) {
        std::cout << "Bridge daemon: Android Auto H.264 access unit #"
                  << received_video_count_ << " (" << frame.size()
                  << " bytes; NAL type:size=" << nalu_summary << ")\n";
      }
      if (keyframe) {
        // CarPlay setup can take longer than the ordinary frame queue. Keep
        // the latest decoder entry point and every dependent frame after it.
        keyframe_ = std::move(frame);
        frames_.clear();
        frames_.push_back(keyframe_);
        std::cout << "Bridge daemon: retained Android Auto H.264 keyframe "
                     "and dependent frame sequence\n";
      } else if (frames_.size() < 600) {
        frames_.push_back(std::move(frame));
      }
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
      if (end + 3 > input.size())
        end = input.size();
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
      bool has_keyframe = false;
      {
        std::lock_guard lock(mutex_);
        if (!sps_.empty() && !pps_.empty()) {
          config = {0, 0, 0, 1};
          config.insert(config.end(), sps_.begin(), sps_.end());
          config.insert(config.end(), {0, 0, 0, 1});
          config.insert(config.end(), pps_.begin(), pps_.end());
        }
        has_keyframe = !keyframe_.empty();
      }
      if (!config.empty() && !send_frame(client, config)) {
        close(client);
        continue;
      }
      std::cout << "Bridge daemon: forwarded Android Auto H.264 "
                << (config.empty() ? "without cached config" : "config")
                << (has_keyframe ? " and cached keyframe sequence\n"
                                 : " and awaiting keyframe\n");
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
  Bytes keyframe_;
  std::size_t received_video_count_{};
  std::ofstream dump_;
};

// Moves PCM off AASDK's I/O thread without allowing a slow CarPlay consumer to
// block Android Auto acknowledgements. Frames use the same four-byte big-endian
// length prefix as the video socket.
class AudioSocketForwarder {
public:
  AudioSocketForwarder(const std::filesystem::path &path, std::string name)
      : path_(path), name_(std::move(name)) {
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
    worker_ =
        std::jthread([this](const std::stop_token stop) { forward(stop); });
  }

  ~AudioSocketForwarder() {
    worker_.request_stop();
    close_listener();
    frames_ready_.notify_all();
    if (!path_.empty())
      unlink(path_.c_str());
  }

  bool ready() const { return listener_ >= 0; }

  void push(const std::span<const std::uint8_t> pcm) {
    if (pcm.empty())
      return;
    std::lock_guard lock(mutex_);
    ++received_packets_;
    if (received_packets_ <= 3 || received_packets_ % 500 == 0) {
      std::cout << "Bridge daemon: Android Auto " << name_ << " audio packet #"
                << received_packets_ << " (" << pcm.size() << " bytes)\n";
    }
    if (frames_.size() >= 100)
      frames_.pop_front();
    frames_.emplace_back(pcm.begin(), pcm.end());
    frames_ready_.notify_one();
  }

private:
  using Bytes = std::vector<std::uint8_t>;

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
      std::cout << "Bridge daemon: connected to Android Auto " << name_
                << " audio source\n";
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
  std::string name_;
  int listener_{-1};
  std::jthread worker_;
  std::mutex mutex_;
  std::condition_variable frames_ready_;
  std::deque<Bytes> frames_;
  std::size_t received_packets_{};
};

bool stop_carplay_process_group(const pid_t child, int *status) {
  kill(-child, SIGTERM);
  const auto graceful_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (std::chrono::steady_clock::now() < graceful_deadline) {
    if (waitpid(child, status, WNOHANG) == child)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::cerr << "Bridge daemon: CarPlay worker did not stop after SIGTERM; "
               "sending SIGKILL\n";
  kill(-child, SIGKILL);
  while (waitpid(child, status, 0) < 0 && errno == EINTR) {
  }
  return true;
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
    std::cerr << "Bridge daemon: configure a head-unit MAC first\n";
    return 2;
  }
  std::vector<std::string> arguments{
      "aa2acp-iap2-bt",
      "--bridge",
      "--mac",
      config.head_unit_mac,
      "--wifi-interface",
      config.wifi_interface,
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
    std::cerr << "Bridge daemon: CarPlay worker is unavailable\n";
    return 1;
  }
  return carplay_worker->run(std::move(arguments), stop, phone_disconnected,
                             active_child);

  std::vector<char *> argv;
  for (auto &argument : arguments)
    argv.push_back(argument.data());
  argv.push_back(nullptr);
  int output_pipe[2];
  if (pipe2(output_pipe, O_CLOEXEC) != 0) {
    std::cerr << "Bridge daemon: unable to capture CarPlay session output\n";
    return 1;
  }
  const pid_t child = fork();
  if (child == 0) {
    setpgid(0, 0);
    dup2(output_pipe[1], STDOUT_FILENO);
    dup2(output_pipe[1], STDERR_FILENO);
    close(output_pipe[0]);
    close(output_pipe[1]);
    _exit(aa2acp::iap2::run_bluetooth_worker(static_cast<int>(argv.size() - 1),
                                             argv.data()));
  }
  close(output_pipe[1]);
  if (child < 0) {
    close(output_pipe[0]);
    std::cerr << "Bridge daemon: unable to start CarPlay session\n";
    return 1;
  }
  auto forward_output = [&](const int timeout) {
    pollfd descriptor{output_pipe[0], POLLIN, 0};
    while (output_pipe[0] >= 0 && poll(&descriptor, 1, timeout) > 0) {
      std::array<char, 4096> output{};
      const auto count = read(output_pipe[0], output.data(), output.size());
      if (count > 0) {
        std::cout.write(output.data(), count);
        std::cout.flush();
        descriptor.revents = 0;
        continue;
      }
      close(output_pipe[0]);
      output_pipe[0] = -1;
    }
  };
  active_child = child;
  std::cout << "Bridge daemon: CarPlay session started\n";
  for (;;) {
    forward_output(0);
    int status{};
    if (waitpid(child, &status, WNOHANG) == child) {
      while (output_pipe[0] >= 0)
        forward_output(100);
      active_child = -1;
      if (!WIFEXITED(status)) {
        std::cout << "Bridge daemon: cleaning up CarPlay Wi-Fi after worker "
                     "failure\n";
        if (!aa2acp::iap2::leave_with_networkmanager(config.wifi_interface))
          std::cerr << "Bridge daemon: unable to disconnect CarPlay Wi-Fi\n";
      }
      return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    if (stop.stop_requested() || phone_disconnected.load()) {
      std::cout << "Bridge daemon: stopping active CarPlay session\n";
      const auto forced = stop_carplay_process_group(child, &status);
      while (output_pipe[0] >= 0)
        forward_output(100);
      if (forced || !WIFEXITED(status)) {
        std::cout << "Bridge daemon: cleaning up CarPlay Wi-Fi after "
                  << (forced ? "forced worker termination"
                             : "abnormal worker termination")
                  << '\n';
        if (!aa2acp::iap2::leave_with_networkmanager(config.wifi_interface))
          std::cerr << "Bridge daemon: unable to disconnect CarPlay Wi-Fi\n";
      }
      active_child = -1;
      return 128 + SIGTERM;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
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
  std::cout << "Management: starting CarPlay preflight for "
            << config.head_unit_mac << '\n';
  const auto result = run_carplay_session(config, stop, phone_disconnected,
                                          active_child, {}, {}, {}, {}, true);
  {
    std::lock_guard lock(management.mutex);
    management.snapshot.carplay_preflight_running = false;
    management.snapshot.carplay_preflight_status =
        result == 0 ? "ready; capabilities cached"
                    : "failed (see daemon log for details)";
  }
  std::cout << "Management: CarPlay preflight "
            << (result == 0 ? "succeeded" : "failed") << '\n';
}

int run_wired_android_auto_receiver(
    const std::function<aa2acp::bridge::Config()> &config_provider,
    const std::stop_token stop) {
  bool configuration_warning_logged = false;
  while (!stop.stop_requested()) {
    const auto config = config_provider();
    if (!config.head_unit_mac.empty() && !config.wifi_interface.empty())
      break;
    if (!configuration_warning_logged) {
      std::cout << "Bridge daemon: wired Android Auto is disabled until a "
                   "head-unit MAC and Wi-Fi interface are configured\n";
      configuration_warning_logged = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (stop.stop_requested())
    return 0;

  const auto video_socket =
      std::filesystem::path("/tmp") /
      ("aa2acp-video-" + std::to_string(getpid()) + ".sock");
  const auto media_audio_socket =
      std::filesystem::path("/tmp") /
      ("aa2acp-media-audio-" + std::to_string(getpid()) + ".sock");
  const auto guidance_audio_socket =
      std::filesystem::path("/tmp") /
      ("aa2acp-guidance-audio-" + std::to_string(getpid()) + ".sock");
  const auto system_audio_socket =
      std::filesystem::path("/tmp") /
      ("aa2acp-system-audio-" + std::to_string(getpid()) + ".sock");
  VideoSocketForwarder forwarder(video_socket);
  if (!forwarder.ready()) {
    std::cerr << "Bridge daemon: unable to listen for Android Auto video\n";
    return 1;
  }
  AudioSocketForwarder media_audio_forwarder(media_audio_socket, "media");
  AudioSocketForwarder guidance_audio_forwarder(guidance_audio_socket,
                                                "guidance");
  AudioSocketForwarder system_audio_forwarder(system_audio_socket, "system");
  if (!media_audio_forwarder.ready() || !guidance_audio_forwarder.ready() ||
      !system_audio_forwarder.ready()) {
    std::cerr << "Bridge daemon: unable to listen for Android Auto audio\n";
    return 1;
  }
  std::atomic_bool carplay_start_requested{};
  std::atomic_bool phone_disconnected{};
  std::atomic_bool carplay_preparation_failed{};
  std::atomic<pid_t> active_carplay_child{-1};
  aa2acp::bridge::H264Normalizer h264_normalizer;
  aa2acp::aa::WiredReceiver receiver(
      [&carplay_start_requested, &phone_disconnected,
       &carplay_preparation_failed, &active_carplay_child](const auto &event) {
        switch (event.type) {
        case aa2acp::aa::WiredReceiverEventType::waiting_for_phone:
          std::cout << "Bridge daemon: Android Auto USB idle: " << event.detail
                    << '\n';
          break;
        case aa2acp::aa::WiredReceiverEventType::aoap_transport_ready:
          phone_disconnected = false;
          carplay_preparation_failed = false;
          std::cout << "Bridge daemon: Android Auto USB transport ready: "
                    << event.detail << '\n';
          carplay_start_requested = true;
          break;
        case aa2acp::aa::WiredReceiverEventType::control_session_ready:
          std::cout << "Bridge daemon: Android Auto control session ready: "
                    << event.detail << '\n';
          break;
        case aa2acp::aa::WiredReceiverEventType::video_stream_configured:
          std::cout << "Bridge daemon: Android Auto video configured: "
                    << event.detail << '\n';
          break;
        case aa2acp::aa::WiredReceiverEventType::video_stream_started:
          std::cout << "Bridge daemon: Android Auto video stream started\n";
          break;
        case aa2acp::aa::WiredReceiverEventType::disconnected:
          std::cout << "Bridge daemon: Android Auto USB disconnected\n";
          phone_disconnected = true;
          if (const auto child = active_carplay_child.load(); child > 0) {
            std::cout << "Bridge daemon: stopping CarPlay after Android Auto "
                         "disconnect\n";
            kill(-child, SIGTERM);
          }
          break;
        case aa2acp::aa::WiredReceiverEventType::error:
          std::cerr << "Bridge daemon: " << event.detail << '\n';
          break;
        }
      },
      [&forwarder,
       &h264_normalizer](const std::span<const std::uint8_t> frame) {
        std::string error;
        const auto normalized = h264_normalizer.normalize(frame, &error);
        if (normalized.empty()) {
          std::cerr << "Bridge daemon: " << error << '\n';
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
      [&config_provider, &carplay_preparation_failed]()
          -> std::optional<aa2acp::aa::HeadUnitCapabilities> {
        const auto config = config_provider();
        // A cold preflight normally populates this cache. Keep the fallback
        // bounded so a failed CarPlay attempt cannot consume Android Auto's
        // entire connection window.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
          if (carplay_preparation_failed.load())
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
        std::cerr << "Bridge daemon: CarPlay capabilities unavailable after "
                     "5 seconds; using 1280x720 fallback\n";
        return std::nullopt;
      });
  std::string error;
  if (!receiver.start(&error)) {
    std::cerr << "Bridge daemon: unable to start Android Auto USB receiver: "
              << error << '\n';
    return 1;
  }
  std::cout << "Bridge daemon: wired Android Auto receiver started\n";
  auto retry_delay = std::chrono::seconds(0);
  while (!stop.stop_requested()) {
    if (!carplay_start_requested.exchange(false)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    std::cout << "Bridge daemon: preparing CarPlay for Android Auto\n";
    const auto config = config_provider();
    if (config.head_unit_mac.empty() || config.wifi_interface.empty()) {
      std::cerr << "Bridge daemon: configure a head-unit MAC and Wi-Fi "
                   "interface first\n";
      continue;
    }
    const auto result = run_carplay_session(
        config, stop, phone_disconnected, active_carplay_child, video_socket,
        media_audio_socket, guidance_audio_socket, system_audio_socket);
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
    if (result == 0) {
      retry_delay = std::chrono::seconds(0);
    } else {
      carplay_preparation_failed = true;
      if (retry_delay.count() == 0)
        retry_delay = std::chrono::seconds(1);
      else
        retry_delay = std::min(retry_delay * 2, std::chrono::seconds(30));
      std::cout << "Bridge daemon: retrying CarPlay in " << retry_delay.count()
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
  std::cout << "Bridge daemon: stopping wired Android Auto receiver\n";
  receiver.stop();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  auto daemon_log = start_daemon_log();
  if (daemon_log)
    std::cout << "Bridge daemon: logging to " << daemon_log->path() << '\n';
  std::filesystem::path config_path = aa2acp::bridge::default_config_path();
  int port = 8080;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--config" && index + 1 < argc)
      config_path = argv[++index];
    else if (argument == "--port" && index + 1 < argc)
      port = std::stoi(argv[++index]);
    else {
      std::cerr
          << "usage: aa2acp-bridge-daemon [--config PATH] [--port PORT]\n";
      return 2;
    }
  }
  auto config =
      aa2acp::bridge::load_config(config_path)
          .value_or(aa2acp::bridge::Config{
              "", "", aa2acp::bridge::default_airplay_pairing_store()});
  if (config.airplay_pairing_store.empty())
    config.airplay_pairing_store =
        aa2acp::bridge::default_airplay_pairing_store();
  std::mutex config_mutex;
  carplay_worker = std::make_unique<CarPlayWorker>();
  if (!carplay_worker) {
    std::cerr << "Unable to start CarPlay worker\n";
    return 1;
  }
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
  std::jthread android_auto_worker(
      [&config, &config_mutex](const std::stop_token stop) {
        run_wired_android_auto_receiver(
            [&config, &config_mutex] {
              std::lock_guard lock(config_mutex);
              return config;
            },
            stop);
      });
  std::jthread carplay_preflight_worker;
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
    std::cerr << "Unable to listen on 0.0.0.0:" << port << '\n';
    close(signal_fd);
    return 1;
  }
  std::cout << "Bridge management UI listening on http://0.0.0.0:" << port
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
      const bool carplay_error =
          request.find("carplay_error=1") != std::string::npos;
      const auto snapshot = management_snapshot(management_state);
      const auto configuration = [&] {
        std::lock_guard lock(config_mutex);
        return config;
      }();
      respond(200, "text/html; charset=utf-8",
              page(configuration, snapshot, saved, carplay_error));
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
      respond(snapshot.carplay_preflight_running ? 204 : 205, "text/plain",
              snapshot.carplay_preflight_status);
    } else if (request.starts_with("POST /carplay-prepare ")) {
      const auto selected_config = [&] {
        std::lock_guard lock(config_mutex);
        return config;
      }();
      bool start_preflight = false;
      {
        std::lock_guard lock(management_state.mutex);
        if (!management_state.snapshot.carplay_preflight_running) {
          management_state.snapshot.carplay_preflight_running = true;
          management_state.snapshot.carplay_preflight_status = "queued";
          start_preflight = true;
        }
      }
      if (start_preflight && !selected_config.head_unit_mac.empty() &&
          !selected_config.wifi_interface.empty()) {
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
              "configure a head-unit MAC and Wi-Fi interface first";
        }
        const auto location = selected_config.head_unit_mac.empty() ||
                                      selected_config.wifi_interface.empty()
                                  ? "Location: /?carplay_error=1\r\n"
                                  : "Location: /\r\n";
        respond(303, "text/plain", "", location);
      }
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
          aa2acp::bridge::save_config(
              config_path, {mac, *wifi, previous.airplay_pairing_store})) {
        if (mac != previous.head_unit_mac) {
          std::error_code error;
          const auto capabilities_store =
              aa2acp::bridge::default_head_unit_capabilities_store();
          if (std::filesystem::remove(capabilities_store, error)) {
            std::cout
                << "Management: invalidated cached head-unit capabilities "
                   "after head-unit change\n";
          } else if (error) {
            std::cerr << "Management: unable to invalidate cached head-unit "
                         "capabilities: "
                      << error.message() << '\n';
          }
        }
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
  carplay_preflight_worker.request_stop();
  carplay_preflight_worker.join();
  android_auto_worker.request_stop();
  android_auto_worker.join();
  close(signal_fd);
  std::cout << "Bridge daemon: stopped\n";
}
