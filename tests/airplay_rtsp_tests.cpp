#include "aa2acp/airplay/bplist.hpp"
#include "aa2acp/airplay/control_cipher.hpp"
#include "aa2acp/airplay/crypto.hpp"
#include "aa2acp/airplay/display_profile.hpp"
#include "aa2acp/airplay/pairing_store.hpp"
#include "aa2acp/airplay/rtsp.hpp"

#include <array>
#include <cassert>
#include <filesystem>
#include <iostream>

int main() {
  constexpr std::array<std::uint8_t, 2> request_body{1, 2};
  const auto request = aa2acp::airplay::encode_request(
      "POST", "/pair-setup", 1, request_body, "application/pairing+tlv8");
  const std::string request_text(request.begin(), request.end());
  assert(request_text.find("POST /pair-setup RTSP/1.0") == 0);
  assert(request_text.find("Content-Length: 2") != std::string::npos);

  const std::string raw =
      "RTSP/1.0 200 OK\r\nContent-Length: 3\r\nCSeq: 1\r\n\r\nabc";
  const auto parsed = aa2acp::airplay::parse_response(std::span(
      reinterpret_cast<const std::uint8_t *>(raw.data()), raw.size()));
  assert(parsed.has_value());
  assert(parsed->status == 200);
  assert(parsed->headers.at("cseq") == "1");
  assert(parsed->body == aa2acp::airplay::Bytes({'a', 'b', 'c'}));

  std::array<std::uint8_t, 300> large{};
  const auto encoded = aa2acp::airplay::encode_tlv8(
      {{3, {large.begin(), large.end()}}, {6, {2}}});
  const auto decoded = aa2acp::airplay::decode_tlv8(encoded);
  assert(decoded.at(3).size() == large.size());
  assert(decoded.at(6) == aa2acp::airplay::Bytes{2});

  const auto first = aa2acp::airplay::x25519_generate();
  const auto second = aa2acp::airplay::x25519_generate();
  assert(first && second);
  const auto first_shared =
      aa2acp::airplay::x25519_shared(first->private_key, second->public_key);
  const auto second_shared =
      aa2acp::airplay::x25519_shared(second->private_key, first->public_key);
  assert(first_shared && first_shared == second_shared);

  aa2acp::airplay::Bytes control_key(32, 0x42);
  aa2acp::airplay::ControlCipher writer(control_key, control_key);
  aa2acp::airplay::ControlCipher reader(control_key, control_key);
  const auto encrypted = writer.encrypt(aa2acp::airplay::Bytes{'o', 'k'});
  assert(encrypted);
  aa2acp::airplay::Bytes partial(encrypted->begin(), encrypted->begin() + 1);
  assert(reader.decrypt_one(partial) == aa2acp::airplay::Bytes{});
  partial.insert(partial.end(), encrypted->begin() + 1, encrypted->end());
  assert(reader.decrypt_one(partial) == aa2acp::airplay::Bytes({'o', 'k'}));
  assert(partial.empty());

  const aa2acp::airplay::PlistValue plist(
      aa2acp::airplay::PlistValue::Dictionary{
          {"name", "C++ bridge"},
          {"streams",
           aa2acp::airplay::PlistValue::Array{
               aa2acp::airplay::PlistValue::Dictionary{
                   {"type", aa2acp::airplay::PlistValue(std::uint64_t{110})}}}},
      });
  const auto plist_bytes = aa2acp::airplay::encode_bplist(plist);
  const auto parsed_plist = aa2acp::airplay::decode_bplist(plist_bytes);
  assert(parsed_plist);
  const auto *dictionary =
      std::get_if<aa2acp::airplay::PlistValue::Dictionary>(&parsed_plist->data);
  assert(dictionary &&
         std::get<std::string>(dictionary->at("name").data) == "C++ bridge");

  const aa2acp::airplay::PlistValue::Dictionary info{
      {"displays",
       aa2acp::airplay::PlistValue::Array{
           aa2acp::airplay::PlistValue::Dictionary{
               {"type", aa2acp::airplay::PlistValue(std::uint64_t{110})},
               {"widthPixels",
                aa2acp::airplay::PlistValue(std::uint64_t{1920})},
               {"heightPixels",
                aa2acp::airplay::PlistValue(std::uint64_t{720})},
               {"maxFPS", aa2acp::airplay::PlistValue(std::uint64_t{60})}}}}};
  const auto display = aa2acp::airplay::main_display_profile(info, "AA:BB:CC");
  assert(display && display->width_pixels == 1920 &&
         display->height_pixels == 720 && display->max_fps == 60);
  const auto display_path =
      std::filesystem::temp_directory_path() / "aa2acp-display-profile-test";
  assert(aa2acp::airplay::save_display_profile(display_path, *display));
  assert(aa2acp::airplay::load_display_profile(display_path, "AA:BB:CC"));
  assert(!aa2acp::airplay::load_display_profile(display_path, "DD:EE:FF"));
  std::filesystem::remove(display_path);

  const auto identity = aa2acp::airplay::ed25519_generate();
  assert(identity);
  const auto pairing_path =
      std::filesystem::temp_directory_path() / "acp-airplay-pairing-test.bin";
  const aa2acp::airplay::PairingRecord record{"controller", *identity,
                                              aa2acp::airplay::Bytes(32, 0x11)};
  assert(aa2acp::airplay::save_pairing_record(pairing_path, record));
  const auto restored = aa2acp::airplay::load_pairing_record(pairing_path);
  assert(restored && restored->controller_id == record.controller_id &&
         restored->controller.public_key == record.controller.public_key &&
         restored->accessory_public_key == record.accessory_public_key);
  std::filesystem::remove(pairing_path);
  std::cout << "airplay RTSP tests passed\n";
}
