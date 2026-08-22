#include "aa2acp/aa/wired_receiver.hpp"
#include "aa2acp/airplay/head_unit_capabilities.hpp"
#include "aa2acp/bridge/bluez_inventory.hpp"
#include "aa2acp/bridge/config.hpp"
#include "aa2acp/bridge/h264_normalizer.hpp"
#include "aa2acp/bridge/logging.hpp"
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
  std::optional<aa2acp::iap2::PairingConfirmationMessage> pairing_confirmation;
  bool management_hotspot_password_pending{};
  std::string pending_management_hotspot_ssid;
};

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
      aa2acp::iap2::set_pairing_confirmation_control_fd(control[1]);
      std::array<char, 8192> request{};
      for (;;) {
        const auto count = recv(control[1], request.data(), request.size(), 0);
        if (count <= 0)
          _exit(0);
        if (request[0] == '\2') {
          aa2acp::iap2::request_bluetooth_worker_stop();
          continue;
        }
        if (request[0] == '\4' &&
            count ==
                static_cast<ssize_t>(
                    1 + sizeof(aa2acp::iap2::PairingConfirmationMessage) + 1)) {
          aa2acp::iap2::PairingConfirmationMessage confirmation{};
          std::memcpy(&confirmation, request.data() + 1, sizeof(confirmation));
          aa2acp::iap2::answer_pairing_confirmation(
              confirmation.id, request[1 + sizeof(confirmation)] != 0);
          continue;
        }
        if (request[0] != '\1')
          continue;
        aa2acp::iap2::reset_bluetooth_worker_stop();
        std::vector<char *> argv;
        for (char *argument = request.data() + 1;
             argument < request.data() + count;
             argument += std::strlen(argument) + 1)
          argv.push_back(argument);
        argv.push_back(nullptr);
        std::vector<std::string> arguments;
        for (const char *argument : argv)
          if (argument)
            arguments.emplace_back(argument);
        std::thread([control_fd = control[1],
                     arguments = std::move(arguments)] {
          std::vector<char *> worker_argv;
          for (const auto &argument : arguments)
            worker_argv.push_back(const_cast<char *>(argument.data()));
          worker_argv.push_back(nullptr);
          const int result = aa2acp::iap2::run_bluetooth_worker(
              static_cast<int>(worker_argv.size() - 1), worker_argv.data());
          std::array<std::byte, 1 + sizeof(result)> response{};
          response[0] = std::byte{2};
          std::memcpy(response.data() + 1, &result, sizeof(result));
          send(control_fd, response.data(), response.size(), MSG_NOSIGNAL);
        }).detach();
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
    request.push_back('\1');
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
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
            (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          std::lock_guard state_lock(management_state.mutex);
          management_state.snapshot.pairing_confirmation.reset();
          active_child = -1;
          return 1;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
          std::array<char, 4096> output{};
          const auto count = read(output_fd_, output.data(), output.size());
          if (count > 0) {
            // Child output is already level-prefixed; preserve it verbatim.
            std::cout.write(output.data(), count);
            std::cout.flush();
          }
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
          std::array<std::byte,
                     1 + sizeof(aa2acp::iap2::PairingConfirmationMessage) + 1>
              message{};
          const auto count =
              recv(control_fd_, message.data(), message.size(), 0);
          if (count > 0 && message[0] == std::byte{3} &&
              count ==
                  static_cast<ssize_t>(
                      1 + sizeof(aa2acp::iap2::PairingConfirmationMessage))) {
            aa2acp::iap2::PairingConfirmationMessage confirmation{};
            std::memcpy(&confirmation, message.data() + 1,
                        sizeof(confirmation));
            std::lock_guard state_lock(management_state.mutex);
            management_state.snapshot.pairing_confirmation = confirmation;
            management_state.snapshot.carplay_preflight_status =
                "compare the Bluetooth pairing code in the management UI";
            continue;
          }
          if (count == static_cast<ssize_t>(1 + sizeof(int)) &&
              message[0] == std::byte{2}) {
            int result{};
            std::memcpy(&result, message.data() + 1, sizeof(result));
            {
              std::lock_guard state_lock(management_state.mutex);
              management_state.snapshot.pairing_confirmation.reset();
            }
            active_child = -1;
            return result;
          }
          active_child = -1;
          return 1;
        }
      }
      if (!stopping && (stop.stop_requested() || phone_disconnected.load())) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
            << "Bridge daemon: stopping active CarPlay session\n";
        const char stop_command = '\2';
        send(control_fd_, &stop_command, sizeof(stop_command), MSG_NOSIGNAL);
        stopping = true;
      }
    }
  }

  bool answer_pairing_confirmation(
      const aa2acp::iap2::PairingConfirmationMessage &confirmation,
      const bool confirmed) {
    std::array<std::byte, 1 + sizeof(confirmation) + 1> message{};
    message[0] = std::byte{4};
    std::memcpy(message.data() + 1, &confirmation, sizeof(confirmation));
    message[1 + sizeof(confirmation)] = confirmed ? std::byte{1} : std::byte{0};
    std::lock_guard lock(command_mutex_);
    return control_fd_ >= 0 &&
           send(control_fd_, message.data(), message.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(message.size());
  }

private:
  pid_t pid_{-1};
  int control_fd_{-1};
  int output_fd_{-1};
  std::mutex mutex_;
  std::mutex command_mutex_;
};

