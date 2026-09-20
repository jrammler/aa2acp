#include "aa2acp/airplay/session.hpp"
#include "aa2acp/airplay/bplist.hpp"
#include "aa2acp/airplay/control_cipher.hpp"
#include "aa2acp/airplay/crypto.hpp"
#include "aa2acp/airplay/head_unit_capabilities.hpp"
#include "aa2acp/airplay/pairing_store.hpp"
#include "aa2acp/airplay/rtsp.hpp"
#include "aa2acp/airplay/srp.hpp"
#include "aa2acp/bridge/logging.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

aa2acp::airplay::RequestHeaders pairing_headers(const int hkp) {
  constexpr auto kAppleEpoch = std::chrono::seconds(978307200);
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()) -
                   kAppleEpoch;
  aa2acp::airplay::RequestHeaders headers{
      {"X-Apple-AbsoluteTime", std::to_string(now.count())},
      {"X-Apple-HKP", std::to_string(hkp)},
      {"X-Apple-Client-Name", "User"},
      {"User-Agent", "AirPlay/950.7.1"},
  };
  if (hkp == 2) {
    headers.emplace_back("X-Apple-PD", "1");
  }
  return headers;
}

std::optional<std::uint64_t> random_stream_connection_id() {
  std::array<unsigned char, 8> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    return std::nullopt;
  std::uint64_t id{};
  for (const auto byte : bytes) {
    id = (id << 8) | byte;
  }
  return id == 0 ? std::nullopt : std::optional(id);
}

std::optional<std::string> random_controller_id() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    return std::nullopt;
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);

  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10)
      result.push_back('-');
    result.push_back(hex[bytes[index] >> 4]);
    result.push_back(hex[bytes[index] & 0x0f]);
  }
  return result;
}

int connect_tcp(const std::string &host, const std::string &port) {
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0) {
    return -1;
  }
  int socket_fd = -1;
  for (auto *address = addresses; address != nullptr;
       address = address->ai_next) {
    socket_fd =
        socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket_fd >= 0 &&
        connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
      break;
    }
    if (socket_fd >= 0)
      close(socket_fd);
    socket_fd = -1;
  }
  freeaddrinfo(addresses);
  return socket_fd;
}

int connect_tcp_with_timeout(const std::string &host, const std::string &port,
                             const std::chrono::milliseconds timeout,
                             std::string *error) {
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0) {
    if (error != nullptr)
      *error = "name resolution failed";
    return -1;
  }
  int socket_fd = -1;
  std::string last_error = "connection failed";
  for (auto *address = addresses; address != nullptr;
       address = address->ai_next) {
    socket_fd =
        socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket_fd < 0) {
      last_error = std::strerror(errno);
      continue;
    }
    const auto flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      last_error = std::strerror(errno);
      close(socket_fd);
      socket_fd = -1;
      continue;
    }
    if (connect(socket_fd, address->ai_addr, address->ai_addrlen) != 0 &&
        errno != EINPROGRESS) {
      last_error = std::strerror(errno);
      close(socket_fd);
      socket_fd = -1;
      continue;
    }
    pollfd descriptor{socket_fd, POLLOUT, 0};
    const auto ready = poll(&descriptor, 1, static_cast<int>(timeout.count()));
    int socket_error = ready > 0 ? 0 : ETIMEDOUT;
    socklen_t socket_error_size = sizeof(socket_error);
    if (ready > 0 && getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error,
                                &socket_error_size) != 0)
      socket_error = errno;
    if (socket_error == 0 && fcntl(socket_fd, F_SETFL, flags) == 0)
      break;
    last_error = std::strerror(socket_error);
    close(socket_fd);
    socket_fd = -1;
  }
  freeaddrinfo(addresses);
  if (socket_fd < 0 && error != nullptr)
    *error = last_error;
  return socket_fd;
}

int connect_udp(const std::string &host, const std::string &port) {
  addrinfo hints{};
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo *addresses = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0)
    return -1;
  int socket_fd = -1;
  for (auto *address = addresses; address != nullptr;
       address = address->ai_next) {
    socket_fd =
        socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket_fd >= 0 &&
        connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0)
      break;
    if (socket_fd >= 0)
      close(socket_fd);
    socket_fd = -1;
  }
  freeaddrinfo(addresses);
  return socket_fd;
}

bool send_all(const int socket_fd, const std::span<const std::uint8_t> bytes) {
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto count = send(socket_fd, bytes.data() + offset,
                            bytes.size() - offset, MSG_NOSIGNAL);
    if (count <= 0)
      return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

std::optional<aa2acp::airplay::Response>
send_encrypted(const int socket_fd, aa2acp::airplay::ControlCipher &cipher,
               aa2acp::airplay::Bytes &encrypted_buffer,
               const std::span<const std::uint8_t> plaintext,
               const std::optional<int> &expected_cseq,
               const int timeout_seconds,
               const std::function<bool()> &stop_requested,
               const std::string_view operation = "encrypted RTSP request") {
  const auto encrypted = cipher.encrypt(plaintext);
  if (!encrypted) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "AirPlay: " << operation << " could not be encrypted\n";
    return std::nullopt;
  }
  if (!send_all(socket_fd, *encrypted)) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "AirPlay: " << operation << " send failed: " << std::strerror(errno)
        << '\n';
    return std::nullopt;
  }
  aa2acp::airplay::Bytes response_plaintext;
  std::array<std::uint8_t, 4096> buffer{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  while (std::chrono::steady_clock::now() < deadline &&
         (!stop_requested || !stop_requested())) {
    while (true) {
      const auto frame = cipher.decrypt_one(encrypted_buffer);
      if (!frame) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
            << "AirPlay: " << operation << " received undecryptable control "
            << "data (" << encrypted_buffer.size() << " buffered byte(s))\n";
        return std::nullopt;
      }
      if (frame->empty())
        break;
      response_plaintext.insert(response_plaintext.end(), frame->begin(),
                                frame->end());
      constexpr std::size_t kMaxResponseBytes = 1024 * 1024;
      if (response_plaintext.size() > kMaxResponseBytes) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
            << "AirPlay: " << operation << " response exceeded 1 MiB without "
            << "completing\n";
        return std::nullopt;
      }
      const auto complete =
          aa2acp::airplay::complete_response_size(response_plaintext);
      if (!complete)
        continue;
      auto parsed = aa2acp::airplay::parse_response(response_plaintext);
      if (!parsed) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
            << "AirPlay: " << operation << " returned an invalid RTSP "
            << "response (" << response_plaintext.size() << " byte(s))\n";
        return std::nullopt;
      }
      if (expected_cseq) {
        const auto header = parsed->headers.find("cseq");
        if (header == parsed->headers.end() ||
            header->second != std::to_string(*expected_cseq)) {
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
              << "AirPlay: discarding response with "
              << (header == parsed->headers.end() ? std::string("no CSeq")
                                                  : "CSeq " + header->second)
              << " while waiting for " << *expected_cseq << '\n';
          response_plaintext.erase(response_plaintext.begin(),
                                   response_plaintext.begin() +
                                       static_cast<std::ptrdiff_t>(*complete));
          break;
        }
      }
      return parsed;
    }
    pollfd descriptor{socket_fd, POLLIN, 0};
    const auto ready = poll(&descriptor, 1, 100);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "AirPlay: " << operation
          << " poll failed: " << std::strerror(errno) << '\n';
      return std::nullopt;
    }
    if (ready == 0)
      continue;
    if (descriptor.revents & (POLLERR | POLLNVAL)) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "AirPlay: " << operation << " control socket error (revents=0x"
          << std::hex << descriptor.revents << std::dec << ")\n";
      return std::nullopt;
    }
    const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
    if (count == 0) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "AirPlay: " << operation << " control socket closed by receiver\n";
      return std::nullopt;
    }
    if (count < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "AirPlay: " << operation
          << " receive failed: " << std::strerror(errno) << '\n';
      return std::nullopt;
    }
    encrypted_buffer.insert(encrypted_buffer.end(), buffer.begin(),
                            buffer.begin() + count);
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
      << "AirPlay: " << operation
      << (stop_requested && stop_requested()
              ? " stopped before a response\n"
              : " timed out waiting for response\n");
  return std::nullopt;
}

