#include "aa2acp/airplay/display_profile.hpp"

#include <charconv>
#include <fstream>
#include <string_view>
#include <sys/stat.h>

namespace aa2acp::airplay {
namespace {

constexpr char kMagic[] = "AA2ACP-DISPLAY-1";

std::optional<std::uint64_t>
integer_at(const PlistValue::Dictionary &dictionary,
           const std::string_view key) {
  const auto item = dictionary.find(std::string(key));
  if (item == dictionary.end())
    return std::nullopt;
  return std::get_if<std::uint64_t>(&item->second.data)
             ? std::optional(std::get<std::uint64_t>(item->second.data))
             : std::nullopt;
}

bool valid(const DisplayProfile &profile) {
  return !profile.head_unit_mac.empty() && profile.width_pixels > 0 &&
         profile.height_pixels > 0 && profile.max_fps > 0 &&
         profile.head_unit_mac.find_first_of("\r\n=") == std::string::npos;
}

std::optional<std::uint32_t> parse_u32(const std::string_view value) {
  std::uint32_t result{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  return error == std::errc{} && end == value.data() + value.size()
             ? std::optional(result)
             : std::nullopt;
}

} // namespace

std::optional<DisplayProfile>
main_display_profile(const PlistValue::Dictionary &info,
                     std::string head_unit_mac) {
  const auto displays = info.find("displays");
  if (displays == info.end())
    return std::nullopt;
  const auto *array = std::get_if<PlistValue::Array>(&displays->second.data);
  if (array == nullptr)
    return std::nullopt;
  for (const auto &display : *array) {
    const auto *dictionary = std::get_if<PlistValue::Dictionary>(&display.data);
    if (dictionary == nullptr || integer_at(*dictionary, "type") != 110)
      continue;
    const auto width = integer_at(*dictionary, "widthPixels");
    const auto height = integer_at(*dictionary, "heightPixels");
    const auto fps = integer_at(*dictionary, "maxFPS");
    if (!width || !height || !fps || *width > UINT32_MAX ||
        *height > UINT32_MAX || *fps > UINT32_MAX)
      return std::nullopt;
    DisplayProfile profile{
        std::move(head_unit_mac), static_cast<std::uint32_t>(*width),
        static_cast<std::uint32_t>(*height), static_cast<std::uint32_t>(*fps)};
    return valid(profile) ? std::optional(std::move(profile)) : std::nullopt;
  }
  return std::nullopt;
}

std::optional<DisplayProfile>
load_display_profile(const std::filesystem::path &path,
                     const std::string_view head_unit_mac) {
  std::ifstream stream(path);
  std::string line;
  if (!std::getline(stream, line) || line != kMagic)
    return std::nullopt;
  DisplayProfile profile;
  while (std::getline(stream, line)) {
    const auto separator = line.find('=');
    if (separator == std::string::npos)
      return std::nullopt;
    const auto key = line.substr(0, separator);
    const auto value = line.substr(separator + 1);
    if (key == "head_unit_mac")
      profile.head_unit_mac = value;
    else if (key == "width_pixels") {
      const auto parsed = parse_u32(value);
      if (!parsed)
        return std::nullopt;
      profile.width_pixels = *parsed;
    } else if (key == "height_pixels") {
      const auto parsed = parse_u32(value);
      if (!parsed)
        return std::nullopt;
      profile.height_pixels = *parsed;
    } else if (key == "max_fps") {
      const auto parsed = parse_u32(value);
      if (!parsed)
        return std::nullopt;
      profile.max_fps = *parsed;
    } else
      return std::nullopt;
  }
  return valid(profile) && profile.head_unit_mac == head_unit_mac
             ? std::optional(std::move(profile))
             : std::nullopt;
}

bool save_display_profile(const std::filesystem::path &path,
                          const DisplayProfile &profile) {
  if (!valid(profile))
    return false;
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error)
    return false;
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::trunc);
    if (!stream)
      return false;
    stream << kMagic << '\n'
           << "head_unit_mac=" << profile.head_unit_mac << '\n'
           << "width_pixels=" << profile.width_pixels << '\n'
           << "height_pixels=" << profile.height_pixels << '\n'
           << "max_fps=" << profile.max_fps << '\n';
    if (!stream)
      return false;
  }
  return chmod(temporary.c_str(), S_IRUSR | S_IWUSR) == 0 &&
         std::rename(temporary.c_str(), path.c_str()) == 0;
}

} // namespace aa2acp::airplay