std::unique_ptr<CarPlayWorker> carplay_worker;

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

class RecentLog final {
public:
  static constexpr std::size_t kMaximumBytes = 200 * 1024;

  void append(const std::string_view text) {
    std::lock_guard lock(mutex_);
    contents_ += text;
    if (contents_.size() > kMaximumBytes)
      contents_.erase(0, contents_.size() - kMaximumBytes);
  }

  std::string snapshot() const {
    std::lock_guard lock(mutex_);
    return contents_;
  }

private:
  mutable std::mutex mutex_;
  std::string contents_;
};

class TeeBuffer final : public std::streambuf {
public:
  TeeBuffer(std::streambuf *console, std::ofstream *file, RecentLog &recent,
            std::mutex &mutex, const aa2acp::bridge::LogLevel default_level)
      : console_(console), file_(file), recent_(recent), mutex_(mutex),
        default_level_(default_level) {}

private:
  static bool has_log_level_prefix(const std::string_view text) {
    for (const auto level :
         {aa2acp::bridge::LogLevel::debug, aa2acp::bridge::LogLevel::info,
          aa2acp::bridge::LogLevel::warning, aa2acp::bridge::LogLevel::error}) {
      const auto name = aa2acp::bridge::log_level_name(level);
      if (text.starts_with("[" + std::string(name)))
        return true;
    }
    return false;
  }

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
    if (file_ != nullptr)
      file_->flush();
    return console_result == 0 && (file_ == nullptr || *file_) ? 0 : -1;
  }

  bool write_locked(const std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
      if (at_line_start_) {
        const bool explicit_level = has_log_level_prefix(text.substr(offset));
        const auto level =
            aa2acp::bridge::current_log_level().value_or(default_level_);
        const char marker = offset == 0 ? '>' : '|';
        const std::string prefix =
            explicit_level ? "" : aa2acp::bridge::log_prefix(level, marker);
        if (!prefix.empty() && console_->sputn(prefix.data(), prefix.size()) !=
                                   static_cast<std::streamsize>(prefix.size()))
          return false;
        const auto retained_prefix = log_timestamp() + prefix;
        if (file_ != nullptr &&
            !file_->write(retained_prefix.data(), retained_prefix.size()))
          return false;
        recent_.append(retained_prefix);
        at_line_start_ = false;
      }
      const auto line_end = text.find('\n', offset);
      const auto count = line_end == std::string_view::npos
                             ? text.size() - offset
                             : line_end - offset + 1;
      if (console_->sputn(text.data() + offset, count) !=
          static_cast<std::streamsize>(count))
        return false;
      if (file_ != nullptr && !file_->write(text.data() + offset, count))
        return false;
      recent_.append(text.substr(offset, count));
      at_line_start_ = text[offset + count - 1] == '\n';
      offset += count;
    }
    return true;
  }

  std::streambuf *console_;
  std::ofstream *file_;
  RecentLog &recent_;
  std::mutex &mutex_;
  aa2acp::bridge::LogLevel default_level_;
  bool at_line_start_{true};
};

