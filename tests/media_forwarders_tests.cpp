#include "aa2acp/bridge/media_forwarders.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int connect_with_retry(const std::filesystem::path &path) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client < 0)
      return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto value = path.string();
    std::copy(value.begin(), value.end(), address.sun_path);
    if (connect(client, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) == 0)
      return client;
    close(client);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
}

bool read_exact(const int client, std::vector<std::uint8_t> &bytes) {
  std::size_t offset{};
  while (offset < bytes.size()) {
    pollfd descriptor{client, POLLIN, 0};
    if (poll(&descriptor, 1, 2000) <= 0)
      return false;
    const auto count =
        recv(client, bytes.data() + offset, bytes.size() - offset, 0);
    if (count <= 0)
      return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

std::vector<std::uint8_t> read_frame(const int client) {
  std::vector<std::uint8_t> header(4);
  assert(read_exact(client, header));
  const auto size = (static_cast<std::size_t>(header[0]) << 24) |
                    (static_cast<std::size_t>(header[1]) << 16) |
                    (static_cast<std::size_t>(header[2]) << 8) |
                    static_cast<std::size_t>(header[3]);
  std::vector<std::uint8_t> frame(size);
  assert(read_exact(client, frame));
  return frame;
}

} // namespace

int main() {
  const auto directory = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(static_cast<long long>(getpid()));
  const auto socket_path =
      directory / ("aa2acp-video-test-" + suffix + ".sock");
  const auto capture_path = directory / ("aa2acp-video-capture-" + suffix);
  const auto hardlink_path = directory / ("aa2acp-video-hardlink-" + suffix);
  const auto keyframe_socket_path =
      directory / ("aa2acp-video-keyframe-" + suffix + ".sock");
  std::error_code cleanup_error;
  std::filesystem::remove(capture_path, cleanup_error);
  std::filesystem::remove(hardlink_path, cleanup_error);
  std::filesystem::remove(keyframe_socket_path, cleanup_error);
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

  {
    aa2acp::bridge::VideoSocketForwarder forwarder(keyframe_socket_path);
    assert(forwarder.ready());
    const std::vector<std::uint8_t> keyframe{0, 0, 0, 1, 0x67, 0x64, 0,    0x1f,
                                             0, 0, 0, 1, 0x68, 0xee, 0x3c, 0x80,
                                             0, 0, 0, 1, 0x65, 1,    2,    3};
    forwarder.push(keyframe);
    const auto client = connect_with_retry(keyframe_socket_path);
    assert(client >= 0);
    const auto config = read_frame(client);
    assert(config.size() > 8);
    assert(config[0] == 0 && config[1] == 0 && config[2] == 0 &&
           config[3] == 1);
    assert(read_frame(client) == keyframe);
    close(client);
  }
  std::filesystem::remove(keyframe_socket_path);
}
