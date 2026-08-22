#pragma once

#include <cstdint>
#include <string_view>

namespace aa2acp::iap2 {

// Runs the Bluetooth iAP2 and optional AirPlay bridge worker.  Kept separate
// from its command-line wrapper so the bridge daemon can invoke it in its
// isolated child process without depending on a sibling executable.
int run_bluetooth_worker(int argc, char **argv);
void request_bluetooth_worker_stop();
void reset_bluetooth_worker_stop();

struct PairingConfirmationMessage {
  std::uint64_t id{};
  std::uint32_t passkey{};
  char address[18]{};
};

void set_pairing_confirmation_control_fd(int fd);
bool request_pairing_confirmation(std::string_view address,
                                  std::uint32_t passkey, std::uint64_t *id);
bool pairing_confirmation_result(std::uint64_t id, bool *confirmed);
void answer_pairing_confirmation(std::uint64_t id, bool confirmed);
void cancel_pairing_confirmation(std::uint64_t id);

} // namespace aa2acp::iap2