class DaemonLog final {
public:
  DaemonLog(RecentLog &recent, const std::optional<std::filesystem::path> &path)
      : path_(path),
        file_(path_ ? *path_ : std::filesystem::path{}, std::ios::app),
        cout_buffer_(std::cout.rdbuf(), file_ ? &file_ : nullptr, recent,
                     mutex_, aa2acp::bridge::LogLevel::info),
        cerr_buffer_(std::cerr.rdbuf(), file_ ? &file_ : nullptr, recent,
                     mutex_, aa2acp::bridge::LogLevel::error) {
    old_cout_ = std::cout.rdbuf(&cout_buffer_);
    old_cerr_ = std::cerr.rdbuf(&cerr_buffer_);
  }

  ~DaemonLog() {
    if (old_cout_ != nullptr)
      std::cout.rdbuf(old_cout_);
    if (old_cerr_ != nullptr)
      std::cerr.rdbuf(old_cerr_);
  }

  const std::optional<std::filesystem::path> &path() const { return path_; }

private:
  std::optional<std::filesystem::path> path_;
  std::ofstream file_;
  std::mutex mutex_;
  TeeBuffer cout_buffer_;
  TeeBuffer cerr_buffer_;
  std::streambuf *old_cout_{};
  std::streambuf *old_cerr_{};
};

std::optional<std::filesystem::path> next_daemon_log_path() {
  const auto directory = aa2acp::bridge::default_state_directory() / "logs";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error)
    return std::nullopt;
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
  return path;
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

