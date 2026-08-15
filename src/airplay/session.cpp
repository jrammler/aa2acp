#include "acp/airplay/session.hpp"
#include "acp/airplay/bplist.hpp"
#include "acp/airplay/control_cipher.hpp"
#include "acp/airplay/crypto.hpp"
#include "acp/airplay/pairing_store.hpp"
#include "acp/airplay/rtsp.hpp"
#include "acp/airplay/srp.hpp"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

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

std::optional<acp::airplay::Response>
send_encrypted(const int socket_fd, acp::airplay::ControlCipher &cipher,
               acp::airplay::Bytes &encrypted_buffer,
               const std::span<const std::uint8_t> plaintext,
               const int timeout_seconds,
               const std::function<bool()> &stop_requested) {
  const auto encrypted = cipher.encrypt(plaintext);
  if (!encrypted || !send_all(socket_fd, *encrypted))
    return std::nullopt;
  acp::airplay::Bytes response_plaintext;
  std::array<std::uint8_t, 4096> buffer{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  while (std::chrono::steady_clock::now() < deadline &&
         (!stop_requested || !stop_requested())) {
    while (true) {
      const auto frame = cipher.decrypt_one(encrypted_buffer);
      if (!frame)
        return std::nullopt;
      if (frame->empty())
        break;
      response_plaintext.insert(response_plaintext.end(), frame->begin(),
                                frame->end());
      if (acp::airplay::complete_response_size(response_plaintext))
        return acp::airplay::parse_response(response_plaintext);
    }
    pollfd descriptor{socket_fd, POLLIN, 0};
    if (poll(&descriptor, 1, 100) <= 0)
      continue;
    const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
    if (count <= 0)
      return std::nullopt;
    encrypted_buffer.insert(encrypted_buffer.end(), buffer.begin(),
                            buffer.begin() + count);
  }
  return std::nullopt;
}

const acp::airplay::PlistValue::Dictionary *
dictionary_of(const std::optional<acp::airplay::PlistValue> &value) {
  return value ? std::get_if<acp::airplay::PlistValue::Dictionary>(&value->data)
               : nullptr;
}

std::optional<std::uint64_t>
integer_at(const acp::airplay::PlistValue::Dictionary &dictionary,
           const std::string_view key) {
  const auto item = dictionary.find(std::string(key));
  if (item == dictionary.end())
    return std::nullopt;
  const auto *value = std::get_if<std::uint64_t>(&item->second.data);
  return value ? std::optional<std::uint64_t>(*value) : std::nullopt;
}

std::vector<acp::airplay::Bytes> h264_nalus(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};
  const std::vector<std::uint8_t> input((std::istreambuf_iterator<char>(file)),
                                        std::istreambuf_iterator<char>());
  std::vector<acp::airplay::Bytes> result;
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
    if (start < end)
      result.emplace_back(input.begin() + static_cast<std::ptrdiff_t>(start),
                          input.begin() + static_cast<std::ptrdiff_t>(end));
    offset = end;
  }
  return result;
}

std::vector<acp::airplay::Bytes>
h264_nalus(const std::span<const std::uint8_t> input) {
  std::vector<acp::airplay::Bytes> result;
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
    if (start < end)
      result.emplace_back(input.begin() + static_cast<std::ptrdiff_t>(start),
                          input.begin() + static_cast<std::ptrdiff_t>(end));
    offset = end;
  }
  return result;
}

