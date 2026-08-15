#pragma once

#include "acp/airplay/rtsp.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <variant>

namespace acp::airplay {

struct PlistValue {
  using Array = std::vector<PlistValue>;
  using Dictionary = std::map<std::string, PlistValue>;
  std::variant<bool, std::uint64_t, std::string, Bytes, Array, Dictionary> data;

  PlistValue(bool value) : data(value) {}
  PlistValue(const std::uint64_t value) : data(value) {}
  PlistValue(std::string value) : data(std::move(value)) {}
  PlistValue(const char *value) : data(std::string(value)) {}
  PlistValue(Bytes value) : data(std::move(value)) {}
  PlistValue(Array value) : data(std::move(value)) {}
  PlistValue(Dictionary value) : data(std::move(value)) {}
};

Bytes encode_bplist(const PlistValue &root);
std::optional<PlistValue> decode_bplist(std::span<const std::uint8_t> bytes);

} // namespace acp::airplay