std::string random_token() {
  constexpr char digits[] = "0123456789abcdef";
  std::random_device random;
  std::string token;
  token.reserve(48);
  for (int index = 0; index < 48; ++index)
    token.push_back(digits[random() & 0x0f]);
  return token;
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
  std::size_t offset{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (offset < response.size()) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now())
            .count();
    if (remaining <= 0)
      return false;
    pollfd descriptor{client, POLLOUT, 0};
    if (poll(&descriptor, 1, static_cast<int>(remaining)) <= 0)
      return false;
    const auto written =
        send(client, response.data() + offset, response.size() - offset,
             MSG_NOSIGNAL | MSG_DONTWAIT);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
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

std::string page(const aa2acp::bridge::Config &config,
                 const ManagementSnapshot &snapshot, const bool saved,
                 const bool carplay_error, const std::string &csrf_token) {
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
      "<script>document.addEventListener('submit',event=>{const "
      "form=event.target;"
      "if(form.method.toLowerCase()!=='post'||form.querySelector('input[name="
      "csrf]'))return;"
      "const "
      "input=document.createElement('input');input.type='hidden';input.name='"
      "csrf';"
      "input.value='" +
      csrf_token +
      "';form.append(input);},true);</script>"
      "<h1>AA2ACP</h1><p>Configure the pinned CarPlay head unit.</p>";
  const std::string csrf_input =
      "<input type=hidden name=csrf value=\"" + csrf_token + "\">";
  if (management_hotspot_needs_setup(config)) {
    output += "<p class=\"status error\">Change the default management "
              "hotspot password before using AA2ACP.</p><p>Connect to Wi-Fi "
              "network <b>" +
              html_escape(config.management_hotspot_ssid) + "</b>.</p>";
    if (snapshot.management_hotspot_password_pending) {
      output +=
          "<p>Your new password is ready. Applying it will disconnect this "
          "device from the hotspot. Its new name will be <b>" +
          html_escape(snapshot.pending_management_hotspot_ssid) +
          "</b>.</p><form method=post "
          "action=\"/management-hotspot/apply\">" +
          csrf_input +
          "<button type=submit>Apply "
          "new password</button></form>";
    } else {
      output += "<form method=post action=\"/management-hotspot\">" +
                csrf_input +
                "<label>New "
                "management hotspot password<input required minlength=8 "
                "type=password name=\"management_hotspot_passphrase\"></label>"
                "<label>Confirm new password<input required minlength=8 "
                "type=password name=\"management_hotspot_passphrase_confirm\">"
                "</label><label><input checked type=checkbox "
                "name=\"management_hotspot_change_ssid\" value=\"1\">Use "
                "a new hotspot name (recommended)</label><button type=submit>"
                "Continue</button></form>";
    }
    output += "<p><a href=\"/logs\">View recent logs</a></p></body></html>";
    return output;
  }
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
  if (snapshot.pairing_confirmation) {
    auto code = std::to_string(snapshot.pairing_confirmation->passkey);
    code.insert(0, 6 - std::min<std::size_t>(6, code.size()), '0');
    output += "<div class=status><p>Compare this Bluetooth pairing code with "
              "the code shown by the car:</p><p style=\"font-size:2rem;letter-"
              "spacing:.15em\"><b>" +
              html_escape(code) +
              "</b></p><form "
              "method=post action=\"/bluetooth-confirm\">" +
              csrf_input +
              "<input type=hidden "
              "name=\"id\" value=\"" +
              std::to_string(snapshot.pairing_confirmation->id) +
              "\"><button name=\"decision\" value=\"confirm\" type=submit>"
              "Codes match — Confirm</button><button name=\"decision\" "
              "value=\"reject\" type=submit>Reject</button></form></div>";
  }
  if (!snapshot.bluetooth_error.empty())
    output += "<p class=hint>Bluetooth refresh failed: " +
              html_escape(snapshot.bluetooth_error) + "</p>";
  output +=
      "<form id=\"scan-form\" method=post action=\"/scan\">" + csrf_input +
      "</form><form id=\"display-form\" method=post action=\"/display\">" +
      csrf_input + "</form><form method=post action=\"/config\">" + csrf_input +
      "<div id=\"bluetooth-picker\">";
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
  output += "</select></label><label>Management hotspot SSID<input "
            "name=\"management_hotspot_ssid\" value=\"" +
            html_escape(config.management_hotspot_ssid) +
            "\"></label><label>New management hotspot password (leave empty "
            "to keep)<input type=password "
            "name=\"management_hotspot_passphrase\"></label><label>Confirm "
            "new hotspot password<input type=password "
            "name=\"management_hotspot_passphrase_confirm\"></label><button "
            "type=submit>Save configuration</button></form>";
  output += "<p><a href=\"/logs\">View recent logs</a></p>";
  if (!config.head_unit_mac.empty() && !config.wifi_interface.empty())
    output += "<form method=post action=\"/carplay-prepare\">" + csrf_input +
              "<button type=submit " +
              std::string(snapshot.carplay_preflight_running ||
                                  snapshot.bluetooth_scan_running
                              ? "disabled"
                              : "") +
              ">Prepare/Test CarPlay</button></form>";
  if (!config.head_unit_mac.empty())
    output +=
        "<form method=post action=\"/bluetooth-forget\" onsubmit=\"return "
        "confirm('Forget the local Bluetooth bond? You may also need to "
        "clear the pairing on the head unit.');\">" +
        csrf_input + "<button type=submit " +
        std::string(snapshot.carplay_preflight_running ? "disabled" : "") +
        ">Forget Bluetooth bond</button></form><p class=hint>This removes "
        "the bond from AA2ACP only. It keeps the configured head unit; clear "
        "or restart pairing on the head unit too if it remains stuck.</p>";
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

std::string logs_page(const RecentLog &recent) {
  const auto css_class = [](const std::string_view line) {
    for (const auto level :
         {aa2acp::bridge::LogLevel::debug, aa2acp::bridge::LogLevel::info,
          aa2acp::bridge::LogLevel::warning, aa2acp::bridge::LogLevel::error}) {
      const auto prefix =
          "[" + std::string(aa2acp::bridge::log_level_name(level));
      if (line.find(prefix) != std::string_view::npos)
        return std::string("log-") +
               std::string(aa2acp::bridge::log_level_name(level));
    }
    return std::string{};
  };
  std::string rendered_logs;
  const auto logs = recent.snapshot();
  for (std::size_t offset = 0; offset < logs.size();) {
    const auto line_end = logs.find('\n', offset);
    const auto line = logs.substr(offset, line_end - offset);
    const auto css = css_class(line);
    if (!css.empty())
      rendered_logs += "<span class=\"" + css + "\">";
    rendered_logs += html_escape(line);
    if (!css.empty())
      rendered_logs += "</span>";
    rendered_logs += '\n';
    if (line_end == std::string::npos)
      break;
    offset = line_end + 1;
  }
  return "<!doctype html><html><head><meta name=\"viewport\" "
         "content=\"width=device-width,initial-scale=1\"><title>AA2ACP "
         "logs</title><style>body{font:16px sans-serif;max-width:60rem;"
         "margin:3rem auto;padding:0 1rem}pre{white-space:pre-wrap;"
         "word-break:break-word;background:#f5f5f5;padding:1rem}.log-debug"
         "{color:#666}.log-info{color:#174ea6}.log-warning{color:#8a5a00;"
         "background:#fff5cf}.log-error{color:#a61b1b;background:#ffe2e2}"
         "</style></head>"
         "<body><p><a href=\"/\">Back to management</a></p><h1>Recent "
         "logs</h1><p>Current service run; newest " +
         std::to_string(RecentLog::kMaximumBytes / 1024) +
         " KiB retained.</p><pre>" + rendered_logs + "</pre></body></html>";
}

class VideoSocketForwarder {
public:
  explicit VideoSocketForwarder(const std::filesystem::path &path)
      : path_(path) {
    if (const char *dump_path = std::getenv("AA2ACP_DUMP_H264");
        dump_path != nullptr && *dump_path != '\0') {
      dump_.open(dump_path, std::ios::binary | std::ios::trunc);
      if (dump_)
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
            << "Bridge daemon: capturing Android Auto H.264 to " << dump_path
            << '\n';
      else
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
            << "Bridge daemon: unable to capture Android Auto H.264 to "
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
    close_client();
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
      if (aa2acp::bridge::debug_logging_enabled() &&
          (received_video_count_ <= 5 || received_video_count_ % 60 == 0)) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
            << "Bridge daemon: Android Auto H.264 access unit #"
            << received_video_count_ << " (" << frame.size()
            << " bytes; NAL type:size=" << nalu_summary << ")\n";
      }
      if (keyframe) {
        // CarPlay setup can take longer than the ordinary frame queue. Keep
        // the latest decoder entry point and every dependent frame after it.
        keyframe_ = std::move(frame);
        frames_.clear();
        frames_.push_back(keyframe_);
        if (aa2acp::bridge::debug_logging_enabled())
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
              << "Bridge daemon: retained Android Auto H.264 keyframe "
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
      client_.store(client);
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
        if (client_.exchange(-1) == client)
          close(client);
        continue;
      }
      if (aa2acp::bridge::debug_logging_enabled())
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
            << "Bridge daemon: forwarded Android Auto H.264 "
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
      if (client_.exchange(-1) == client)
        close(client);
    }
  }

  void close_listener() {
    if (listener_ >= 0) {
      close(listener_);
      listener_ = -1;
    }
  }

  void close_client() {
    const auto client = client_.exchange(-1);
    if (client >= 0) {
      shutdown(client, SHUT_RDWR);
      close(client);
    }
  }

  std::filesystem::path path_;
  int listener_{-1};
  std::atomic<int> client_{-1};
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
    close_client();
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
    if (aa2acp::bridge::debug_logging_enabled() &&
        (received_packets_ <= 3 || received_packets_ % 500 == 0)) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
          << "Bridge daemon: Android Auto " << name_ << " audio packet #"
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
      client_.store(client);
      if (aa2acp::bridge::debug_logging_enabled())
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
            << "Bridge daemon: connected to Android Auto " << name_
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
      if (client_.exchange(-1) == client)
        close(client);
    }
  }

  void close_listener() {
    if (listener_ >= 0) {
      close(listener_);
      listener_ = -1;
    }
  }

  void close_client() {
    const auto client = client_.exchange(-1);
    if (client >= 0) {
      shutdown(client, SHUT_RDWR);
      close(client);
    }
  }

  std::filesystem::path path_;
  std::string name_;
  int listener_{-1};
  std::atomic<int> client_{-1};
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
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
      << "Bridge daemon: CarPlay worker did not stop after SIGTERM; sending "
         "SIGKILL\n";
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
  return carplay_worker->run(std::move(arguments), stop, phone_disconnected,
                             active_child);

  std::vector<char *> argv;
  for (auto &argument : arguments)
    argv.push_back(argument.data());
  argv.push_back(nullptr);
  int output_pipe[2];
  if (pipe2(output_pipe, O_CLOEXEC) != 0) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Bridge daemon: unable to capture CarPlay session output\n";
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
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Bridge daemon: unable to start CarPlay session\n";
    return 1;
  }
  auto forward_output = [&](const int timeout) {
    pollfd descriptor{output_pipe[0], POLLIN, 0};
    while (output_pipe[0] >= 0 && poll(&descriptor, 1, timeout) > 0) {
      std::array<char, 4096> output{};
      const auto count = read(output_pipe[0], output.data(), output.size());
      if (count > 0) {
        // Child output is already level-prefixed; preserve it verbatim.
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
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "Bridge daemon: CarPlay session started\n";
  for (;;) {
    forward_output(0);
    int status{};
    if (waitpid(child, &status, WNOHANG) == child) {
      while (output_pipe[0] >= 0)
        forward_output(100);
      active_child = -1;
      if (!WIFEXITED(status)) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
            << "Bridge daemon: cleaning up CarPlay Wi-Fi after worker "
               "failure\n";
        if (!aa2acp::iap2::leave_with_networkmanager(config.wifi_interface))
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
              << "Bridge daemon: unable to disconnect CarPlay Wi-Fi\n";
      }
      return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    if (stop.stop_requested() || phone_disconnected.load()) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
          << "Bridge daemon: stopping active CarPlay session\n";
      const auto forced = stop_carplay_process_group(child, &status);
      while (output_pipe[0] >= 0)
        forward_output(100);
      if (forced || !WIFEXITED(status)) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
            << "Bridge daemon: cleaning up CarPlay Wi-Fi after "
            << (forced ? "forced worker termination"
                       : "abnormal worker termination")
            << '\n';
        if (!aa2acp::iap2::leave_with_networkmanager(config.wifi_interface))
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
              << "Bridge daemon: unable to disconnect CarPlay Wi-Fi\n";
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
  std::atomic_bool carplay_preparation_failed{};
  std::atomic<pid_t> active_carplay_child{-1};
  aa2acp::bridge::H264Normalizer h264_normalizer;
  aa2acp::aa::WiredReceiver receiver(
      [&carplay_start_requested, &phone_disconnected,
       &carplay_preparation_failed, &active_carplay_child](const auto &event) {
        switch (event.type) {
        case aa2acp::aa::WiredReceiverEventType::waiting_for_phone:
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
              << "Bridge daemon: Android Auto USB idle: " << event.detail
              << '\n';
          break;
        case aa2acp::aa::WiredReceiverEventType::aoap_transport_ready:
          phone_disconnected = false;
          carplay_preparation_failed = false;
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
      carplay_preparation_failed = true;
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
  RecentLog recent_log;
  const auto log_path = file_logging ? next_daemon_log_path()
                                     : std::optional<std::filesystem::path>{};
  DaemonLog daemon_log(recent_log, log_path);
  if (log_path)
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
        << "Bridge daemon: logging to " << *log_path << '\n';
  std::filesystem::path config_path = aa2acp::bridge::default_config_path();
  int port = 8080;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--config" && index + 1 < argc)
      config_path = argv[++index];
    else if (argument == "--port" && index + 1 < argc)
      port = std::stoi(argv[++index]);
    else if (argument == "--no-file-log")
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
      [&config, &config_mutex](const std::stop_token stop) {
        run_wired_android_auto_receiver(
            [&config, &config_mutex] {
              std::lock_guard lock(config_mutex);
              return config;
            },
            stop);
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
              page(configuration, snapshot, saved, carplay_error, csrf_token));
    } else if (request.starts_with("GET /logs")) {
      respond(200, "text/html; charset=utf-8", logs_page(recent_log),
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
      if (start_scan)
        bluetooth_scan_worker = std::jthread([](const std::stop_token stop) {
          run_bluetooth_scan(management_state, stop);
        });
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