std::uint64_t ntp_timestamp() {
  constexpr std::uint64_t kNtpUnixEpochOffset = 2208988800ULL;
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
  const auto remainder = now - seconds;
  const auto fraction =
      std::chrono::duration_cast<std::chrono::nanoseconds>(remainder).count();
  return ((static_cast<std::uint64_t>(seconds.count()) + kNtpUnixEpochOffset)
          << 32) |
         ((static_cast<std::uint64_t>(fraction) << 32) / 1000000000ULL);
}

void write_ntp_timestamp(std::uint8_t *destination, const std::uint64_t value) {
  for (int index = 0; index < 8; ++index)
    destination[index] = static_cast<std::uint8_t>(value >> (56 - index * 8));
}

int bind_timing_socket(std::uint16_t &port) {
  const auto socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0)
    return -1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(socket_fd, reinterpret_cast<const sockaddr *>(&address),
           sizeof(address)) != 0) {
    close(socket_fd);
    return -1;
  }
  socklen_t address_size = sizeof(address);
  if (getsockname(socket_fd, reinterpret_cast<sockaddr *>(&address),
                  &address_size) != 0) {
    close(socket_fd);
    return -1;
  }
  port = ntohs(address.sin_port);
  return socket_fd;
}

void service_timing_channel(const int socket_fd, const std::stop_token stop) {
  bool logged_request = false;
  std::array<std::uint8_t, 64> request{};
  while (!stop.stop_requested()) {
    pollfd descriptor{socket_fd, POLLIN, 0};
    if (poll(&descriptor, 1, 100) <= 0)
      continue;
    sockaddr_storage peer{};
    socklen_t peer_size = sizeof(peer);
    const auto count =
        recvfrom(socket_fd, request.data(), request.size(), 0,
                 reinterpret_cast<sockaddr *>(&peer), &peer_size);
    if (count < 32 || request[1] != 210)
      continue;
    std::array<std::uint8_t, 32> response{};
    response[0] = 0x80;
    response[1] = 211;
    response[3] = 7;
    std::copy_n(request.begin() + 24, 8, response.begin() + 8);
    write_ntp_timestamp(response.data() + 16, ntp_timestamp());
    write_ntp_timestamp(response.data() + 24, ntp_timestamp());
    if (sendto(socket_fd, response.data(), response.size(), 0,
               reinterpret_cast<const sockaddr *>(&peer), peer_size) >= 0 &&
        !logged_request) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
          << "AirPlay: timing sync request answered\n";
      logged_request = true;
    }
  }
  close(socket_fd);
}

void log_event_plist(const std::span<const std::uint8_t> body) {
  const auto plist = aa2acp::airplay::decode_bplist(
      aa2acp::airplay::Bytes(body.begin(), body.end()));
  if (!plist)
    return;
  const auto *dictionary =
      std::get_if<aa2acp::airplay::PlistValue::Dictionary>(&plist->data);
  if (dictionary == nullptr)
    return;
  std::ostringstream summary;
  summary << "AirPlay: event plist keys:";
  for (const auto &[key, value] : *dictionary) {
    summary << ' ' << key;
    if (const auto *text = std::get_if<std::string>(&value.data))
      summary << '=' << '"' << *text << '"';
    else if (const auto *bytes =
                 std::get_if<aa2acp::airplay::Bytes>(&value.data))
      summary << "[" << bytes->size() << " bytes]";
  }
  if (const auto uuid = dictionary->find("uuid"); uuid != dictionary->end())
    if (const auto *text = std::get_if<std::string>(&uuid->second.data))
      summary << " uuid=" << *text;
  if (const auto report = dictionary->find("hidReport");
      report != dictionary->end()) {
    if (const auto *bytes =
            std::get_if<aa2acp::airplay::Bytes>(&report->second.data)) {
      summary << " hidReport=";
      constexpr std::size_t kMaximumReportBytes = 64;
      for (const auto byte : std::span(*bytes).first(
               std::min(bytes->size(), kMaximumReportBytes)))
        summary << ' ' << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(byte);
      if (bytes->size() > kMaximumReportBytes)
        summary << " ...";
    }
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug) << summary.str() << '\n';
}

void service_event_channel(
    const int socket_fd, aa2acp::airplay::Bytes read_key,
    aa2acp::airplay::Bytes write_key,
    const std::function<void(std::span<const std::uint8_t>)> &event_received,
    const std::stop_token stop) {
  aa2acp::airplay::ControlCipher cipher(std::move(read_key),
                                        std::move(write_key));
  aa2acp::airplay::Bytes encrypted_buffer;
  aa2acp::airplay::Bytes plaintext;
  std::array<std::uint8_t, 4096> buffer{};
  while (!stop.stop_requested()) {
    pollfd descriptor{socket_fd, POLLIN, 0};
    const auto ready = poll(&descriptor, 1, 100);
    if (ready <= 0)
      continue;
    const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
    if (count <= 0)
      break;
    encrypted_buffer.insert(encrypted_buffer.end(), buffer.begin(),
                            buffer.begin() + count);
    while (true) {
      const auto frame = cipher.decrypt_one(encrypted_buffer);
      if (!frame) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
            << "AirPlay: event channel received undecryptable data\n";
        close(socket_fd);
        return;
      }
      if (frame->empty())
        break;
      if (aa2acp::bridge::debug_logging_enabled()) {
        std::ostringstream dump;
        dump << "AirPlay: event channel decrypted " << frame->size()
             << " byte(s):";
        constexpr std::size_t kMaximumEventDumpBytes = 64;
        for (const auto byte : std::span(*frame).first(
                 std::min(frame->size(), kMaximumEventDumpBytes)))
          dump << ' ' << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned int>(byte);
        if (frame->size() > kMaximumEventDumpBytes)
          dump << " ...";
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
            << dump.str() << '\n';
      }
      plaintext.insert(plaintext.end(), frame->begin(), frame->end());
      const std::string request(plaintext.begin(), plaintext.end());
      const auto header_end = request.find("\r\n\r\n");
      if (header_end == std::string::npos)
        continue;
      std::istringstream lines(request.substr(0, header_end));
      std::string line;
      std::string cseq;
      std::size_t body_size{};
      while (std::getline(lines, line)) {
        if (line.ends_with('\r'))
          line.pop_back();
        if (line.starts_with("CSeq:"))
          cseq = line.substr(5);
        if (line.starts_with("Content-Length:")) {
          auto value = line.substr(std::string("Content-Length:").size());
          value.erase(0, value.find_first_not_of(' '));
          const auto [end, error] = std::from_chars(
              value.data(), value.data() + value.size(), body_size);
          if (error != std::errc{} || end != value.data() + value.size()) {
            aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
                << "AirPlay: event channel request has invalid "
                   "Content-Length\n";
            close(socket_fd);
            return;
          }
        }
      }
      const auto request_size = header_end + 4 + body_size;
      if (plaintext.size() < request_size)
        continue;
      if (aa2acp::bridge::debug_logging_enabled())
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
            << "AirPlay: event channel request "
            << request.substr(0, header_end) << " (body=" << body_size
            << " byte(s))\n";
      if (aa2acp::bridge::debug_logging_enabled() && body_size != 0)
        log_event_plist(
            std::span(plaintext).subspan(header_end + 4, body_size));
      if (event_received)
        event_received(std::span(plaintext).first(request_size));
      std::string response =
          "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nAudio-Latency: 0\r\n";
      if (!cseq.empty())
        response += "CSeq:" + cseq + "\r\n";
      response += "\r\n";
      const auto encrypted = cipher.encrypt(
          aa2acp::airplay::Bytes(response.begin(), response.end()));
      if (!encrypted || !send_all(socket_fd, *encrypted)) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
            << "AirPlay: event channel response failed\n";
        close(socket_fd);
        return;
      }
      plaintext.erase(plaintext.begin(),
                      plaintext.begin() +
                          static_cast<std::ptrdiff_t>(request_size));
    }
  }
  close(socket_fd);
}

const aa2acp::airplay::PlistValue::Dictionary *
dictionary_of(const std::optional<aa2acp::airplay::PlistValue> &value) {
  return value ? std::get_if<aa2acp::airplay::PlistValue::Dictionary>(
                     &value->data)
               : nullptr;
}

