#include "aa2acp/bridge/carplay_ipc.hpp"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
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
  assert(!std::filesystem::exists(path) || receiver.ready());
}
