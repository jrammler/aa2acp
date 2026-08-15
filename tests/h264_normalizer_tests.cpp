#include "aa2acp/bridge/h264_normalizer.hpp"

#include <array>
#include <cassert>
#include <string>
#include <vector>

int main() {
  // SPS/PPS emitted by a development Android phone. Some CarPlay decoders
  // reject this metadata until the h264_metadata filter normalizes it.
  constexpr std::array<std::uint8_t, 29> android_configuration{
      0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x80, 0x1f, 0xda, 0x01,
      0x40, 0x16, 0xe9, 0xa8, 0x28, 0x30, 0x28, 0x36, 0x85, 0x09,
      0xa8, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xf2};
  aa2acp::bridge::H264Normalizer normalizer;
  std::string error;
  const auto output = normalizer.normalize(android_configuration, &error);
  assert(error.empty());
  assert(output.size() == 1);
  assert(output.front() !=
         std::vector<std::uint8_t>(android_configuration.begin(),
                                   android_configuration.end()));
}