std::string plist_dictionary_summary(
    const aa2acp::airplay::PlistValue::Dictionary &dictionary) {
  std::ostringstream summary;
  for (const auto &[key, value] : dictionary) {
    summary << ' ' << key << '=';
    if (const auto *number = std::get_if<std::uint64_t>(&value.data))
      summary << *number;
    else if (const auto *text = std::get_if<std::string>(&value.data))
      summary << '"' << *text << '"';
    else if (const auto *array =
                 std::get_if<aa2acp::airplay::PlistValue::Array>(&value.data))
      summary << "array(" << array->size() << ')';
    else if (const auto *bytes =
                 std::get_if<aa2acp::airplay::Bytes>(&value.data))
      summary << "bytes(" << bytes->size() << ')';
    else if (std::holds_alternative<aa2acp::airplay::PlistValue::Dictionary>(
                 value.data))
      summary << "dictionary";
    else
      summary << "scalar";
  }
  return summary.str();
}

std::optional<std::uint64_t>
integer_at(const aa2acp::airplay::PlistValue::Dictionary &dictionary,
           const std::string_view key) {
  const auto item = dictionary.find(std::string(key));
  if (item == dictionary.end())
    return std::nullopt;
  const auto *value = std::get_if<std::uint64_t>(&item->second.data);
  return value ? std::optional<std::uint64_t>(*value) : std::nullopt;
}

