#pragma once

namespace aa2acp::iap2 {

// Runs the Bluetooth iAP2 and optional AirPlay bridge worker.  Kept separate
// from its command-line wrapper so the bridge daemon can invoke it in its
// isolated child process without depending on a sibling executable.
int run_bluetooth_worker(int argc, char **argv);
void request_bluetooth_worker_stop();

} // namespace aa2acp::iap2
