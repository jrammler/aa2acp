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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace {

std::atomic_bool bluetooth_scan_running = false;
std::mutex bluetooth_devices_mutex;
std::map<std::string, std::string> scanned_bluetooth_devices;

constexpr char kPage[] =
    R"HTML(<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><title>ACP-AA Bridge</title><style>body{font:16px sans-serif;max-width:34rem;margin:3rem auto;padding:0 1rem}label,select,input{display:block;width:100%;box-sizing:border-box;margin:.5rem 0}button{padding:.6rem 1rem}</style><h1>ACP-AA Bridge</h1><p>Configure the pinned CarPlay head unit.</p><form><label>Head-unit Bluetooth device<select id="head_unit_mac" required></select></label><button type="button" id="scan">Scan for devices</button><label>Manual MAC fallback<input id="manual_mac" placeholder="[redacted-device-address]"></label><label>Wi-Fi interface<select id="wifi_interface" required></select></label><button>Save</button></form><p id="result"></p><script>const add=(id,items,current,bt)=>{let s=document.querySelector(id);s.innerHTML='';items.forEach(x=>{let [value,name]=bt?x.split('|'): [x,x],o=new Option(bt?name+' — '+value:name,value);if(value===current)o.selected=true;s.add(o)});if(!s.options.length)s.add(new Option('No devices found',''))};const devices=async(current=head_unit_mac.value)=>add('#head_unit_mac',await fetch('/api/bluetooth-devices').then(r=>r.json()),current,true);Promise.all([fetch('/api/config').then(r=>r.json()),fetch('/api/wifi-interfaces').then(r=>r.json())]).then(async([c,w])=>{manual_mac.value=c.head_unit_mac;await devices(c.head_unit_mac);add('#wifi_interface',w,c.wifi_interface,false)});scan.onclick=async()=>{scan.disabled=true;result.textContent='Scanning…';await fetch('/api/bluetooth-scan');for(let i=0;i<24;i++){await new Promise(r=>setTimeout(r,2000));await devices();if(!(await fetch('/api/bluetooth-scan-status').then(r=>r.json())).running)break}result.textContent='Scan complete.';scan.disabled=false};document.querySelector('form').onsubmit=async e=>{e.preventDefault();let c={head_unit_mac:manual_mac.value||head_unit_mac.value,wifi_interface:wifi_interface.value};let r=await fetch('/api/config',{method:'PUT',body:JSON.stringify(c)});result.textContent=r.ok?'Saved.':'Save failed.'}</script>)HTML";

constexpr char kPageExtension[] = R"HTML(<script>
const keepConfigured=()=>{if(!head_unit_mac.querySelector('option[value=""]'))head_unit_mac.add(new Option('Keep configured device',''),0)};
setInterval(keepConfigured,250);keepConfigured();
head_unit_mac.onchange=()=>{if(head_unit_mac.value)manual_mac.value=''};
document.querySelector('form').onsubmit=async e=>{e.preventDefault();let c={head_unit_mac:manual_mac.value||head_unit_mac.value,wifi_interface:wifi_interface.value};let r=await fetch('/api/config',{method:'PUT',body:JSON.stringify(c)});result.textContent=r.ok?'Saved.':'Save failed.'};
</script>)HTML";

std::string json(const acp::bridge::Config &config) {
  // Values are validated as single-line key/value data by the config store.
  return "{\"head_unit_mac\":\"" + config.head_unit_mac +
         "\",\"wifi_interface\":\"" + config.wifi_interface + "\"}";
}

std::optional<std::string> field(const std::string &body,
                                 const std::string &name) {
  const auto prefix = "\"" + name + "\":\"";
  const auto start = body.find(prefix);
  if (start == std::string::npos)
    return std::nullopt;
  const auto value_start = start + prefix.size();
  const auto end = body.find('"', value_start);
  if (end == std::string::npos)
    return std::nullopt;
  return body.substr(value_start, end - value_start);
}

