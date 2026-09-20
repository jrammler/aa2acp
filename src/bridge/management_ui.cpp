#include "aa2acp/bridge/management_ui.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <random>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

#include <signal.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>

#include "aa2acp/bridge/logging.hpp"

extern char **environ;

namespace aa2acp::bridge::management {
namespace {

std::string normalized_bluetooth_address(const std::string &value) {
  std::string normalized;
  for (const unsigned char character : value) {
    if (std::isxdigit(character))
      normalized += static_cast<char>(std::toupper(character));
  }
  return normalized;
}

bool is_unnamed(const BluetoothDevice &device) {
  // BlueZ commonly uses the address itself as Alias when no advertised name is
  // available, with either colons or hyphens as separators. Treat that as
  // unnamed rather than displaying the address twice.
  return device.name.empty() ||
         normalized_bluetooth_address(device.name) ==
             normalized_bluetooth_address(device.address);
}

} // namespace

bool valid_hotspot_passphrase(const std::string_view passphrase) {
  return passphrase != kDefaultHotspotPassphrase && passphrase.size() >= 8 &&
         passphrase.find_first_of("\r\n=") == std::string_view::npos;
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

HttpRequestFraming frame_http_request(const std::string_view request,
                                      const std::size_t maximum_headers,
                                      const std::size_t maximum_body) {
  const auto header_end = request.find("\r\n\r\n");
  if (header_end == std::string_view::npos)
    return {request.size() > maximum_headers
                ? HttpRequestFraming::Status::invalid
                : HttpRequestFraming::Status::incomplete,
            {}};
  if (header_end > maximum_headers)
    return {HttpRequestFraming::Status::invalid, {}};

  std::optional<std::size_t> body_size;
  std::size_t line_start{};
  while (line_start < header_end) {
    const auto line_end = request.find("\r\n", line_start);
    const auto line =
        request.substr(line_start, (line_end == std::string_view::npos ||
                                    line_end > header_end)
                                       ? header_end - line_start
                                       : line_end - line_start);
    const auto separator = line.find(':');
    if (separator != std::string_view::npos) {
      std::string name(line.substr(0, separator));
      std::ranges::transform(name, name.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
      });
      if (name == "content-length") {
        const auto value_start = line.find_first_not_of(" \t", separator + 1);
        auto value_end = line.size();
        while (value_end > separator + 1 &&
               std::isspace(static_cast<unsigned char>(line[value_end - 1])))
          --value_end;
        const auto value =
            value_start == std::string_view::npos || value_start >= value_end
                ? std::string_view{}
                : line.substr(value_start, value_end - value_start);
        std::size_t parsed{};
        const auto result =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (value.empty() || result.ec != std::errc{} ||
            result.ptr != value.data() + value.size() ||
            (body_size && *body_size != parsed))
          return {HttpRequestFraming::Status::invalid, {}};
        body_size = parsed;
      }
    }
    if (line_end == std::string_view::npos || line_end >= header_end)
      break;
    line_start = line_end + 2;
  }
  const auto body_start = header_end + 4;
  const auto length = body_size.value_or(0);
  if (length > maximum_body || body_start > request.max_size() - length)
    return {HttpRequestFraming::Status::invalid, {}};
  const auto request_size = body_start + length;
  if (request.size() < request_size)
    return {HttpRequestFraming::Status::incomplete, {}};
  if (request.size() > request_size)
    return {HttpRequestFraming::Status::invalid, {}};
  return {HttpRequestFraming::Status::complete,
          request.substr(body_start, length)};
}

bool send_response(const int client, const int status, const char *type,
                   const std::string &body, const std::string &extra) {
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

std::optional<std::string> run_nmcli_device_status() {
  int output_pipe[2]{-1, -1};
  if (pipe2(output_pipe, O_CLOEXEC) != 0)
    return std::nullopt;
  std::vector<std::string> arguments{"nmcli",       "-t",     "-f",
                                     "DEVICE,TYPE", "device", "status"};
  std::vector<char *> argv;
  argv.reserve(arguments.size() + 1);
  for (auto &argument : arguments)
    argv.push_back(argument.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
  posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
  pid_t child{};
  const auto result = posix_spawnp(&child, argv.front(), &actions, nullptr,
                                   argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(output_pipe[1]);
  if (result != 0) {
    close(output_pipe[0]);
    return std::nullopt;
  }
  const auto flags = fcntl(output_pipe[0], F_GETFL);
  if (flags < 0 || fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK) != 0) {
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    close(output_pipe[0]);
    return std::nullopt;
  }
  std::string output;
  constexpr std::size_t kMaximumOutput = 64 * 1024;
  int status{};
  bool complete = false;
  bool overflow = false;
  const auto drain = [&] {
    std::array<char, 4096> buffer{};
    for (;;) {
      const auto count = read(output_pipe[0], buffer.data(), buffer.size());
      if (count > 0) {
        if (output.size() + static_cast<std::size_t>(count) > kMaximumOutput) {
          overflow = true;
          return;
        }
        output.append(buffer.data(), static_cast<std::size_t>(count));
        continue;
      }
      if (count < 0 && errno == EINTR)
        continue;
      return;
    }
  };
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!complete && !overflow) {
    drain();
    const auto waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      complete = true;
      break;
    }
    if (waited < 0 && errno != EINTR)
      break;
    if (std::chrono::steady_clock::now() >= deadline)
      break;
    pollfd descriptor{output_pipe[0], POLLIN, 0};
    (void)poll(&descriptor, 1, 50);
  }
  if (!complete || overflow) {
    kill(child, SIGKILL);
    do {
      complete = waitpid(child, &status, 0) == child;
    } while (!complete && errno == EINTR);
  }
  drain();
  close(output_pipe[0]);
  if (overflow || !complete || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return std::nullopt;
  return output;
}

std::vector<std::string> wifi_interfaces() {
  std::vector<std::string> interfaces;
  const auto output = run_nmcli_device_status();
  if (!output) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
        << "Management: nmcli Wi-Fi inventory failed or timed out\n";
    return interfaces;
  }
  std::istringstream lines(*output);
  std::string value;
  while (std::getline(lines, value)) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
      value.pop_back();
    const auto separator = value.find(':');
    if (separator != std::string::npos && value.substr(separator + 1) == "wifi")
      interfaces.push_back(value.substr(0, separator));
  }
  return interfaces;
}

