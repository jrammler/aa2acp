#pragma once

#include "aa2acp/airplay/bplist.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace aa2acp::airplay {

struct HeadUnitCapabilities {
  std::string head_unit_mac;
  std::uint32_t width_pixels{};
  std::uint32_t height_pixels{};
  std::uint32_t max_fps{};
  bool media_pcm_48k_stereo{};
  bool guidance_pcm_16k_mono{};
  bool system_pcm_16k_mono{};
};

std::optional<HeadUnitCapabilities>
head_unit_capabilities(const PlistValue::Dictionary &info,
                       std::string head_unit_mac);
std::optional<HeadUnitCapabilities>
load_head_unit_capabilities(const std::filesystem::path &path,
                            std::string_view head_unit_mac);
bool save_head_unit_capabilities(const std::filesystem::path &path,
                                 const HeadUnitCapabilities &profile);

} // namespace aa2acp::airplay
