#include "aa2acp/airplay/rtsp.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string_view>

namespace aa2acp::airplay {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

std::string trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t");
  return std::string(value.substr(first, last - first + 1));
}

} // namespace

Bytes encode_request(const std::string_view method, const std::string_view path,
                     const int cseq, const std::span<const std::uint8_t> body,
                     const std::string_view content_type) {
  std::string headers = std::string(method) + " " + std::string(path) +
                        " RTSP/1.0\r\n" + "User-Agent: AA2ACP/0.1\r\n" +
                        "CSeq: " + std::to_string(cseq) + "\r\n";
  if (!content_type.empty()) {
    headers += "Content-Type: " + std::string(content_type) + "\r\n";
  }
  headers += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  Bytes output(headers.begin(), headers.end());
  output.insert(output.end(), body.begin(), body.end());
  return output;
}

std::optional<std::size_t>
complete_response_size(const std::span<const std::uint8_t> bytes) {
  const std::string_view text(reinterpret_cast<const char *>(bytes.data()),
                              bytes.size());
  const auto header_end = text.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    return std::nullopt;
  }
  std::size_t content_length = 0;
  std::size_t cursor = 0;
  while (cursor < header_end) {
    const auto line_end = text.find("\r\n", cursor);
    const auto line = text.substr(
        cursor,
        (line_end == std::string_view::npos ? header_end : line_end) - cursor);
    const auto colon = line.find(':');
    if (colon != std::string_view::npos &&
        lower(std::string(line.substr(0, colon))) == "content-length") {
      const auto value = trim(line.substr(colon + 1));
      const auto [end, error] = std::from_chars(
          value.data(), value.data() + value.size(), content_length);
      if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
      }
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    cursor = line_end + 2;
  }
  const auto size = header_end + 4 + content_length;
  return bytes.size() >= size ? std::optional<std::size_t>(size) : std::nullopt;
}

std::optional<Response>
parse_response(const std::span<const std::uint8_t> bytes) {
  const auto full_size = complete_response_size(bytes);
  if (!full_size) {
    return std::nullopt;
  }
  const std::string_view text(reinterpret_cast<const char *>(bytes.data()),
                              *full_size);
  const auto header_end = text.find("\r\n\r\n");
  const auto line_end = text.find("\r\n");
  if (line_end == std::string_view::npos || !text.starts_with("RTSP/1.0 ")) {
    return std::nullopt;
  }
  const auto status_text = text.substr(9, line_end - 9);
  const auto space = status_text.find(' ');
  int status = 0;
  const auto status_number = status_text.substr(0, space);
  const auto [end, error] =
      std::from_chars(status_number.data(),
                      status_number.data() + status_number.size(), status);
  if (error != std::errc{} ||
      end != status_number.data() + status_number.size()) {
    return std::nullopt;
  }
  Response response;
  response.status = status;
  std::size_t cursor = line_end + 2;
  while (cursor < header_end) {
    const auto next = text.find("\r\n", cursor);
    const auto line = text.substr(cursor, next - cursor);
    const auto colon = line.find(':');
    if (colon != std::string_view::npos) {
      response.headers.emplace(lower(trim(line.substr(0, colon))),
                               trim(line.substr(colon + 1)));
    }
    cursor = next + 2;
  }
  response.body.assign(bytes.begin() +
                           static_cast<std::ptrdiff_t>(header_end + 4),
                       bytes.begin() + static_cast<std::ptrdiff_t>(*full_size));
  return response;
}

Bytes encode_tlv8(const std::vector<std::pair<std::uint8_t, Bytes>> &fields) {
  Bytes output;
  for (const auto &[type, value] : fields) {
    for (std::size_t offset = 0; offset < value.size(); offset += 255) {
      const auto size = std::min<std::size_t>(255, value.size() - offset);
      output.push_back(type);
      output.push_back(static_cast<std::uint8_t>(size));
      output.insert(output.end(),
                    value.begin() + static_cast<std::ptrdiff_t>(offset),
                    value.begin() + static_cast<std::ptrdiff_t>(offset + size));
    }
  }
  return output;
}

std::map<std::uint8_t, Bytes>
decode_tlv8(const std::span<const std::uint8_t> bytes) {
  std::map<std::uint8_t, Bytes> result;
  for (std::size_t offset = 0; offset + 2 <= bytes.size();) {
    const auto type = bytes[offset++];
    const auto size = bytes[offset++];
    if (offset + size > bytes.size()) {
      return {};
    }
    result[type].insert(
        result[type].end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
    offset += size;
  }
  return result;
}

} // namespace aa2acp::airplay
