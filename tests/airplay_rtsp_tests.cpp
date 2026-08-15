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
  std::cout << "airplay RTSP tests passed\n";
}
