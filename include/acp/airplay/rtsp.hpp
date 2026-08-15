#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace acp::airplay {

using Bytes = std::vector<std::uint8_t>;

struct Response {
  int status{};
  std::map<std::string, std::string> headers;
  Bytes body;
};

Bytes encode_request(std::string_view method, std::string_view path, int cseq,
                     std::span<const std::uint8_t> body,
                     std::string_view content_type = {});
std::optional<std::size_t>
complete_response_size(std::span<const std::uint8_t> bytes);
std::optional<Response> parse_response(std::span<const std::uint8_t> bytes);

// HAP TLV8 used by AirPlay pair-setup and pair-verify. Repeated types are
// concatenated, matching the continuation convention for values >255 bytes.
Bytes encode_tlv8(const std::vector<std::pair<std::uint8_t, Bytes>> &fields);
std::map<std::uint8_t, Bytes> decode_tlv8(std::span<const std::uint8_t> bytes);

} // namespace acp::airplay
