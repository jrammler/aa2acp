#include "acp/bridge/bluez_inventory.hpp"
#include "acp/bridge/config.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace {

std::atomic_bool bluetooth_scan_running = false;

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

bool send_response(const int client, const int status, const char *type,
                   const std::string &body, const std::string &extra = {}) {
  const std::string response =
      "HTTP/1.1 " + std::to_string(status) +
      (status == 200   ? " OK\r\n"
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
    const auto separator = value.find(':');
    if (separator != std::string::npos &&
        value.substr(separator + 1).starts_with("wifi"))
      interfaces.push_back(value.substr(0, separator));
  }
  if (stream != nullptr)
    pclose(stream);
  return interfaces;
}

std::string page(const acp::bridge::Config &config, const bool saved,
                 const bool scan_requested) {
  std::string error;
  const auto devices = acp::bridge::list_bluez_devices(&error);
  const auto interfaces = wifi_interfaces();
  std::string output =
      "<!doctype html><html><head><meta name=\"viewport\" "
      "content=\"width=device-width,initial-scale=1\">"
      "<title>ACP-AA Bridge</title><style>body{font:16px "
      "sans-serif;max-width:38rem;margin:3rem auto;padding:0 1rem}"
      "label,select,input{display:block;width:100%;box-sizing:border-box;"
      "margin:.5rem 0}button{padding:.6rem 1rem;margin:.25rem 0}"
      ".hint{color:#555}.status{padding:.6rem;background:#eef7ee}</style></"
      "head><body>"
      "<h1>ACP-AA Bridge</h1><p>Configure the pinned CarPlay head unit.</p>";
  if (saved)
    output += "<p class=status>Configuration saved.</p>";
  if (scan_requested || bluetooth_scan_running)
    output +=
        "<p class=status>Bluetooth discovery is running in the background. "
        "Refresh this page to see devices as BlueZ discovers them.</p>";
  if (!error.empty())
    output +=
        "<p class=hint>BlueZ inventory unavailable: " + html_escape(error) +
        "</p>";
  output += "<form method=post action=\"/config\">"
            "<label>Known or discovered Bluetooth device<select "
            "name=\"head_unit_mac\">"
            "<option value=\"\">Keep the configured device (use manual field "
            "below)</option>";
  for (const auto &device : devices) {
    const auto selected =
        device.address == config.head_unit_mac ? " selected" : "";
    auto name = device.name.empty() ? "Unnamed device" : device.name;
    output += "<option value=\"" + html_escape(device.address) + "\"" +
              selected + ">" + html_escape(name) + " — " +
              html_escape(device.address) + (device.paired ? " (paired)" : "") +
              (device.connected ? " (connected)" : "") + "</option>";
  }
  output += "</select></label><p class=hint>Paired devices and discoveries "
            "both come directly from BlueZ. A CarPlay capability filter is "
            "deliberately not applied yet, because its advertised identifiers "
            "are not sufficient to safely identify every head unit.</p>"
            "<label>Manual Bluetooth MAC (leave unchanged to keep the saved "
            "device)<input name=\"manual_mac\" value=\"" +
            html_escape(config.head_unit_mac) +
            "\" placeholder=\"[redacted-device-address]\"></label>"
            "<label>Wi-Fi interface<select name=\"wifi_interface\">";
  bool current_interface_present = false;
  for (const auto &interface : interfaces) {
    const auto selected = interface == config.wifi_interface ? " selected" : "";
    current_interface_present =
        current_interface_present || interface == config.wifi_interface;
    output += "<option value=\"" + html_escape(interface) + "\"" + selected +
              ">" + html_escape(interface) + "</option>";
  }
  if (!current_interface_present)
    output += "<option selected value=\"" + html_escape(config.wifi_interface) +
              "\">" + html_escape(config.wifi_interface) +
              " (configured)</option>";
  output +=
      "</select></label><button type=submit>Save configuration</button></form>"
      "<form method=post action=\"/scan\"><button type=submit" +
      std::string(bluetooth_scan_running ? " disabled" : "") +
      ">Scan Bluetooth devices (LE, then classic)</button></form>"
      "<p class=hint>Discovery runs independently of the web request, so "
      "reloading this page remains responsive. The Wi-Fi list is always "
      "rendered with the page.</p></body></html>";
  return output;
}

