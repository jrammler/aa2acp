#include "aa2acp/bridge/carplay_ipc.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

int main() {
  const auto path =
      std::filesystem::temp_directory_path() / "aa2acp-carplay-ipc-test.sock";
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<std::uint8_t> received;
  aa2acp::bridge::FrameSocketReceiver receiver(
      path, "test", [&](const std::span<const std::uint8_t> frame) {
        std::lock_guard lock(mutex);
        received.assign(frame.begin(), frame.end());
        condition.notify_one();
      });
  assert(receiver.ready());
  const std::vector<std::uint8_t> expected{0, 1, 2, 0xff, 0x10};
  {
    aa2acp::bridge::FrameSocketWriter writer(path, "test");
    assert(writer.send(expected));
    std::unique_lock lock(mutex);
    assert(condition.wait_for(lock, std::chrono::seconds(2),
                              [&] { return received == expected; }));
    assert(!writer.send(std::span<const std::uint8_t>{}));
    const std::vector<std::uint8_t> oversized(4 * 1024 * 1024 + 1);
    assert(!writer.send(oversized));
  }
  {
    aa2acp::bridge::FrameSocketWriter writer(path, "test-reconnect");
    const std::vector<std::uint8_t> replacement{9, 8, 7};
    assert(writer.send(replacement));
    std::unique_lock lock(mutex);
    assert(condition.wait_for(lock, std::chrono::seconds(2),
                              [&] { return received == replacement; }));
  }
  // Close a raw peer immediately after its complete frame. The receiver must
  // still consume data reported alongside POLLHUP.
  const std::vector<std::uint8_t> final_frame{4, 5, 6, 7};
  const auto client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  assert(client >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path_text = path.string();
  assert(path_text.size() < sizeof(address.sun_path));
  std::copy(path_text.begin(), path_text.end(), address.sun_path);
  assert(connect(client, reinterpret_cast<const sockaddr *>(&address),
                 sizeof(address)) == 0);
  const std::array<std::uint8_t, 4> header{
      0, 0, 0, static_cast<std::uint8_t>(final_frame.size())};
  assert(send(client, header.data(), header.size(), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(header.size()));
  assert(send(client, final_frame.data(), final_frame.size(), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(final_frame.size()));
  close(client);
  {
    std::unique_lock lock(mutex);
    assert(condition.wait_for(lock, std::chrono::seconds(2),
                              [&] { return received == final_frame; }));
  }
  assert(!std::filesystem::exists(path) || receiver.ready());
}