bool send_response(const int client, const int status, const char *type,
                   const std::string &body) {
  const std::string response =
      "HTTP/1.1 " + std::to_string(status) +
      (status == 200 ? " OK\r\n" : " Bad Request\r\n") +
      "Content-Type: " + type +
      "\r\nContent-Length: " + std::to_string(body.size()) +
      "\r\nConnection: close\r\n\r\n" + body;
  return send(client, response.data(), response.size(), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(response.size());
}

void capture_bluetooth_scan(const char *command) {
  FILE *stream = popen(command, "r");
  std::array<char, 512> line{};
  while (stream != nullptr &&
         fgets(line.data(), line.size(), stream) != nullptr) {
    std::string value(line.data());
    for (std::size_t index = 0; index < value.size();) {
      if (static_cast<unsigned char>(value[index]) == 0x1b &&
          index + 1 < value.size() && value[index + 1] == '[') {
        auto end = index + 2;
        while (end < value.size() && !(value[end] >= '@' && value[end] <= '~'))
          ++end;
        value.erase(index, std::min(end + 1, value.size()) - index);
      } else {
        ++index;
      }
    }
    const auto marker = value.find("Device ");
    if (marker == std::string::npos || value.size() < marker + 24)
      continue;
    const auto mac = value.substr(marker + 7, 17);
    if (mac.find(':') == std::string::npos)
      continue;
    auto name = value.substr(marker + 25);
    name.erase(
        std::remove_if(name.begin(), name.end(),
                       [](unsigned char character) { return character < 32; }),
        name.end());
    static constexpr std::array<std::string_view, 8> properties{
        "RSSI",        "ManufacturerData", "AdvertisingFlags", "UUID",
        "ServiceData", "Appearance",       "Modalias",         "TxPower"};
    if (name.empty() || std::any_of(properties.begin(), properties.end(),
                                    [&name](const auto property) {
                                      return name.starts_with(property);
                                    }))
      continue;
    std::lock_guard lock(bluetooth_devices_mutex);
    scanned_bluetooth_devices.try_emplace(mac, name);
  }
  if (stream != nullptr)
    pclose(stream);
}

std::string scanned_bluetooth_json() {
  std::lock_guard lock(bluetooth_devices_mutex);
  std::string output{"["};
  for (const auto &[mac, name] : scanned_bluetooth_devices) {
    if (output.size() != 1)
      output += ',';
    output += '"' + mac + "|" + name + '"';
  }
  return output + ']';
}

std::string command_json(const char *command) {
  std::vector<std::string> values;
  FILE *stream = popen(command, "r");
  std::array<char, 256> line{};
  while (stream != nullptr &&
         fgets(line.data(), line.size(), stream) != nullptr) {
    std::string value(line.data());
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
      value.pop_back();
    if (!value.empty())
      values.push_back(value);
  }
  if (stream != nullptr)
    pclose(stream);
  std::string output{"["};
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0)
      output += ',';
    output += '"' + values[index] + '"';
  }
  return output + ']';
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
      const auto signal_bytes =
          read(signal_fd, &signal_info, sizeof(signal_info));
      if (signal_bytes != static_cast<ssize_t>(sizeof(signal_info)))
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
  auto config = acp::bridge::load_config(config_path)
                    .value_or(acp::bridge::Config{
                        "", "wlan0", "/var/lib/acp-aa-bridge/airplay"});
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
      const auto signal_bytes =
          read(signal_fd, &signal_info, sizeof(signal_info));
      if (signal_bytes != static_cast<ssize_t>(sizeof(signal_info)))
        continue;
      std::cout << "Bridge daemon: graceful shutdown requested\n";
      break;
    }
    if ((descriptors[0].revents & POLLIN) == 0)
      continue;
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0)
      continue;
    std::array<char, 4096> buffer{};
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos) {
      const auto count = recv(client, buffer.data(), buffer.size(), 0);
      if (count <= 0)
        break;
      request.append(buffer.data(), static_cast<std::size_t>(count));
      if (request.size() > 16 * 1024)
        break;
    }
    const auto header_end = request.find("\r\n\r\n");
    const auto content_length_marker = request.find("Content-Length: ");
    if (header_end != std::string::npos &&
        content_length_marker != std::string::npos) {
      const auto length_start = content_length_marker + 16;
      const auto length_end = request.find("\r\n", length_start);
      const auto length =
          length_end == std::string::npos
              ? 0U
              : static_cast<std::size_t>(std::stoul(
                    request.substr(length_start, length_end - length_start)));
      while (request.size() < header_end + 4 + length) {
        const auto more = recv(client, buffer.data(), buffer.size(), 0);
        if (more <= 0)
          break;
        request.append(buffer.data(), static_cast<std::size_t>(more));
      }
    }
    const auto split = request.find("\r\n\r\n");
    const auto body =
        split == std::string::npos ? "" : request.substr(split + 4);
    if (request.starts_with("GET / "))
      send_response(client, 200, "text/html; charset=utf-8",
                    std::string(kPage) + kPageExtension);
    else if (request.starts_with("GET /api/config "))
      send_response(client, 200, "application/json", json(config));
    else if (request.starts_with("GET /api/bluetooth-devices ")) {
      bool empty = false;
      {
        std::lock_guard lock(bluetooth_devices_mutex);
        empty = scanned_bluetooth_devices.empty();
      }
      if (empty && !bluetooth_scan_running)
        capture_bluetooth_scan("bluetoothctl devices");
      send_response(client, 200, "application/json", scanned_bluetooth_json());
    } else if (request.starts_with("GET /api/bluetooth-scan ")) {
      if (!bluetooth_scan_running.exchange(true)) {
        std::thread([] {
          capture_bluetooth_scan("bluetoothctl --timeout 30 scan le 2>&1");
          capture_bluetooth_scan("bluetoothctl --timeout 15 scan bredr 2>&1");
          bluetooth_scan_running = false;
        }).detach();
      }
      send_response(client, 200, "application/json", "{\"running\":true}");
    } else if (request.starts_with("GET /api/bluetooth-scan-status ")) {
      send_response(client, 200, "application/json",
                    bluetooth_scan_running ? "{\"running\":true}"
                                           : "{\"running\":false}");
    } else if (request.starts_with("GET /api/wifi-interfaces "))
      send_response(client, 200, "application/json",
                    command_json("nmcli -t -f DEVICE,TYPE device status | awk "
                                 "-F: '$2 == \"wifi\" {print $1}'"));
    else if (request.starts_with("PUT /api/config ")) {
      const auto mac = field(body, "head_unit_mac");
      const auto wifi = field(body, "wifi_interface");
      if (mac && wifi &&
          acp::bridge::save_config(
              config_path, {*mac, *wifi, config.airplay_pairing_store})) {
        config = {*mac, *wifi, config.airplay_pairing_store};
        send_response(client, 200, "application/json", json(config));
      } else {
        send_response(client, 400, "text/plain", "Invalid configuration\n");
      }
    } else {
      send_response(client, 400, "text/plain", "Unknown endpoint\n");
    }
    close(client);
  }
  close(listener);
  close(signal_fd);
  std::cout << "Bridge daemon: stopped\n";
}