int run_carplay_session(const char *program_path,
                        const acp::bridge::Config &config,
                        const int signal_fd) {
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
    pollfd signal_poll{signal_fd, POLLIN, 0};
    if (poll(&signal_poll, 1, 100) > 0 && (signal_poll.revents & POLLIN) != 0) {
      signalfd_siginfo signal_info{};
      if (read(signal_fd, &signal_info, sizeof(signal_info)) !=
          static_cast<ssize_t>(sizeof(signal_info)))
        continue;
      std::cout << "Bridge daemon: stopping active CarPlay session\n";
      kill(child, SIGTERM);
      waitpid(child, &status, 0);
      return 128 + SIGTERM;
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  std::filesystem::path config_path = "/var/lib/acp-aa-bridge/config";
  int port = 8080;
  bool run_session = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--config" && index + 1 < argc)
      config_path = argv[++index];
    else if (argument == "--port" && index + 1 < argc)
      port = std::stoi(argv[++index]);
    else if (argument == "--run")
      run_session = true;
    else {
      std::cerr
          << "usage: bridge-daemon [--config PATH] [--port PORT] [--run]\n";
      return 2;
    }
  }
  auto config =
      acp::bridge::load_config(config_path)
          .value_or(acp::bridge::Config{
              "", "wlan0", "/var/lib/acp-aa-bridge/airplay-pairing.bin"});
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
  if (run_session) {
    const auto result = run_carplay_session(argv[0], config, signal_fd);
    close(signal_fd);
    return result;
  }
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
    if (request.starts_with("GET / ") || request.starts_with("GET /?")) {
      const bool saved = request.find("saved=1") != std::string::npos;
      std::cout << "Management: GET / ("
                << acp::bridge::list_bluez_devices().size()
                << " BlueZ devices)\n";
      send_response(client, 200, "text/html; charset=utf-8",
                    page(config, saved, false));
    } else if (request.starts_with("POST /scan ")) {
      std::cout << "Management: Bluetooth scan requested\n";
      if (!bluetooth_scan_running.exchange(true)) {
        std::thread([] {
          const auto log = [](const std::string &message) {
            std::cout << "Management Bluetooth: " << message << '\n';
          };
          acp::bridge::discover_bluez_devices("le", 30, log);
          acp::bridge::discover_bluez_devices("bredr", 15, log);
          bluetooth_scan_running = false;
          std::cout << "Management Bluetooth: discovery finished\n";
        }).detach();
      }
      send_response(client, 303, "text/plain", "", "Location: /?scan=1\r\n");
    } else if (request.starts_with("POST /config ")) {
      const auto manual = form_field(body, "manual_mac");
      const auto selected = form_field(body, "head_unit_mac");
      const auto wifi = form_field(body, "wifi_interface");
      const auto mac = manual && !manual->empty()
                           ? *manual
                           : selected.value_or(config.head_unit_mac);
      if (wifi && !mac.empty() &&
          acp::bridge::save_config(
              config_path, {mac, *wifi, config.airplay_pairing_store})) {
        config = {mac, *wifi, config.airplay_pairing_store};
        std::cout << "Management: saved head unit " << config.head_unit_mac
                  << " on " << config.wifi_interface << '\n';
        send_response(client, 303, "text/plain", "", "Location: /?saved=1\r\n");
      } else {
        std::cout << "Management: rejected invalid configuration\n";
        send_response(client, 400, "text/plain", "Invalid configuration\n");
      }
    } else {
      std::cout << "Management: unknown request\n";
      send_response(client, 400, "text/plain", "Unknown endpoint\n");
    }
    close(client);
  }
  close(listener);
  close(signal_fd);
  std::cout << "Bridge daemon: stopped\n";
}
