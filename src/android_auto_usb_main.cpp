#include "aa2acp/aa/wired_receiver.hpp"

#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t stop_requested{};

void handle_signal(int) { stop_requested = 1; }

const char *event_name(const aa2acp::aa::WiredReceiverEventType type) {
  switch (type) {
  case aa2acp::aa::WiredReceiverEventType::waiting_for_phone:
    return "waiting";
  case aa2acp::aa::WiredReceiverEventType::aoap_transport_ready:
    return "ready";
  case aa2acp::aa::WiredReceiverEventType::control_session_ready:
    return "session";
  case aa2acp::aa::WiredReceiverEventType::video_stream_configured:
    return "video-configured";
  case aa2acp::aa::WiredReceiverEventType::video_stream_started:
    return "video";
  case aa2acp::aa::WiredReceiverEventType::disconnected:
    return "disconnected";
  case aa2acp::aa::WiredReceiverEventType::error:
    return "error";
  }
  return "unknown";
}

} // namespace

int main() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  aa2acp::aa::WiredReceiver receiver([](const auto &event) {
    std::cout << "Android Auto USB [" << event_name(event.type)
              << "]: " << event.detail << '\n';
  });
  std::string error;
  if (!receiver.start(&error)) {
    std::cerr << "Android Auto USB: " << error << '\n';
    return 1;
  }
  while (!stop_requested)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  receiver.stop();
}
