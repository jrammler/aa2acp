#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <sys/types.h>

#include "aa2acp/iap2/bluetooth_worker.hpp"

namespace aa2acp::bridge {

// Hosts the CarPlay/IAP2 Bluetooth worker in a forked child process so a
// crashed or wedged session can be torn down without taking the daemon down.
// Control messages cross a sequenced UNIX socket; worker output is piped to
// the daemon's stdout.
class CarPlayWorker final {
public:
  // Hooks invoked while a session runs so the caller can mirror session
  // events into its own state.
  struct RunHooks {
    std::function<void()> on_connection_lost;
    std::function<void(const iap2::PairingConfirmationMessage &)>
        on_pairing_confirmation;
    std::function<void()> on_pairing_reset;
  };

  CarPlayWorker();
  ~CarPlayWorker();

  CarPlayWorker(const CarPlayWorker &) = delete;
  CarPlayWorker &operator=(const CarPlayWorker &) = delete;

  // Runs one worker session to completion and returns its exit status.
  int run(std::vector<std::string> arguments, const std::stop_token stop,
          const std::atomic_bool &phone_disconnected,
          std::atomic<pid_t> &active_child, const RunHooks &hooks);

  // Forwards a user's decision for a pending pairing confirmation.
  bool answer_pairing_confirmation(
      const iap2::PairingConfirmationMessage &confirmation, bool confirmed);

private:
  pid_t pid_{-1};
  int control_fd_{-1};
  int output_fd_{-1};
  std::mutex mutex_;
  std::mutex command_mutex_;
};

} // namespace aa2acp::bridge
