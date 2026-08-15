#pragma once

#include "aa2acp/airplay/bplist.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace aa2acp::airplay {

struct DisplayProfile {
  std::string head_unit_mac;
  std::uint32_t width_pixels{};
  std::uint32_t height_pixels{};
  std::uint32_t max_fps{};
};

std::optional<DisplayProfile>
main_display_profile(const PlistValue::Dictionary &info,
                     std::string head_unit_mac);
std::optional<DisplayProfile>
load_display_profile(const std::filesystem::path &path,
                     std::string_view head_unit_mac);
bool save_display_profile(const std::filesystem::path &path,
                          const DisplayProfile &profile);

} // namespace aa2acp::airplay
