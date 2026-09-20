#include "aa2acp/bridge/media_forwarders.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

int main() {
  const auto directory = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(static_cast<long long>(getpid()));
  const auto socket_path =
      directory / ("aa2acp-video-test-" + suffix + ".sock");
  const auto capture_path = directory / ("aa2acp-video-capture-" + suffix);
  const auto hardlink_path = directory / ("aa2acp-video-hardlink-" + suffix);
  std::error_code cleanup_error;
  std::filesystem::remove(capture_path, cleanup_error);
  std::filesystem::remove(hardlink_path, cleanup_error);
  {
    std::ofstream file(capture_path, std::ios::binary);
    file << "private-data";
  }
  std::error_code link_error;
  std::filesystem::create_hard_link(capture_path, hardlink_path, link_error);
  assert(!link_error);
  assert(setenv("AA2ACP_DUMP_H264", hardlink_path.c_str(), 1) == 0);
  {
    aa2acp::bridge::VideoSocketForwarder forwarder(socket_path);
    const std::vector<std::uint8_t> frame{0, 0, 0, 1, 0x65, 1, 2, 3};
    forwarder.push(frame);
  }
  assert(unsetenv("AA2ACP_DUMP_H264") == 0);
  std::ifstream file(capture_path, std::ios::binary);
  const std::string contents((std::istreambuf_iterator<char>(file)), {});
  assert(contents == "private-data");
  std::filesystem::remove(socket_path);
  std::filesystem::remove(hardlink_path);
  std::filesystem::remove(capture_path);
}