std::optional<acp::airplay::Bytes>
avcc_config(const std::vector<acp::airplay::Bytes> &nalus) {
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
  acp::airplay::Bytes config{1,
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

int acp::airplay::run_session(const SessionOptions &options) {
  const std::string &host = options.host;
  const auto port = std::to_string(options.port);
  const std::string &video_path = options.video_path;
  const std::string &pairing_store = options.pairing_store;
  const int timeout_seconds = options.timeout_seconds;
  const auto socket_fd = connect_tcp(host, port);
  if (socket_fd < 0) {
    std::cerr << "Unable to connect to AirPlay " << host << ':' << port << '\n';
    return 1;
  }
  acp::airplay::PairingRecord pairing;
  std::vector<std::uint8_t> response_bytes;
  std::array<std::uint8_t, 4096> buffer{};
  if (!pairing_store.empty()) {
    const auto stored = acp::airplay::load_pairing_record(pairing_store);
    if (stored) {
      pairing = *stored;
      std::cout << "AirPlay: loaded persistent pairing identity\n";
    }
  }
  if (pairing.controller.private_key.empty()) {
    const auto m1 = acp::airplay::encode_tlv8({{0x06, {1}}, {0x00, {0}}});
    const auto request = acp::airplay::encode_request(
        "POST", "/pair-setup", 1, m1, "application/pairing+tlv8");
    if (!send_all(socket_fd, request)) {
      std::cerr << "Unable to send Pair-Setup M1\n";
      close(socket_fd);
      return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline &&
           (!options.stop_requested || !options.stop_requested()) &&
           !acp::airplay::complete_response_size(response_bytes)) {
      pollfd descriptor{socket_fd, POLLIN, 0};
      if (poll(&descriptor, 1, 100) <= 0)
        continue;
      const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
      if (count <= 0)
        break;
      response_bytes.insert(response_bytes.end(), buffer.begin(),
                            buffer.begin() + count);
    }
    const auto response = acp::airplay::parse_response(response_bytes);
    if (!response || response->status != 200) {
      std::cerr << "Pair-Setup M1 did not receive RTSP 200\n";
      return 1;
    }
    const auto fields = acp::airplay::decode_tlv8(response->body);
    const auto state = fields.find(0x06);
    const auto salt = fields.find(0x02);
    const auto public_key = fields.find(0x03);
    if (state == fields.end() || state->second != acp::airplay::Bytes{2} ||
        salt == fields.end() || public_key == fields.end()) {
      std::cerr
          << "Pair-Setup M2 is missing state=2, salt, or SRP public key\n";
      return 1;
    }
    std::cout << "AirPlay: Pair-Setup M2 received (salt=" << salt->second.size()
              << "B, SRP public key=" << public_key->second.size() << "B)\n";
    acp::airplay::SrpClient srp;
    if (!srp.process_challenge(salt->second, public_key->second)) {
      std::cerr << "Unable to process Pair-Setup SRP challenge\n";
      close(socket_fd);
      return 1;
    }
    const auto m3 = acp::airplay::encode_tlv8(
        {{0x06, {3}}, {0x03, srp.public_key()}, {0x04, srp.client_proof()}});
    const auto m3_request = acp::airplay::encode_request(
        "POST", "/pair-setup", 2, m3, "application/pairing+tlv8");
    if (!send_all(socket_fd, m3_request)) {
      std::cerr << "Unable to send Pair-Setup M3\n";
      close(socket_fd);
      return 1;
    }
    response_bytes.clear();
    const auto m4_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < m4_deadline &&
           (!options.stop_requested || !options.stop_requested()) &&
           !acp::airplay::complete_response_size(response_bytes)) {
      pollfd descriptor{socket_fd, POLLIN, 0};
      if (poll(&descriptor, 1, 100) <= 0)
        continue;
      const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
      if (count <= 0)
        break;
      response_bytes.insert(response_bytes.end(), buffer.begin(),
                            buffer.begin() + count);
    }
    const auto m4_response = acp::airplay::parse_response(response_bytes);
    if (!m4_response || m4_response->status != 200) {
      std::cerr << "Pair-Setup M3 did not receive RTSP 200\n";
      return 1;
    }
    const auto m4_fields = acp::airplay::decode_tlv8(m4_response->body);
    const auto m4_state = m4_fields.find(0x06);
    const auto server_proof = m4_fields.find(0x04);
    if (m4_state == m4_fields.end() ||
        m4_state->second != acp::airplay::Bytes{4} ||
        server_proof == m4_fields.end() ||
        !srp.verify_server(server_proof->second)) {
      std::cerr << "Pair-Setup M4 server proof validation failed\n";
      return 1;
    }
    std::cout << "AirPlay: Pair-Setup M4 server proof validated\n";

    const auto encryption_key =
        acp::airplay::hkdf_sha512(srp.session_key(), "Pair-Setup-Encrypt-Salt",
                                  "Pair-Setup-Encrypt-Info", 32);
    const auto controller = acp::airplay::ed25519_generate();
    if (encryption_key.size() != 32 || !controller) {
      std::cerr << "Unable to create Pair-Setup controller identity\n";
      close(socket_fd);
      return 1;
    }
    constexpr std::string_view controller_id =
        "85A6B4F2-3C8D-4E1A-9F7B-2D5E6C8A0B3C";
    const auto controller_sign_key = acp::airplay::hkdf_sha512(
        srp.session_key(), "Pair-Setup-Controller-Sign-Salt",
        "Pair-Setup-Controller-Sign-Info", 32);
    acp::airplay::Bytes controller_signed(controller_sign_key);
    controller_signed.insert(controller_signed.end(), controller_id.begin(),
                             controller_id.end());
    controller_signed.insert(controller_signed.end(),
                             controller->public_key.begin(),
                             controller->public_key.end());
    const auto controller_signature =
        acp::airplay::ed25519_sign(controller->private_key, controller_signed);
    if (controller_sign_key.size() != 32 || !controller_signature) {
      std::cerr << "Unable to sign Pair-Setup controller identity\n";
      close(socket_fd);
      return 1;
    }
    const auto inner = acp::airplay::encode_tlv8({
        {0x01, {controller_id.begin(), controller_id.end()}},
        {0x03, controller->public_key},
        {0x0a, *controller_signature},
    });
    const auto encrypted =
        acp::airplay::seal(encryption_key, "PS-Msg05", inner);
    if (!encrypted) {
      std::cerr << "Unable to encrypt Pair-Setup M5\n";
      close(socket_fd);
      return 1;
    }
    const auto m5 =
        acp::airplay::encode_tlv8({{0x06, {5}}, {0x05, *encrypted}});
    const auto m5_request = acp::airplay::encode_request(
        "POST", "/pair-setup", 3, m5, "application/pairing+tlv8");
    if (!send_all(socket_fd, m5_request)) {
      std::cerr << "Unable to send Pair-Setup M5\n";
      close(socket_fd);
      return 1;
    }
    response_bytes.clear();
    const auto m6_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < m6_deadline &&
           (!options.stop_requested || !options.stop_requested()) &&
           !acp::airplay::complete_response_size(response_bytes)) {
      pollfd descriptor{socket_fd, POLLIN, 0};
      if (poll(&descriptor, 1, 100) <= 0)
        continue;
      const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
      if (count <= 0)
        break;
      response_bytes.insert(response_bytes.end(), buffer.begin(),
                            buffer.begin() + count);
    }
    const auto m6_response = acp::airplay::parse_response(response_bytes);
    if (!m6_response || m6_response->status != 200) {
      std::cerr << "Pair-Setup M5 did not receive RTSP 200\n";
      return 1;
    }
    const auto m6_fields = acp::airplay::decode_tlv8(m6_response->body);
    const auto m6_state = m6_fields.find(0x06);
    const auto m6_encrypted = m6_fields.find(0x05);
    const auto decrypted = m6_encrypted == m6_fields.end()
                               ? std::nullopt
                               : acp::airplay::open(encryption_key, "PS-Msg06",
                                                    m6_encrypted->second);
    if (m6_state == m6_fields.end() ||
        m6_state->second != acp::airplay::Bytes{6} || !decrypted) {
      std::cerr << "Pair-Setup M6 validation failed\n";
      return 1;
    }
    const auto accessory = acp::airplay::decode_tlv8(*decrypted);
    const auto accessory_id = accessory.find(0x01);
    const auto accessory_key = accessory.find(0x03);
    const auto accessory_signature = accessory.find(0x0a);
    const auto accessory_sign_key = acp::airplay::hkdf_sha512(
        srp.session_key(), "Pair-Setup-Accessory-Sign-Salt",
        "Pair-Setup-Accessory-Sign-Info", 32);
    if (accessory_id == accessory.end() || accessory_key == accessory.end() ||
        accessory_signature == accessory.end() ||
        accessory_sign_key.size() != 32) {
      std::cerr << "Pair-Setup M6 identity is incomplete\n";
      return 1;
    }
    acp::airplay::Bytes accessory_signed(accessory_sign_key);
    accessory_signed.insert(accessory_signed.end(),
                            accessory_id->second.begin(),
                            accessory_id->second.end());
    accessory_signed.insert(accessory_signed.end(),
                            accessory_key->second.begin(),
                            accessory_key->second.end());
    if (!acp::airplay::ed25519_verify(accessory_key->second, accessory_signed,
                                      accessory_signature->second)) {
      std::cerr << "Pair-Setup M6 accessory signature validation failed\n";
      close(socket_fd);
      return 1;
    }
    std::cout << "AirPlay: Pair-Setup M6 accessory identity validated\n";
    pairing = {std::string(controller_id), *controller, accessory_key->second};
    if (!pairing_store.empty() &&
        !acp::airplay::save_pairing_record(pairing_store, pairing)) {
      std::cerr << "Unable to save persistent AirPlay pairing\n";
      close(socket_fd);
      return 1;
    }
  }

  const auto ephemeral = acp::airplay::x25519_generate();
  if (!ephemeral) {
    std::cerr << "Unable to create Pair-Verify ephemeral key\n";
    close(socket_fd);
    return 1;
  }
  const auto verify_m1 =
      acp::airplay::encode_tlv8({{0x06, {1}}, {0x03, ephemeral->public_key}});
  const auto verify_m1_request = acp::airplay::encode_request(
      "POST", "/pair-verify", 4, verify_m1, "application/pairing+tlv8");
  if (!send_all(socket_fd, verify_m1_request)) {
    std::cerr << "Unable to send Pair-Verify M1\n";
    close(socket_fd);
    return 1;
  }
  response_bytes.clear();
  const auto verify_m2_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  while (std::chrono::steady_clock::now() < verify_m2_deadline &&
         (!options.stop_requested || !options.stop_requested()) &&
         !acp::airplay::complete_response_size(response_bytes)) {
    pollfd descriptor{socket_fd, POLLIN, 0};
    if (poll(&descriptor, 1, 100) <= 0)
      continue;
    const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
    if (count <= 0)
      break;
    response_bytes.insert(response_bytes.end(), buffer.begin(),
                          buffer.begin() + count);
  }
  const auto verify_m2_response = acp::airplay::parse_response(response_bytes);
  if (!verify_m2_response || verify_m2_response->status != 200) {
    std::cerr << "Pair-Verify M1 did not receive RTSP 200\n";
    close(socket_fd);
    return 1;
  }
  const auto verify_m2 = acp::airplay::decode_tlv8(verify_m2_response->body);
  const auto verify_m2_state = verify_m2.find(0x06);
  const auto peer_ephemeral = verify_m2.find(0x03);
  const auto verify_m2_encrypted = verify_m2.find(0x05);
  const auto shared = peer_ephemeral == verify_m2.end()
                          ? std::nullopt
                          : acp::airplay::x25519_shared(ephemeral->private_key,
                                                        peer_ephemeral->second);
  const auto verify_key =
      shared ? acp::airplay::hkdf_sha512(*shared, "Pair-Verify-Encrypt-Salt",
                                         "Pair-Verify-Encrypt-Info", 32)
             : acp::airplay::Bytes{};
  const auto verify_m2_plain =
      verify_m2_encrypted == verify_m2.end()
          ? std::nullopt
          : acp::airplay::open(verify_key, "PV-Msg02",
                               verify_m2_encrypted->second);
  if (verify_m2_state == verify_m2.end() ||
      verify_m2_state->second != acp::airplay::Bytes{2} || !shared ||
      verify_key.size() != 32 || !verify_m2_plain) {
    std::cerr << "Pair-Verify M2 is incomplete or could not be decrypted\n";
    close(socket_fd);
    return 1;
  }
  const auto verify_identity = acp::airplay::decode_tlv8(*verify_m2_plain);
  const auto verify_accessory_id = verify_identity.find(0x01);
  const auto verify_accessory_signature = verify_identity.find(0x0a);
  if (verify_accessory_id == verify_identity.end() ||
      verify_accessory_signature == verify_identity.end()) {
    std::cerr << "Pair-Verify M2 identity is incomplete\n";
    close(socket_fd);
    return 1;
  }
  acp::airplay::Bytes verify_accessory_signed(peer_ephemeral->second);
  verify_accessory_signed.insert(verify_accessory_signed.end(),
                                 verify_accessory_id->second.begin(),
                                 verify_accessory_id->second.end());
  verify_accessory_signed.insert(verify_accessory_signed.end(),
                                 ephemeral->public_key.begin(),
                                 ephemeral->public_key.end());
  if (!acp::airplay::ed25519_verify(pairing.accessory_public_key,
                                    verify_accessory_signed,
                                    verify_accessory_signature->second)) {
    std::cerr << "Pair-Verify M2 accessory signature validation failed\n";
    close(socket_fd);
    return 1;
  }
  acp::airplay::Bytes verify_controller_signed(ephemeral->public_key);
  verify_controller_signed.insert(verify_controller_signed.end(),
                                  pairing.controller_id.begin(),
                                  pairing.controller_id.end());
  verify_controller_signed.insert(verify_controller_signed.end(),
                                  peer_ephemeral->second.begin(),
                                  peer_ephemeral->second.end());
  const auto verify_controller_signature = acp::airplay::ed25519_sign(
      pairing.controller.private_key, verify_controller_signed);
  if (!verify_controller_signature) {
    std::cerr << "Unable to sign Pair-Verify M3\n";
    close(socket_fd);
    return 1;
  }
  const auto verify_m3_inner = acp::airplay::encode_tlv8(
      {{0x01, acp::airplay::Bytes(pairing.controller_id.begin(),
                                  pairing.controller_id.end())},
       {0x0a, *verify_controller_signature}});
  const auto verify_m3_encrypted =
      acp::airplay::seal(verify_key, "PV-Msg03", verify_m3_inner);
  if (!verify_m3_encrypted) {
    std::cerr << "Unable to encrypt Pair-Verify M3\n";
    close(socket_fd);
    return 1;
  }
  const auto verify_m3 =
      acp::airplay::encode_tlv8({{0x06, {3}}, {0x05, *verify_m3_encrypted}});
  const auto verify_m3_request = acp::airplay::encode_request(
      "POST", "/pair-verify", 5, verify_m3, "application/pairing+tlv8");
  if (!send_all(socket_fd, verify_m3_request)) {
    std::cerr << "Unable to send Pair-Verify M3\n";
    close(socket_fd);
    return 1;
  }
  response_bytes.clear();
  const auto verify_m4_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  while (std::chrono::steady_clock::now() < verify_m4_deadline &&
         (!options.stop_requested || !options.stop_requested()) &&
         !acp::airplay::complete_response_size(response_bytes)) {
    pollfd descriptor{socket_fd, POLLIN, 0};
    if (poll(&descriptor, 1, 100) <= 0)
      continue;
    const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
    if (count <= 0)
      break;
    response_bytes.insert(response_bytes.end(), buffer.begin(),
                          buffer.begin() + count);
  }
  const auto verify_m4_response = acp::airplay::parse_response(response_bytes);
  if (!verify_m4_response || verify_m4_response->status != 200) {
    std::cerr << "Pair-Verify M3 did not receive RTSP 200\n";
    return 1;
  }
  const auto verify_m4 = acp::airplay::decode_tlv8(verify_m4_response->body);
  const auto verify_m4_state = verify_m4.find(0x06);
  if (verify_m4_state == verify_m4.end() ||
      verify_m4_state->second != acp::airplay::Bytes{4}) {
    std::cerr << "Pair-Verify M4 validation failed\n";
    return 1;
  }
  const auto control_write = acp::airplay::hkdf_sha512(
      *shared, "Control-Salt", "Control-Write-Encryption-Key", 32);
  const auto control_read = acp::airplay::hkdf_sha512(
      *shared, "Control-Salt", "Control-Read-Encryption-Key", 32);
  if (control_write.size() != 32 || control_read.size() != 32) {
    std::cerr << "Unable to derive control channel keys\n";
    return 1;
  }
  std::cout
      << "AirPlay: Pair-Verify M4 validated; encrypted control keys derived\n";

  acp::airplay::ControlCipher control(control_read, control_write);
  acp::airplay::Bytes encrypted_read_buffer;
  const auto info_body =
      acp::airplay::encode_bplist(acp::airplay::PlistValue::Dictionary{
          {"name", acp::airplay::PlistValue("ACP-AA Bridge")},
          {"deviceID", acp::airplay::PlistValue(pairing.controller_id)},
          {"manufacturer", acp::airplay::PlistValue("ACP-AA Bridge")},
          {"model", acp::airplay::PlistValue("AA2ACP")},
          {"osVersion", acp::airplay::PlistValue("0.1")},
      });
  const auto info_response = send_encrypted(
      socket_fd, control, encrypted_read_buffer,
      acp::airplay::encode_request("POST", "/info", 6, info_body,
                                   "application/x-apple-binary-plist"),
      timeout_seconds, options.stop_requested);
  const auto info_plist = info_response
                              ? acp::airplay::decode_bplist(info_response->body)
                              : std::nullopt;
  if (!info_response || info_response->status != 200 ||
      !dictionary_of(info_plist)) {
    if (info_response) {
      std::cerr << "Encrypted /info response status=" << info_response->status
                << ", body=" << info_response->body.size() << "B\n";
    } else {
      std::cerr << "Encrypted /info had no decryptable RTSP response\n";
    }
    std::cerr << "Encrypted /info request failed\n";
    close(socket_fd);
    return 1;
  }
  std::cout << "AirPlay: encrypted /info capabilities received\n";

  const auto session_body =
      acp::airplay::encode_bplist(acp::airplay::PlistValue::Dictionary{
          {"timingPort", acp::airplay::PlistValue(std::uint64_t{0})},
          {"name", acp::airplay::PlistValue("ACP-AA Bridge")},
          {"deviceID", acp::airplay::PlistValue(pairing.controller_id)},
          {"model", acp::airplay::PlistValue("AA2ACP")},
      });
  const auto session_response =
      send_encrypted(socket_fd, control, encrypted_read_buffer,
                     acp::airplay::encode_request(
                         "SETUP", "rtsp://127.0.0.1/stream", 7, session_body,
                         "application/x-apple-binary-plist"),
                     timeout_seconds, options.stop_requested);
  const auto session_plist =
      session_response ? acp::airplay::decode_bplist(session_response->body)
                       : std::nullopt;
  const auto session_info = dictionary_of(session_plist);
  if (!session_response || session_response->status != 200 || !session_info ||
      !integer_at(*session_info, "timingPort") ||
      !integer_at(*session_info, "eventPort")) {
    std::cerr << "Encrypted session SETUP failed\n";
    close(socket_fd);
    return 1;
  }
  std::cout << "AirPlay: session SETUP received timing/event ports\n";

  constexpr std::string_view screen_stream_id =
      "3A5B6C7D-8E9F-4012-A345-B678C901D234";
  const auto screen_body =
      acp::airplay::encode_bplist(acp::airplay::PlistValue::Dictionary{
          {"streams",
           acp::airplay::PlistValue::Array{acp::airplay::PlistValue::Dictionary{
               {"type", acp::airplay::PlistValue(std::uint64_t{110})},
               {"streamConnectionID",
                acp::airplay::PlistValue(screen_stream_id.data())}}}},
      });
  const auto screen_response =
      send_encrypted(socket_fd, control, encrypted_read_buffer,
                     acp::airplay::encode_request(
                         "SETUP", "rtsp://127.0.0.1/stream", 8, screen_body,
                         "application/x-apple-binary-plist"),
                     timeout_seconds, options.stop_requested);
  const auto screen_plist =
      screen_response ? acp::airplay::decode_bplist(screen_response->body)
                      : std::nullopt;
  const auto screen_info = dictionary_of(screen_plist);
  const auto *stream_array = screen_info && screen_info->contains("streams")
                                 ? std::get_if<acp::airplay::PlistValue::Array>(
                                       &screen_info->at("streams").data)
                                 : nullptr;
  const auto *first_stream =
      stream_array && !stream_array->empty()
          ? std::get_if<acp::airplay::PlistValue::Dictionary>(
                &stream_array->front().data)
          : nullptr;
  const auto screen_port =
      first_stream ? integer_at(*first_stream, "dataPort") : std::nullopt;
  if (!screen_response || screen_response->status != 200 || !screen_port ||
      *screen_port == 0 || *screen_port > UINT16_MAX) {
    if (screen_response) {
      std::cerr << "Encrypted screen SETUP response status="
                << screen_response->status
                << ", body=" << screen_response->body.size() << "B\n";
      if (screen_info) {
        std::cerr << "Screen SETUP plist keys:";
        for (const auto &[key, value] : *screen_info) {
          std::cerr << ' ' << key;
          if (const auto *number = std::get_if<std::uint64_t>(&value.data))
            std::cerr << '=' << *number;
        }
        std::cerr << '\n';
      } else {
        std::cerr << "Screen SETUP plist could not be decoded\n";
      }
    } else {
      std::cerr << "Encrypted screen SETUP had no decryptable RTSP response\n";
    }
    std::cerr << "Encrypted screen SETUP failed\n";
    close(socket_fd);
    return 1;
  }
  std::cout << "AirPlay: screen SETUP received data port " << *screen_port
            << '\n';

  const auto record_response = send_encrypted(
      socket_fd, control, encrypted_read_buffer,
      acp::airplay::encode_request("RECORD", "rtsp://127.0.0.1/stream", 9, {},
                                   "application/octet-stream"),
      timeout_seconds, options.stop_requested);
  if (!record_response || record_response->status != 200) {
    std::cerr << "Encrypted RECORD failed\n";
    close(socket_fd);
    return 1;
  }
  std::cout << "AirPlay: encrypted RECORD accepted\n";
  if (video_path.empty() && !options.next_video_frame) {
    close(socket_fd);
    return 0;
  }

  std::vector<acp::airplay::Bytes> nalus;
  if (!video_path.empty())
    nalus = h264_nalus(video_path);

  // A live Android Auto stream starts with an Annex-B codec configuration.
  // Wait for it before opening the AirPlay data channel: CarPlay requires the
  // AVCC configuration to be its first data-stream payload.
  std::optional<acp::airplay::Bytes> config;
  std::size_t next_nalu = 0;
  while (!config) {
    config = avcc_config(nalus);
    if (config)
      break;
    if (!options.next_video_frame) {
      std::cerr << "Unable to parse H.264 SPS/PPS from " << video_path << '\n';
      close(socket_fd);
      return 1;
    }
    if (options.stop_requested && options.stop_requested()) {
      close(socket_fd);
      return 1;
    }
    const auto access_unit = options.next_video_frame();
    if (!access_unit) {
      std::cerr << "Android Auto video ended before H.264 SPS/PPS arrived\n";
      close(socket_fd);
      return 1;
    }
    const auto unit_nalus = h264_nalus(*access_unit);
    nalus.insert(nalus.end(), unit_nalus.begin(), unit_nalus.end());
  }
  const auto stream_key = acp::airplay::hkdf_sha512(
      *shared, std::string("DataStream-Salt") + std::string(screen_stream_id),
      "DataStream-Output-Encryption-Key", 32);
  const auto data_socket = connect_tcp(host, std::to_string(*screen_port));
  if (stream_key.size() != 32 || data_socket < 0) {
    std::cerr << "Unable to establish encrypted screen data stream\n";
    close(socket_fd);
    return 1;
  }
  acp::airplay::Bytes config_header(128);
  store_le32(std::span(config_header).first(4), config->size());
  config_header[4] = 1;
  if (!send_all(data_socket, config_header) ||
      !send_all(data_socket, *config)) {
    std::cerr << "Unable to send H.264 video config\n";
    close(data_socket);
    close(socket_fd);
    return 1;
  }
  std::uint64_t frame_counter{};
  std::size_t sent_frames{};
  const auto send_nalu = [&](const acp::airplay::Bytes &nalu) {
    if (nalu.empty() || ((nalu[0] & 0x1f) != 1 && (nalu[0] & 0x1f) != 5))
      return true;
    acp::airplay::Bytes frame;
    frame.reserve(nalu.size() + 4);
    frame.push_back(static_cast<std::uint8_t>(nalu.size() >> 24));
    frame.push_back(static_cast<std::uint8_t>(nalu.size() >> 16));
    frame.push_back(static_cast<std::uint8_t>(nalu.size() >> 8));
    frame.push_back(static_cast<std::uint8_t>(nalu.size()));
    frame.insert(frame.end(), nalu.begin(), nalu.end());
    acp::airplay::Bytes header(128);
    store_le32(std::span(header).first(4), frame.size() + 16);
    header[4] = 0;
    store_le64(std::span(header).subspan(8, 8), frame_counter);
    std::array<std::uint8_t, 12> nonce{};
    store_le64(std::span(nonce).subspan(4, 8), frame_counter);
    const auto encrypted =
        acp::airplay::seal_with_nonce(stream_key, nonce, frame, header);
    if (!encrypted || !send_all(data_socket, header) ||
        !send_all(data_socket, *encrypted))
      return false;
    ++frame_counter;
    ++sent_frames;
    return true;
  };
  for (; next_nalu < nalus.size(); ++next_nalu) {
    if (options.stop_requested && options.stop_requested()) {
      close(data_socket);
      close(socket_fd);
      return 1;
    }
    if (!send_nalu(nalus[next_nalu])) {
      std::cerr << "Unable to send encrypted H.264 frame\n";
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
    for (const auto &nalu : h264_nalus(*access_unit)) {
      if (!send_nalu(nalu)) {
        std::cerr << "Unable to send encrypted H.264 frame\n";
        close(data_socket);
        close(socket_fd);
        return 1;
      }
    }
  }
  close(data_socket);
  close(socket_fd);
  std::cout << "AirPlay: encrypted H.264 stream sent " << sent_frames
            << " frames\n";
  return 0;
}