std::vector<aa2acp::airplay::Bytes> h264_nalus(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};
  const std::vector<std::uint8_t> input((std::istreambuf_iterator<char>(file)),
                                        std::istreambuf_iterator<char>());
  std::vector<aa2acp::airplay::Bytes> result;
  for (std::size_t offset = 0; offset + 3 <= input.size();) {
    std::size_t start_code = 0;
    if (offset + 4 <= input.size() && input[offset] == 0 &&
        input[offset + 1] == 0 && input[offset + 2] == 0 &&
        input[offset + 3] == 1) {
      start_code = 4;
    } else if (input[offset] == 0 && input[offset + 1] == 0 &&
               input[offset + 2] == 1) {
      start_code = 3;
    }
    if (start_code == 0) {
      ++offset;
      continue;
    }
    const auto start = offset + start_code;
    auto end = start;
    while (end + 3 <= input.size()) {
      if ((end + 4 <= input.size() && input[end] == 0 && input[end + 1] == 0 &&
           input[end + 2] == 0 && input[end + 3] == 1) ||
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

std::vector<aa2acp::airplay::Bytes>
h264_nalus(const std::span<const std::uint8_t> input) {
  std::vector<aa2acp::airplay::Bytes> result;
  for (std::size_t offset = 0; offset + 3 <= input.size();) {
    std::size_t start_code = 0;
    if (offset + 4 <= input.size() && input[offset] == 0 &&
        input[offset + 1] == 0 && input[offset + 2] == 0 &&
        input[offset + 3] == 1) {
      start_code = 4;
    } else if (input[offset] == 0 && input[offset + 1] == 0 &&
               input[offset + 2] == 1) {
      start_code = 3;
    }
    if (start_code == 0) {
      ++offset;
      continue;
    }
    const auto start = offset + start_code;
    auto end = start;
    while (end + 3 <= input.size()) {
      if ((end + 4 <= input.size() && input[end] == 0 && input[end + 1] == 0 &&
           input[end + 2] == 0 && input[end + 3] == 1) ||
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

std::optional<aa2acp::airplay::Bytes>
avcc_config(const std::vector<aa2acp::airplay::Bytes> &nalus) {
  const auto sps =
      std::find_if(nalus.begin(), nalus.end(), [](const auto &nalu) {
        return !nalu.empty() && (nalu[0] & 0x1f) == 7;
      });
  const auto pps =
      std::find_if(nalus.begin(), nalus.end(), [](const auto &nalu) {
        return !nalu.empty() && (nalu[0] & 0x1f) == 8;
      });
  if (sps == nalus.end() || pps == nalus.end() || sps->size() < 4 ||
      sps->size() > UINT16_MAX || pps->size() > UINT16_MAX)
    return std::nullopt;
  aa2acp::airplay::Bytes config{1,
                                (*sps)[1],
                                (*sps)[2],
                                (*sps)[3],
                                0xff,
                                0xe1,
                                static_cast<std::uint8_t>(sps->size() >> 8),
                                static_cast<std::uint8_t>(sps->size())};
  config.insert(config.end(), sps->begin(), sps->end());
  config.push_back(1);
  config.push_back(static_cast<std::uint8_t>(pps->size() >> 8));
  config.push_back(static_cast<std::uint8_t>(pps->size()));
  config.insert(config.end(), pps->begin(), pps->end());
  return config;
}

void store_le32(std::span<std::uint8_t> target, const std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index)
    target[index] = static_cast<std::uint8_t>(value >> (index * 8));
}

void store_le64(std::span<std::uint8_t> target, const std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index)
    target[index] = static_cast<std::uint8_t>(value >> (index * 8));
}

} // namespace

int aa2acp::airplay::run_session(const SessionOptions &options) {
  const std::string &host = options.host;
  const auto port = std::to_string(options.port);
  const std::string &video_path = options.video_path;
  const std::string &pairing_store = options.pairing_store;
  const int timeout_seconds = options.timeout_seconds;
  int socket_fd = connect_tcp(host, port);
  if (socket_fd < 0) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to connect to AirPlay " << host << ':' << port << '\n';
    return 1;
  }
  struct SocketGuard {
    int &fd;
    ~SocketGuard() {
      if (fd >= 0)
        ::close(fd);
    }
  } socket_guard{socket_fd};
  const auto close = [&socket_fd](const int fd) {
    const auto result = ::close(fd);
    if (fd == socket_fd)
      socket_fd = -1;
    return result;
  };
  aa2acp::airplay::PairingRecord pairing;
  std::vector<std::uint8_t> response_bytes;
  std::array<std::uint8_t, 4096> buffer{};
  if (!pairing_store.empty()) {
    const auto stored = aa2acp::airplay::load_pairing_record(pairing_store);
    if (stored) {
      pairing = *stored;
      if (aa2acp::bridge::debug_logging_enabled())
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
            << "AirPlay: loaded persistent pairing identity\n";
    }
  }
  const auto pair_setup_headers = pairing_headers(0);
  const auto pair_verify_headers = pairing_headers(2);
  if (pairing.controller.private_key.empty()) {
    const auto m1 = aa2acp::airplay::encode_tlv8({{0x06, {1}}, {0x00, {0}}});
    const auto request = aa2acp::airplay::encode_request(
        "POST", "/pair-setup", 1, m1, "application/pairing+tlv8",
        pair_setup_headers);
    if (!send_all(socket_fd, request)) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Unable to send Pair-Setup M1\n";
      close(socket_fd);
      return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline &&
           (!options.stop_requested || !options.stop_requested()) &&
           !aa2acp::airplay::complete_response_size(response_bytes)) {
      pollfd descriptor{socket_fd, POLLIN, 0};
      if (poll(&descriptor, 1, 100) <= 0)
        continue;
      const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
      if (count <= 0)
        break;
      response_bytes.insert(response_bytes.end(), buffer.begin(),
                            buffer.begin() + count);
      constexpr std::size_t kMaxResponseBytes = 1024 * 1024;
      if (response_bytes.size() > kMaxResponseBytes) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
            << "AirPlay: response exceeded 1 MiB without completing\n";
        close(socket_fd);
        return 1;
      }
    }
    const auto response = aa2acp::airplay::parse_response(response_bytes);
    if (!response || response->status != 200) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Pair-Setup M1 did not receive RTSP 200\n";
      return 1;
    }
    const auto fields = aa2acp::airplay::decode_tlv8(response->body);
    const auto state = fields.find(0x06);
    const auto salt = fields.find(0x02);
    const auto public_key = fields.find(0x03);
    if (state == fields.end() || state->second != aa2acp::airplay::Bytes{2} ||
        salt == fields.end() || public_key == fields.end()) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Pair-Setup M2 is missing state=2, salt, or SRP public key\n";
      return 1;
    }
    if (aa2acp::bridge::debug_logging_enabled())
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
          << "AirPlay: Pair-Setup M2 received (salt=" << salt->second.size()
          << "B, SRP public key=" << public_key->second.size() << "B)\n";
    aa2acp::airplay::SrpClient srp;
    if (!srp.process_challenge(salt->second, public_key->second)) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Unable to process Pair-Setup SRP challenge\n";
      close(socket_fd);
      return 1;
    }
    const auto m3 = aa2acp::airplay::encode_tlv8(
        {{0x06, {3}}, {0x03, srp.public_key()}, {0x04, srp.client_proof()}});
    const auto m3_request = aa2acp::airplay::encode_request(
        "POST", "/pair-setup", 2, m3, "application/pairing+tlv8",
        pair_setup_headers);
    if (!send_all(socket_fd, m3_request)) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Unable to send Pair-Setup M3\n";
      close(socket_fd);
      return 1;
    }
    response_bytes.clear();
    const auto m4_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < m4_deadline &&
           (!options.stop_requested || !options.stop_requested()) &&
           !aa2acp::airplay::complete_response_size(response_bytes)) {
      pollfd descriptor{socket_fd, POLLIN, 0};
      if (poll(&descriptor, 1, 100) <= 0)
        continue;
      const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
      if (count <= 0)
        break;
      response_bytes.insert(response_bytes.end(), buffer.begin(),
                            buffer.begin() + count);
      constexpr std::size_t kMaxResponseBytes = 1024 * 1024;
      if (response_bytes.size() > kMaxResponseBytes) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
            << "AirPlay: response exceeded 1 MiB without completing\n";
        close(socket_fd);
        return 1;
      }
    }
    const auto m4_response = aa2acp::airplay::parse_response(response_bytes);
    if (!m4_response || m4_response->status != 200) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Pair-Setup M3 did not receive RTSP 200\n";
      return 1;
    }
    const auto m4_fields = aa2acp::airplay::decode_tlv8(m4_response->body);
    const auto m4_state = m4_fields.find(0x06);
    const auto server_proof = m4_fields.find(0x04);
    if (m4_state == m4_fields.end() ||
        m4_state->second != aa2acp::airplay::Bytes{4} ||
        server_proof == m4_fields.end() ||
        !srp.verify_server(server_proof->second)) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Pair-Setup M4 server proof validation failed\n";
      return 1;
    }
    if (aa2acp::bridge::debug_logging_enabled())
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
          << "AirPlay: Pair-Setup M4 server proof validated\n";

    const auto encryption_key = aa2acp::airplay::hkdf_sha512(
        srp.session_key(), "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info",
        32);
    const auto controller = aa2acp::airplay::ed25519_generate();
    const auto controller_id = random_controller_id();
    if (encryption_key.size() != 32 || !controller || !controller_id) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Unable to create Pair-Setup controller identity\n";
      close(socket_fd);
      return 1;
    }
    const auto controller_sign_key = aa2acp::airplay::hkdf_sha512(
        srp.session_key(), "Pair-Setup-Controller-Sign-Salt",
        "Pair-Setup-Controller-Sign-Info", 32);
    aa2acp::airplay::Bytes controller_signed(controller_sign_key);
    controller_signed.insert(controller_signed.end(), controller_id->begin(),
                             controller_id->end());
    controller_signed.insert(controller_signed.end(),
                             controller->public_key.begin(),
                             controller->public_key.end());
    const auto controller_signature = aa2acp::airplay::ed25519_sign(
        controller->private_key, controller_signed);
    if (controller_sign_key.size() != 32 || !controller_signature) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Unable to sign Pair-Setup controller identity\n";
      close(socket_fd);
      return 1;
    }
    const auto inner = aa2acp::airplay::encode_tlv8({
        {0x01, {controller_id->begin(), controller_id->end()}},
        {0x03, controller->public_key},
        {0x0a, *controller_signature},
    });
    const auto encrypted =
        aa2acp::airplay::seal(encryption_key, "PS-Msg05", inner);
    if (!encrypted) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Unable to encrypt Pair-Setup M5\n";
      close(socket_fd);
      return 1;
    }
    const auto m5 =
        aa2acp::airplay::encode_tlv8({{0x06, {5}}, {0x05, *encrypted}});
    const auto m5_request = aa2acp::airplay::encode_request(
        "POST", "/pair-setup", 3, m5, "application/pairing+tlv8",
        pair_setup_headers);
    if (!send_all(socket_fd, m5_request)) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Unable to send Pair-Setup M5\n";
      close(socket_fd);
      return 1;
    }
    response_bytes.clear();
    const auto m6_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < m6_deadline &&
           (!options.stop_requested || !options.stop_requested()) &&
           !aa2acp::airplay::complete_response_size(response_bytes)) {
      pollfd descriptor{socket_fd, POLLIN, 0};
      if (poll(&descriptor, 1, 100) <= 0)
        continue;
      const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
      if (count <= 0)
        break;
      response_bytes.insert(response_bytes.end(), buffer.begin(),
                            buffer.begin() + count);
      constexpr std::size_t kMaxResponseBytes = 1024 * 1024;
      if (response_bytes.size() > kMaxResponseBytes) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
            << "AirPlay: response exceeded 1 MiB without completing\n";
        close(socket_fd);
        return 1;
      }
    }
    const auto m6_response = aa2acp::airplay::parse_response(response_bytes);
    if (!m6_response || m6_response->status != 200) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Pair-Setup M5 did not receive RTSP 200\n";
      return 1;
    }
    const auto m6_fields = aa2acp::airplay::decode_tlv8(m6_response->body);
    const auto m6_state = m6_fields.find(0x06);
    const auto m6_encrypted = m6_fields.find(0x05);
    const auto decrypted =
        m6_encrypted == m6_fields.end()
            ? std::nullopt
            : aa2acp::airplay::open(encryption_key, "PS-Msg06",
                                    m6_encrypted->second);
    if (m6_state == m6_fields.end() ||
        m6_state->second != aa2acp::airplay::Bytes{6} || !decrypted) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Pair-Setup M6 validation failed\n";
      return 1;
    }
    const auto accessory = aa2acp::airplay::decode_tlv8(*decrypted);
    const auto accessory_id = accessory.find(0x01);
    const auto accessory_key = accessory.find(0x03);
    const auto accessory_signature = accessory.find(0x0a);
    const auto accessory_sign_key = aa2acp::airplay::hkdf_sha512(
        srp.session_key(), "Pair-Setup-Accessory-Sign-Salt",
        "Pair-Setup-Accessory-Sign-Info", 32);
    if (accessory_id == accessory.end() || accessory_key == accessory.end() ||
        accessory_signature == accessory.end() ||
        accessory_sign_key.size() != 32) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Pair-Setup M6 identity is incomplete\n";
      return 1;
    }
    aa2acp::airplay::Bytes accessory_signed(accessory_sign_key);
    accessory_signed.insert(accessory_signed.end(),
                            accessory_id->second.begin(),
                            accessory_id->second.end());
    accessory_signed.insert(accessory_signed.end(),
                            accessory_key->second.begin(),
                            accessory_key->second.end());
    if (!aa2acp::airplay::ed25519_verify(accessory_key->second,
                                         accessory_signed,
                                         accessory_signature->second)) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Pair-Setup M6 accessory signature validation failed\n";
      close(socket_fd);
      return 1;
    }
    if (aa2acp::bridge::debug_logging_enabled())
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
          << "AirPlay: Pair-Setup M6 accessory identity validated\n";
    pairing = {*controller_id, *controller, accessory_key->second};
    if (!pairing_store.empty() &&
        !aa2acp::airplay::save_pairing_record(pairing_store, pairing)) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Unable to save persistent AirPlay pairing\n";
      close(socket_fd);
      return 1;
    }
  }

  const auto ephemeral = aa2acp::airplay::x25519_generate();
  if (!ephemeral) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to create Pair-Verify ephemeral key\n";
    close(socket_fd);
    return 1;
  }
  const auto verify_m1 = aa2acp::airplay::encode_tlv8(
      {{0x06, {1}}, {0x03, ephemeral->public_key}});
  const auto verify_m1_request = aa2acp::airplay::encode_request(
      "POST", "/pair-verify", 4, verify_m1, "application/pairing+tlv8",
      pair_verify_headers);
  if (!send_all(socket_fd, verify_m1_request)) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to send Pair-Verify M1\n";
    close(socket_fd);
    return 1;
  }
  response_bytes.clear();
  const auto verify_m2_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  while (std::chrono::steady_clock::now() < verify_m2_deadline &&
         (!options.stop_requested || !options.stop_requested()) &&
         !aa2acp::airplay::complete_response_size(response_bytes)) {
    pollfd descriptor{socket_fd, POLLIN, 0};
    if (poll(&descriptor, 1, 100) <= 0)
      continue;
    const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
    if (count <= 0)
      break;
    constexpr std::size_t kMaxResponseBytes = 1024 * 1024;
    if (response_bytes.size() > kMaxResponseBytes) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "AirPlay: pair-verify response exceeded 1 MiB\n";
      close(socket_fd);
      return 1;
    }
    response_bytes.insert(response_bytes.end(), buffer.begin(),
                          buffer.begin() + count);
  }
  const auto verify_m2_response =
      aa2acp::airplay::parse_response(response_bytes);
  if (!verify_m2_response || verify_m2_response->status != 200) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Pair-Verify M1 did not receive RTSP 200\n";
    close(socket_fd);
    return 1;
  }
  const auto verify_m2 = aa2acp::airplay::decode_tlv8(verify_m2_response->body);
  const auto verify_m2_state = verify_m2.find(0x06);
  const auto peer_ephemeral = verify_m2.find(0x03);
  const auto verify_m2_encrypted = verify_m2.find(0x05);
  const auto shared = peer_ephemeral == verify_m2.end()
                          ? std::nullopt
                          : aa2acp::airplay::x25519_shared(
                                ephemeral->private_key, peer_ephemeral->second);
  const auto verify_key =
      shared ? aa2acp::airplay::hkdf_sha512(*shared, "Pair-Verify-Encrypt-Salt",
                                            "Pair-Verify-Encrypt-Info", 32)
             : aa2acp::airplay::Bytes{};
  const auto verify_m2_plain =
      verify_m2_encrypted == verify_m2.end()
          ? std::nullopt
          : aa2acp::airplay::open(verify_key, "PV-Msg02",
                                  verify_m2_encrypted->second);
  if (verify_m2_state == verify_m2.end() ||
      verify_m2_state->second != aa2acp::airplay::Bytes{2} || !shared ||
      verify_key.size() != 32 || !verify_m2_plain) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Pair-Verify M2 is incomplete or could not be decrypted\n";
    close(socket_fd);
    return 1;
  }
  const auto verify_identity = aa2acp::airplay::decode_tlv8(*verify_m2_plain);
  const auto verify_accessory_id = verify_identity.find(0x01);
  const auto verify_accessory_signature = verify_identity.find(0x0a);
  if (verify_accessory_id == verify_identity.end() ||
      verify_accessory_signature == verify_identity.end()) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Pair-Verify M2 identity is incomplete\n";
    close(socket_fd);
    return 1;
  }
  aa2acp::airplay::Bytes verify_accessory_signed(peer_ephemeral->second);
  verify_accessory_signed.insert(verify_accessory_signed.end(),
                                 verify_accessory_id->second.begin(),
                                 verify_accessory_id->second.end());
  verify_accessory_signed.insert(verify_accessory_signed.end(),
                                 ephemeral->public_key.begin(),
                                 ephemeral->public_key.end());
  if (!aa2acp::airplay::ed25519_verify(pairing.accessory_public_key,
                                       verify_accessory_signed,
                                       verify_accessory_signature->second)) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Pair-Verify M2 accessory signature validation failed\n";
    close(socket_fd);
    return 1;
  }
  aa2acp::airplay::Bytes verify_controller_signed(ephemeral->public_key);
  verify_controller_signed.insert(verify_controller_signed.end(),
                                  pairing.controller_id.begin(),
                                  pairing.controller_id.end());
  verify_controller_signed.insert(verify_controller_signed.end(),
                                  peer_ephemeral->second.begin(),
                                  peer_ephemeral->second.end());
  const auto verify_controller_signature = aa2acp::airplay::ed25519_sign(
      pairing.controller.private_key, verify_controller_signed);
  if (!verify_controller_signature) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to sign Pair-Verify M3\n";
    close(socket_fd);
    return 1;
  }
  const auto verify_m3_inner = aa2acp::airplay::encode_tlv8(
      {{0x01, aa2acp::airplay::Bytes(pairing.controller_id.begin(),
                                     pairing.controller_id.end())},
       {0x0a, *verify_controller_signature}});
  const auto verify_m3_encrypted =
      aa2acp::airplay::seal(verify_key, "PV-Msg03", verify_m3_inner);
  if (!verify_m3_encrypted) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to encrypt Pair-Verify M3\n";
    close(socket_fd);
    return 1;
  }
  const auto verify_m3 =
      aa2acp::airplay::encode_tlv8({{0x06, {3}}, {0x05, *verify_m3_encrypted}});
  const auto verify_m3_request = aa2acp::airplay::encode_request(
      "POST", "/pair-verify", 5, verify_m3, "application/pairing+tlv8",
      pair_verify_headers);
  if (!send_all(socket_fd, verify_m3_request)) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to send Pair-Verify M3\n";
    close(socket_fd);
    return 1;
  }
  response_bytes.clear();
  const auto verify_m4_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  while (std::chrono::steady_clock::now() < verify_m4_deadline &&
         (!options.stop_requested || !options.stop_requested()) &&
         !aa2acp::airplay::complete_response_size(response_bytes)) {
    pollfd descriptor{socket_fd, POLLIN, 0};
    if (poll(&descriptor, 1, 100) <= 0)
      continue;
    const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
    if (count <= 0)
      break;
    constexpr std::size_t kMaxResponseBytes = 1024 * 1024;
    if (response_bytes.size() > kMaxResponseBytes) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "AirPlay: pair-verify response exceeded 1 MiB\n";
      close(socket_fd);
      return 1;
    }
    response_bytes.insert(response_bytes.end(), buffer.begin(),
                          buffer.begin() + count);
  }
  const auto verify_m4_response =
      aa2acp::airplay::parse_response(response_bytes);
  if (!verify_m4_response || verify_m4_response->status != 200) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Pair-Verify M3 did not receive RTSP 200\n";
    return 1;
  }
  const auto verify_m4 = aa2acp::airplay::decode_tlv8(verify_m4_response->body);
  const auto verify_m4_state = verify_m4.find(0x06);
  if (verify_m4_state == verify_m4.end() ||
      verify_m4_state->second != aa2acp::airplay::Bytes{4}) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Pair-Verify M4 validation failed\n";
    return 1;
  }
  const auto control_write = aa2acp::airplay::hkdf_sha512(
      *shared, "Control-Salt", "Control-Write-Encryption-Key", 32);
  const auto control_read = aa2acp::airplay::hkdf_sha512(
      *shared, "Control-Salt", "Control-Read-Encryption-Key", 32);
  if (control_write.size() != 32 || control_read.size() != 32) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to derive control channel keys\n";
    return 1;
  }
  if (aa2acp::bridge::debug_logging_enabled())
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
        << "AirPlay: Pair-Verify M4 validated; encrypted control keys "
           "derived\n";

  aa2acp::airplay::ControlCipher control(control_read, control_write);
  aa2acp::airplay::Bytes encrypted_read_buffer;
  const auto info_body =
      aa2acp::airplay::encode_bplist(aa2acp::airplay::PlistValue::Dictionary{
          {"name", aa2acp::airplay::PlistValue("AA2ACP")},
          {"deviceID", aa2acp::airplay::PlistValue(pairing.controller_id)},
          {"manufacturer", aa2acp::airplay::PlistValue("AA2ACP")},
          {"model", aa2acp::airplay::PlistValue("RaspberryPi")},
          {"osVersion", aa2acp::airplay::PlistValue("0.1")},
      });
  const auto info_response = send_encrypted(
      socket_fd, control, encrypted_read_buffer,
      aa2acp::airplay::encode_request("POST", "/info", 6, info_body,
                                      "application/x-apple-binary-plist"),
      6, timeout_seconds, options.stop_requested);
  const auto info_plist =
      info_response ? aa2acp::airplay::decode_bplist(info_response->body)
                    : std::nullopt;
  if (!info_response || info_response->status != 200 ||
      !dictionary_of(info_plist)) {
    if (info_response) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Encrypted /info response status=" << info_response->status
          << ", body=" << info_response->body.size() << "B\n";
    } else {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Encrypted /info had no decryptable RTSP response\n";
    }
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Encrypted /info request failed\n";
    close(socket_fd);
    return 1;
  }
  const auto *info = dictionary_of(info_plist);
  if (aa2acp::bridge::debug_logging_enabled()) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
        << "AirPlay: encrypted /info capabilities received:"
        << plist_dictionary_summary(*info) << '\n';
  }
  const auto carplay_capabilities =
      info ? aa2acp::airplay::head_unit_capabilities(*info,
                                                     options.head_unit_mac)
           : std::nullopt;
  if (!options.head_unit_capabilities_store.empty() &&
      !options.head_unit_mac.empty()) {
    if (!carplay_capabilities) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "AirPlay: /info did not provide valid head-unit capabilities\n";
    } else if (!save_head_unit_capabilities(
                   options.head_unit_capabilities_store,
                   *carplay_capabilities)) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::warning)
          << "AirPlay: unable to cache head-unit capabilities\n";
    } else {
      if (aa2acp::bridge::debug_logging_enabled())
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
            << "AirPlay: cached head-unit capabilities: display "
            << carplay_capabilities->width_pixels << 'x'
            << carplay_capabilities->height_pixels << " at up to "
            << carplay_capabilities->max_fps << " FPS\n";
    }
  }

  std::uint16_t local_timing_port{};
  const auto timing_socket = bind_timing_socket(local_timing_port);
  if (timing_socket < 0) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "AirPlay: unable to bind timing socket: " << std::strerror(errno)
        << '\n';
    close(socket_fd);
    return 1;
  }
  std::jthread timing_channel([timing_socket](const std::stop_token stop) {
    service_timing_channel(timing_socket, stop);
  });
  const auto session_body =
      aa2acp::airplay::encode_bplist(aa2acp::airplay::PlistValue::Dictionary{
          {"timingPort", aa2acp::airplay::PlistValue(
                             static_cast<std::uint64_t>(local_timing_port))},
          {"name", aa2acp::airplay::PlistValue("AA2ACP")},
          {"deviceID", aa2acp::airplay::PlistValue(pairing.controller_id)},
          {"model", aa2acp::airplay::PlistValue("RaspberryPi")},
      });
  const auto session_response =
      send_encrypted(socket_fd, control, encrypted_read_buffer,
                     aa2acp::airplay::encode_request(
                         "SETUP", "rtsp://127.0.0.1/stream", 7, session_body,
                         "application/x-apple-binary-plist"),
                     7, timeout_seconds, options.stop_requested);
  const auto session_plist =
      session_response ? aa2acp::airplay::decode_bplist(session_response->body)
                       : std::nullopt;
  const auto session_info = dictionary_of(session_plist);
  if (!session_response || session_response->status != 200 || !session_info ||
      !integer_at(*session_info, "timingPort") ||
      !integer_at(*session_info, "eventPort")) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Encrypted session SETUP failed\n";
    close(socket_fd);
    return 1;
  }
  const auto event_port = integer_at(*session_info, "eventPort");
  if (!event_port || *event_port == 0 || *event_port > UINT16_MAX) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "AirPlay: session SETUP returned an invalid event port\n";
    close(socket_fd);
    return 1;
  }
  const auto event_read_key = aa2acp::airplay::hkdf_sha512(
      *shared, "Events-Salt", "Events-Write-Encryption-Key", 32);
  const auto event_write_key = aa2acp::airplay::hkdf_sha512(
      *shared, "Events-Salt", "Events-Read-Encryption-Key", 32);
  const auto event_socket = connect_tcp(
      host, std::to_string(static_cast<std::uint16_t>(*event_port)));
  if (event_socket < 0 || event_read_key.size() != 32 ||
      event_write_key.size() != 32) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "AirPlay: unable to establish encrypted event channel\n";
    if (event_socket >= 0)
      close(event_socket);
    close(socket_fd);
    return 1;
  }
  std::jthread event_channel(
      [event_socket, event_read_key, event_write_key,
       event_received = options.event_received](const std::stop_token stop) {
        service_event_channel(event_socket, event_read_key, event_write_key,
                              event_received, stop);
      });
  std::uint32_t next_cseq = 8;
  if (aa2acp::bridge::debug_logging_enabled())
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
        << "AirPlay: session SETUP connected encrypted event channel on port "
        << *event_port << '\n';

  constexpr std::string_view main_audio_stream_id =
      "8B4C4DF6-AE7F-48F5-A36B-546EEAAEF4B5";
  std::optional<std::uint64_t> audio_port;
  aa2acp::airplay::Bytes audio_key;
  if (options.next_media_audio && carplay_capabilities &&
      carplay_capabilities->media_pcm_48k_stereo) {
    // Android Auto delivers media as 48 kHz stereo S16LE. Preserve this direct
    // LPCM stream for the first audio milestone instead of adding an encoder
    // and its latency to the bridge.
    const auto audio_body =
        aa2acp::airplay::encode_bplist(aa2acp::airplay::PlistValue::Dictionary{
            {"streams",
             aa2acp::airplay::PlistValue::Array{
                 aa2acp::airplay::PlistValue::Dictionary{
                     {"type", aa2acp::airplay::PlistValue(std::uint64_t{100})},
                     {"audioType", aa2acp::airplay::PlistValue("media")},
                     {"audioFormat",
                      aa2acp::airplay::PlistValue(std::uint64_t{0x8000})},
                     {"streamConnectionID",
                      aa2acp::airplay::PlistValue(
                          main_audio_stream_id.data())}}}},
        });
    const auto audio_cseq = next_cseq++;
    const auto audio_response =
        send_encrypted(socket_fd, control, encrypted_read_buffer,
                       aa2acp::airplay::encode_request(
                           "SETUP", "rtsp://127.0.0.1/stream", audio_cseq,
                           audio_body, "application/x-apple-binary-plist"),
                       audio_cseq, timeout_seconds, options.stop_requested);
    const auto audio_plist =
        audio_response ? aa2acp::airplay::decode_bplist(audio_response->body)
                       : std::nullopt;
    const auto audio_info = dictionary_of(audio_plist);
    const auto *audio_streams =
        audio_info && audio_info->contains("streams")
            ? std::get_if<aa2acp::airplay::PlistValue::Array>(
                  &audio_info->at("streams").data)
            : nullptr;
    const auto *first_audio_stream =
        audio_streams && !audio_streams->empty()
            ? std::get_if<aa2acp::airplay::PlistValue::Dictionary>(
                  &audio_streams->front().data)
            : nullptr;
    audio_port = first_audio_stream
                     ? integer_at(*first_audio_stream, "dataPort")
                     : std::nullopt;
    audio_key = aa2acp::airplay::hkdf_sha512(
        *shared,
        std::string("DataStream-Salt") + std::string(main_audio_stream_id),
        "DataStream-Output-Encryption-Key", 32);
    if (!audio_response || audio_response->status != 200 || !audio_port ||
        *audio_port == 0 || *audio_port > UINT16_MAX ||
        audio_key.size() != 32) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Encrypted media-audio SETUP failed\n";
      close(socket_fd);
      return 1;
    }
    if (aa2acp::bridge::debug_logging_enabled())
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
          << "AirPlay: media-audio SETUP received data port " << *audio_port
          << '\n';
  }

  struct AuxiliaryAudioStream {
    std::uint64_t port{};
    aa2acp::airplay::Bytes key;
  };
  const auto setup_auxiliary_audio =
      [&](const std::function<std::optional<std::vector<std::uint8_t>>()> &next,
          const bool supported, const std::string_view name,
          const std::string_view audio_type, const std::string_view stream_id)
      -> std::optional<AuxiliaryAudioStream> {
    if (!next || !supported)
      return std::nullopt;
    const auto body =
        aa2acp::airplay::encode_bplist(aa2acp::airplay::PlistValue::Dictionary{
            {"streams",
             aa2acp::airplay::PlistValue::Array{
                 aa2acp::airplay::PlistValue::Dictionary{
                     {"type", aa2acp::airplay::PlistValue(std::uint64_t{100})},
                     {"audioType",
                      aa2acp::airplay::PlistValue(std::string(audio_type))},
                     {"audioFormat",
                      aa2acp::airplay::PlistValue(std::uint64_t{0x10})},
                     {"streamConnectionID",
                      aa2acp::airplay::PlistValue(std::string(stream_id))}}}}});
    const auto cseq = next_cseq++;
    const auto response =
        send_encrypted(socket_fd, control, encrypted_read_buffer,
                       aa2acp::airplay::encode_request(
                           "SETUP", "rtsp://127.0.0.1/stream", cseq, body,
                           "application/x-apple-binary-plist"),
                       cseq, timeout_seconds, options.stop_requested);
    const auto plist = response ? aa2acp::airplay::decode_bplist(response->body)
                                : std::nullopt;
    const auto info = dictionary_of(plist);
    const auto *streams = info && info->contains("streams")
                              ? std::get_if<aa2acp::airplay::PlistValue::Array>(
                                    &info->at("streams").data)
                              : nullptr;
    const auto *stream =
        streams && !streams->empty()
            ? std::get_if<aa2acp::airplay::PlistValue::Dictionary>(
                  &streams->front().data)
            : nullptr;
    const auto port = stream ? integer_at(*stream, "dataPort") : std::nullopt;
    const auto key = aa2acp::airplay::hkdf_sha512(
        *shared, std::string("DataStream-Salt") + std::string(stream_id),
        "DataStream-Output-Encryption-Key", 32);
    const auto port_value = port.value_or(0);
    if (!response || response->status != 200 || port_value == 0 ||
        port_value > UINT16_MAX || key.size() != 32) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Encrypted " << name << "-audio SETUP failed\n";
      return std::nullopt;
    }
    if (aa2acp::bridge::debug_logging_enabled())
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
          << "AirPlay: " << name << "-audio SETUP received data port " << *port
          << '\n';
    return AuxiliaryAudioStream{*port, key};
  };
  const auto guidance_audio = setup_auxiliary_audio(
      options.next_guidance_audio,
      carplay_capabilities && carplay_capabilities->guidance_pcm_16k_mono,
      "guidance", "default", "9B4C4DF6-AE7F-48F5-A36B-546EEAAEF4B5");
  const auto system_audio = setup_auxiliary_audio(
      options.next_system_audio,
      carplay_capabilities && carplay_capabilities->system_pcm_16k_mono,
      "system", "alert", "AB4C4DF6-AE7F-48F5-A36B-546EEAAEF4B5");

  const auto screen_stream_id = random_stream_connection_id();
  if (!screen_stream_id) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to generate a screen stream connection ID\n";
    close(socket_fd);
    return 1;
  }
  const auto screen_body =
      aa2acp::airplay::encode_bplist(aa2acp::airplay::PlistValue::Dictionary{
          {"streams",
           aa2acp::airplay::PlistValue::Array{
               aa2acp::airplay::PlistValue::Dictionary{
                   {"type", aa2acp::airplay::PlistValue(std::uint64_t{110})},
                   {"streamConnectionID",
                    aa2acp::airplay::PlistValue(*screen_stream_id)}}}},
      });
  if (aa2acp::bridge::debug_logging_enabled()) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
        << "AirPlay: screen SETUP request: type=110 streamConnectionID="
        << *screen_stream_id << '\n';
  }
  const auto screen_cseq = next_cseq++;
  const auto screen_response = send_encrypted(
      socket_fd, control, encrypted_read_buffer,
      aa2acp::airplay::encode_request("SETUP", "rtsp://127.0.0.1/stream",
                                      screen_cseq, screen_body,
                                      "application/x-apple-binary-plist"),
      screen_cseq, timeout_seconds, options.stop_requested, "screen SETUP");
  const auto screen_plist =
      screen_response ? aa2acp::airplay::decode_bplist(screen_response->body)
                      : std::nullopt;
  const auto screen_info = dictionary_of(screen_plist);
  const auto *stream_array =
      screen_info && screen_info->contains("streams")
          ? std::get_if<aa2acp::airplay::PlistValue::Array>(
                &screen_info->at("streams").data)
          : nullptr;
  const auto *first_stream =
      stream_array && !stream_array->empty()
          ? std::get_if<aa2acp::airplay::PlistValue::Dictionary>(
                &stream_array->front().data)
          : nullptr;
  const auto screen_port =
      first_stream ? integer_at(*first_stream, "dataPort") : std::nullopt;
  const auto screen_port_value = screen_port.value_or(0);
  if (!screen_response || screen_response->status != 200 ||
      screen_port_value == 0 || screen_port_value > UINT16_MAX) {
    if (screen_response) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Encrypted screen SETUP response status="
          << screen_response->status
          << ", body=" << screen_response->body.size() << "B\n";
      if (screen_info) {
        std::ostringstream details;
        details << "Screen SETUP plist keys:";
        for (const auto &[key, value] : *screen_info) {
          details << ' ' << key;
          if (const auto *number = std::get_if<std::uint64_t>(&value.data))
            details << '=' << *number;
        }
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
            << details.str() << '\n';
      } else {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
            << "Screen SETUP plist could not be decoded\n";
      }
    } else {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Encrypted screen SETUP had no decryptable RTSP response\n";
    }
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Encrypted screen SETUP failed\n";
    close(socket_fd);
    return 1;
  }
  if (aa2acp::bridge::debug_logging_enabled())
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
        << "AirPlay: screen SETUP received data port " << screen_port_value
        << '\n';

  const auto record_cseq = next_cseq++;
  const auto record_response =
      send_encrypted(socket_fd, control, encrypted_read_buffer,
                     aa2acp::airplay::encode_request(
                         "RECORD", "rtsp://127.0.0.1/stream", record_cseq, {},
                         "application/octet-stream"),
                     record_cseq, timeout_seconds, options.stop_requested);
  if (!record_response || record_response->status != 200) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Encrypted RECORD failed\n";
    close(socket_fd);
    return 1;
  }
  if (aa2acp::bridge::debug_logging_enabled())
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
        << "AirPlay: encrypted RECORD accepted\n";
  std::jthread audio_sender;
  if (options.next_media_audio && audio_port) {
    audio_sender = std::jthread([&] {
      const auto media_socket = connect_udp(
          host, std::to_string(static_cast<std::uint16_t>(*audio_port)));
      if (media_socket < 0) {
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
            << "Unable to establish encrypted media-audio stream\n";
        return;
      }
      std::uint16_t sequence{};
      std::uint32_t timestamp{};
      std::uint64_t nonce_counter{};
      std::size_t sent_packets{};
      while (!options.stop_requested || !options.stop_requested()) {
        const auto pcm = options.next_media_audio();
        if (!pcm)
          break;
        if (pcm->empty() || pcm->size() % 4 != 0)
          continue;
        aa2acp::airplay::Bytes payload = *pcm;
        for (std::size_t index = 0; index < payload.size(); index += 2)
          std::swap(payload[index], payload[index + 1]);
        std::array<std::uint8_t, 12> header{};
        header[0] = 0x80;
        header[1] = 100;
        header[2] = static_cast<std::uint8_t>(sequence >> 8);
        header[3] = static_cast<std::uint8_t>(sequence);
        header[4] = static_cast<std::uint8_t>(timestamp >> 24);
        header[5] = static_cast<std::uint8_t>(timestamp >> 16);
        header[6] = static_cast<std::uint8_t>(timestamp >> 8);
        header[7] = static_cast<std::uint8_t>(timestamp);
        std::array<std::uint8_t, 8> nonce{};
        store_le64(nonce, nonce_counter);
        std::array<std::uint8_t, 12> nonce12{};
        std::copy(nonce.begin(), nonce.end(), nonce12.begin() + 4);
        const auto encrypted = aa2acp::airplay::seal_with_nonce(
            audio_key, nonce12, payload, std::span(header).subspan(4, 8));
        if (!encrypted) {
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
              << "Unable to encrypt Android Auto media audio\n";
          break;
        }
        aa2acp::airplay::Bytes packet(header.begin(), header.end());
        packet.insert(packet.end(), encrypted->begin(), encrypted->end());
        packet.insert(packet.end(), nonce.begin(), nonce.end());
        if (send(media_socket, packet.data(), packet.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(packet.size())) {
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
              << "Unable to send encrypted Android Auto media audio\n";
          break;
        }
        ++sequence;
        timestamp += static_cast<std::uint32_t>(payload.size() / 4);
        ++nonce_counter;
        ++sent_packets;
        if (sent_packets == 1 && aa2acp::bridge::debug_logging_enabled()) {
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
              << "AirPlay: forwarding Android Auto media audio "
                 "(48 kHz stereo PCM)\n";
        }
      }
      close(media_socket);
      if (aa2acp::bridge::debug_logging_enabled())
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
            << "AirPlay: encrypted Android Auto media audio sent "
            << sent_packets << " packets\n";
    });
  }
  const auto launch_auxiliary_audio =
      [&](const std::optional<AuxiliaryAudioStream> &stream,
          const std::function<std::optional<std::vector<std::uint8_t>>()> &next,
          const std::string_view name) -> std::jthread {
    if (!stream)
      return {};
    return std::jthread([&, stream, next, name] {
      const auto fd = connect_udp(
          host, std::to_string(static_cast<std::uint16_t>(stream->port)));
      if (fd < 0)
        return;
      std::uint16_t sequence{};
      std::uint32_t timestamp{};
      std::uint64_t counter{};
      while (!options.stop_requested || !options.stop_requested()) {
        const auto pcm = next();
        if (!pcm)
          break;
        if (pcm->empty() || pcm->size() % 2 != 0)
          continue;
        auto payload = aa2acp::airplay::Bytes(*pcm);
        for (std::size_t index = 0; index < payload.size(); index += 2)
          std::swap(payload[index], payload[index + 1]);
        std::array<std::uint8_t, 12> header{
            0x80,
            100,
            static_cast<std::uint8_t>(sequence >> 8),
            static_cast<std::uint8_t>(sequence),
            static_cast<std::uint8_t>(timestamp >> 24),
            static_cast<std::uint8_t>(timestamp >> 16),
            static_cast<std::uint8_t>(timestamp >> 8),
            static_cast<std::uint8_t>(timestamp)};
        std::array<std::uint8_t, 8> nonce{};
        store_le64(nonce, counter);
        std::array<std::uint8_t, 12> nonce12{};
        std::copy(nonce.begin(), nonce.end(), nonce12.begin() + 4);
        const auto encrypted = aa2acp::airplay::seal_with_nonce(
            stream->key, nonce12, payload, std::span(header).subspan(4, 8));
        if (!encrypted)
          break;
        aa2acp::airplay::Bytes packet(header.begin(), header.end());
        packet.insert(packet.end(), encrypted->begin(), encrypted->end());
        packet.insert(packet.end(), nonce.begin(), nonce.end());
        if (send(fd, packet.data(), packet.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(packet.size()))
          break;
        ++sequence;
        timestamp += static_cast<std::uint32_t>(payload.size() / 2);
        ++counter;
      }
      close(fd);
      if (aa2acp::bridge::debug_logging_enabled())
        aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
            << "AirPlay: encrypted Android Auto " << name << " audio ended\n";
    });
  };
  auto guidance_sender = launch_auxiliary_audio(
      guidance_audio, options.next_guidance_audio, "guidance");
  auto system_sender =
      launch_auxiliary_audio(system_audio, options.next_system_audio, "system");
  struct StreamStopGuard {
    const std::function<void()> &stop;
    ~StreamStopGuard() {
      if (stop)
        stop();
    }
  } stream_stop_guard{options.stop_streams};
  if (video_path.empty() && !options.next_video_frame) {
    close(socket_fd);
    return 0;
  }

  std::vector<aa2acp::airplay::Bytes> nalus;
  std::vector<aa2acp::airplay::Bytes> initial_access_units;
  if (!video_path.empty())
    nalus = h264_nalus(video_path);

  // A live Android Auto stream starts with an Annex-B codec configuration.
  // Wait for it before opening the AirPlay data channel: CarPlay requires the
  // AVCC configuration to be its first data-stream payload.
  std::optional<aa2acp::airplay::Bytes> config;
  while (!config) {
    config = avcc_config(nalus);
    if (config)
      break;
    if (!options.next_video_frame) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Unable to parse H.264 SPS/PPS from " << video_path << '\n';
      close(socket_fd);
      return 1;
    }
    if (options.stop_requested && options.stop_requested()) {
      close(socket_fd);
      return 0;
    }
    const auto access_unit = options.next_video_frame();
    if (!access_unit) {
      if (options.stop_requested && options.stop_requested()) {
        close(socket_fd);
        return 0;
      }
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Android Auto video ended before H.264 SPS/PPS arrived\n";
      close(socket_fd);
      return 1;
    }
    const auto unit_nalus = h264_nalus(*access_unit);
    nalus.insert(nalus.end(), unit_nalus.begin(), unit_nalus.end());
    initial_access_units.push_back(std::move(*access_unit));
  }
  const auto stream_key = aa2acp::airplay::hkdf_sha512(
      *shared,
      std::string("DataStream-Salt") + std::to_string(*screen_stream_id),
      "DataStream-Output-Encryption-Key", 32);
  if (stream_key.size() != 32) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "Unable to derive encrypted screen data-stream key\n";
    close(socket_fd);
    return 1;
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "AirPlay: connecting screen data stream to " << host << ':'
      << screen_port_value << '\n';
  std::string data_stream_error;
  const auto data_socket =
      connect_tcp_with_timeout(host, std::to_string(screen_port_value),
                               std::chrono::seconds(10), &data_stream_error);
  if (data_socket < 0) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "AirPlay: screen data connection failed: " << data_stream_error
        << '\n';
    close(socket_fd);
    return 1;
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "AirPlay: screen data stream connected\n";
  aa2acp::airplay::Bytes config_header(128);
  store_le32(std::span(config_header).first(4), config->size());
  config_header[4] = 1;
  if (!send_all(data_socket, config_header) ||
      !send_all(data_socket, *config)) {
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
        << "AirPlay: unable to send H.264 video config: "
        << std::strerror(errno) << '\n';
    close(data_socket);
    close(socket_fd);
    return 1;
  }
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
      << "AirPlay: sent H.264 video config (" << config->size() << " bytes)\n";
  std::uint64_t frame_counter{};
  std::size_t sent_frames{};
  std::size_t sent_video_bytes{};
  const auto send_access_unit =
      [&](const std::vector<aa2acp::airplay::Bytes> &access_unit) {
        aa2acp::airplay::Bytes frame;
        for (const auto &nalu : access_unit) {
          if (nalu.empty() || ((nalu[0] & 0x1f) != 1 && (nalu[0] & 0x1f) != 5))
            continue;
          frame.push_back(static_cast<std::uint8_t>(nalu.size() >> 24));
          frame.push_back(static_cast<std::uint8_t>(nalu.size() >> 16));
          frame.push_back(static_cast<std::uint8_t>(nalu.size() >> 8));
          frame.push_back(static_cast<std::uint8_t>(nalu.size()));
          frame.insert(frame.end(), nalu.begin(), nalu.end());
        }
        if (frame.empty())
          return true;
        aa2acp::airplay::Bytes header(128);
        store_le32(std::span(header).first(4), frame.size() + 16);
        header[4] = 0;
        store_le64(std::span(header).subspan(8, 8), frame_counter);
        std::array<std::uint8_t, 12> nonce{};
        store_le64(std::span(nonce).subspan(4, 8), frame_counter);
        const auto encrypted =
            aa2acp::airplay::seal_with_nonce(stream_key, nonce, frame, header);
        if (!encrypted || !send_all(data_socket, header) ||
            !send_all(data_socket, *encrypted))
          return false;
        ++frame_counter;
        ++sent_frames;
        sent_video_bytes += frame.size();
        if (sent_frames == 1)
          aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
              << "AirPlay: sent first encrypted H.264 video frame ("
              << frame.size() << " bytes)\n";
        return true;
      };
  for (const auto &nalu : nalus) {
    if (options.stop_requested && options.stop_requested()) {
      close(data_socket);
      close(socket_fd);
      return 0;
    }
    if (!send_access_unit({nalu})) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Unable to send encrypted H.264 frame\n";
      close(data_socket);
      close(socket_fd);
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  for (const auto &access_unit : initial_access_units) {
    if (!send_access_unit(h264_nalus(access_unit))) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Unable to send encrypted H.264 frame\n";
      close(data_socket);
      close(socket_fd);
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  while (options.next_video_frame &&
         (!options.stop_requested || !options.stop_requested())) {
    const auto access_unit = options.next_video_frame();
    if (!access_unit)
      break;
    if (!send_access_unit(h264_nalus(*access_unit))) {
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
          << "Unable to send encrypted H.264 frame\n";
      close(data_socket);
      close(socket_fd);
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  close(data_socket);
  close(socket_fd);
  if (aa2acp::bridge::debug_logging_enabled())
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
        << "AirPlay: encrypted H.264 stream sent " << sent_frames << " frames ("
        << sent_video_bytes << " bytes)\n";
  return 0;
}
