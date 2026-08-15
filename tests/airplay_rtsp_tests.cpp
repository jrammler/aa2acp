#include "acp/airplay/bplist.hpp"
#include "acp/airplay/control_cipher.hpp"
#include "acp/airplay/crypto.hpp"
#include "acp/airplay/rtsp.hpp"

#include <array>
#include <cassert>
#include <iostream>

int main() {
  constexpr std::array<std::uint8_t, 2> request_body{1, 2};
  const auto request = acp::airplay::encode_request(
      "POST", "/pair-setup", 1, request_body, "application/pairing+tlv8");
  const std::string request_text(request.begin(), request.end());
  assert(request_text.find("POST /pair-setup RTSP/1.0") == 0);
  assert(request_text.find("Content-Length: 2") != std::string::npos);

  const std::string raw =
      "RTSP/1.0 200 OK\r\nContent-Length: 3\r\nCSeq: 1\r\n\r\nabc";
  const auto parsed = acp::airplay::parse_response(std::span(
      reinterpret_cast<const std::uint8_t *>(raw.data()), raw.size()));
  assert(parsed.has_value());
  assert(parsed->status == 200);
  assert(parsed->headers.at("cseq") == "1");
  assert(parsed->body == acp::airplay::Bytes({'a', 'b', 'c'}));

  std::array<std::uint8_t, 300> large{};
  const auto encoded =
      acp::airplay::encode_tlv8({{3, {large.begin(), large.end()}}, {6, {2}}});
  const auto decoded = acp::airplay::decode_tlv8(encoded);
  assert(decoded.at(3).size() == large.size());
  assert(decoded.at(6) == acp::airplay::Bytes{2});

  const auto first = acp::airplay::x25519_generate();
  const auto second = acp::airplay::x25519_generate();
  assert(first && second);
  const auto first_shared =
      acp::airplay::x25519_shared(first->private_key, second->public_key);
  const auto second_shared =
      acp::airplay::x25519_shared(second->private_key, first->public_key);
  assert(first_shared && first_shared == second_shared);

  acp::airplay::Bytes control_key(32, 0x42);
  acp::airplay::ControlCipher writer(control_key, control_key);
  acp::airplay::ControlCipher reader(control_key, control_key);
  const auto encrypted = writer.encrypt(acp::airplay::Bytes{'o', 'k'});
  assert(encrypted);
  acp::airplay::Bytes partial(encrypted->begin(), encrypted->begin() + 1);
  assert(reader.decrypt_one(partial) == acp::airplay::Bytes{});
  partial.insert(partial.end(), encrypted->begin() + 1, encrypted->end());
  assert(reader.decrypt_one(partial) == acp::airplay::Bytes({'o', 'k'}));
  assert(partial.empty());

  const acp::airplay::PlistValue plist(acp::airplay::PlistValue::Dictionary{
      {"name", "C++ bridge"},
      {"streams",
       acp::airplay::PlistValue::Array{acp::airplay::PlistValue::Dictionary{
           {"type", acp::airplay::PlistValue(std::uint64_t{110})}}}},
  });
  const auto plist_bytes = acp::airplay::encode_bplist(plist);
  const auto parsed_plist = acp::airplay::decode_bplist(plist_bytes);
  assert(parsed_plist);
  const auto *dictionary =
      std::get_if<acp::airplay::PlistValue::Dictionary>(&parsed_plist->data);
  assert(dictionary &&
         std::get<std::string>(dictionary->at("name").data) == "C++ bridge");
  std::cout << "airplay RTSP tests passed\n";
}
