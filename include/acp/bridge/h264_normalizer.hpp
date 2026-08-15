#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace acp::bridge {

class H264Normalizer final {
public:
  H264Normalizer();
  ~H264Normalizer();

  H264Normalizer(const H264Normalizer &) = delete;
  H264Normalizer &operator=(const H264Normalizer &) = delete;

  std::vector<std::vector<std::uint8_t>>
  normalize(std::span<const std::uint8_t> access_unit, std::string *error);

private:
  struct Context;
  Context *context_{};
};

} // namespace acp::bridge