std::string render_page(const Config &config, const Snapshot &snapshot,
                        const bool hotspot_needs_setup, const bool saved,
                        const bool carplay_error,
                        const std::string &csrf_token) {
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
      "\" data-preflight-state=\"" +
      std::to_string(static_cast<int>(snapshot.preflight_state)) +
      "\" data-preflight-rev=\"" + std::to_string(snapshot.preflight_revision) +
      "\" data-preflight-active=\"" +
      (aa2acp::bridge::management::preflight_active(snapshot.preflight_state)
           ? "1"
           : "0") +
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
  if (hotspot_needs_setup) {
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
  if (!snapshot.preflight_detail.empty()) {
    output += "<p class=status>CarPlay preparation: " +
              html_escape(snapshot.preflight_detail) + "</p>";
  } else if (snapshot.preflight_state !=
             aa2acp::bridge::management::PreflightState::idle) {
    output += "<p class=status>CarPlay preparation: " +
              std::to_string(static_cast<int>(snapshot.preflight_state)) +
              "</p>";
  }
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
#ifdef AA2ACP_VERSION
  output += "<p class=hint>AA2ACP v" AA2ACP_VERSION "</p>";
#endif
  if (!config.head_unit_mac.empty() && !config.wifi_interface.empty())
    output += "<form method=post action=\"/carplay-prepare\">" + csrf_input +
              "<button type=submit " +
              std::string(aa2acp::bridge::management::preflight_active(
                              snapshot.preflight_state) ||
                                  snapshot.bluetooth_scan_running
                              ? "disabled"
                              : "") +
              ">Prepare/Test CarPlay</button></form>";
  if (!config.head_unit_mac.empty())
    output +=
        "<form method=post action=\"/head-unit-forget\" onsubmit=\"return "
        "confirm('Clear all local state for this head unit? You will need to "
        "start CarPlay pairing again on the head unit.');\">" +
        csrf_input + "<button type=submit " +
        std::string(aa2acp::bridge::management::preflight_active(
                        snapshot.preflight_state) ||
                            snapshot.bluetooth_scan_running
                        ? "disabled"
                        : "") +
        ">Forget head unit</button></form><p class=hint>This keeps the "
        "configured head unit but clears its local Bluetooth bond, AirPlay "
        "pairing, and cached capabilities.</p>";
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
      "<script>(()=>{if(document.body.dataset.preflightActive!=='1')return;"
      "const "
      "state=encodeURIComponent(document.body.dataset.preflightState||'0');"
      "const rev=encodeURIComponent(document.body.dataset.preflightRev||'0');"
      "const poll=async()=>{try{const response=await "
      "fetch('/carplay-prepare-status?state='+state+'&rev='+rev);"
      "if(response.status===205){location.reload();return;}setTimeout(poll,"
      "1000);}catch{setTimeout(poll,2000);}};poll();})();</script></"
      "body></html>";
  return output;
}

std::string render_logs_page(const std::string &logs,
                             const std::size_t retained_kib) {
  const auto css_class = [](const std::string_view line) {
    for (const auto level : {LogLevel::debug, LogLevel::info, LogLevel::warning,
                             LogLevel::error}) {
      const auto prefix = "[" + std::string(log_level_name(level));
      if (line.find(prefix) != std::string_view::npos)
        return std::string("log-") + std::string(log_level_name(level));
    }
    return std::string{};
  };
  std::string rendered_logs;
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
         std::to_string(retained_kib) + " KiB retained.</p><pre>" +
         rendered_logs + "</pre></body></html>";
}

} // namespace aa2acp::bridge::management
