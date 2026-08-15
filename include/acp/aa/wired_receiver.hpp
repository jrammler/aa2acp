#pragma once

#include <functional>
#include <memory>
#include <string>

namespace acp::aa {

// This is deliberately a transport-level milestone. "ready" means the phone
// has entered Android Open Accessory Protocol mode and its USB endpoints are
// owned by the bridge. The Android Auto service/channel handshake follows in
// the next milestone and is the point at which projection can start.
enum class WiredReceiverEventType {
  waiting_for_phone,
  aoap_transport_ready,
  control_session_ready,
  disconnected,
  error,
};

struct WiredReceiverEvent {
  WiredReceiverEventType type;
  std::string detail;
};

class WiredReceiver {
public:
  using EventCallback = std::function<void(const WiredReceiverEvent &)>;

  explicit WiredReceiver(EventCallback callback);
  ~WiredReceiver();

  WiredReceiver(const WiredReceiver &) = delete;
  WiredReceiver &operator=(const WiredReceiver &) = delete;

  // Begins monitoring current and newly attached USB devices. AASDK performs
  // AOAP negotiation for a normal Android USB device, then reports readiness
  // after its accessory-mode re-enumeration.
  bool start(std::string *error = nullptr);
  void stop();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace acp::aa
