#pragma once

#include "aa2acp/bridge/logging.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace aa2acp::iap2 {

using PairingLogFunction =
    std::function<void(aa2acp::bridge::LogLevel, const std::string &)>;

// Ensures BlueZ knows, pairs, and trusts a device using a DisplayYesNo agent.
// Numeric-comparison requests are accepted automatically. It persists the bond
// in BlueZ; it does not open iAP2.
bool ensure_bluez_pairing(std::string_view mac, int timeout_seconds,
                          const PairingLogFunction &log = {});

// A connected iAP2 transport obtained through BlueZ's Profile1 API. BlueZ
// discovers the remote service and owns the profile until close() is called.
class BluezIap2Connection {
public:
  struct State;

  BluezIap2Connection(BluezIap2Connection &&) noexcept;
  BluezIap2Connection &operator=(BluezIap2Connection &&) noexcept;
  ~BluezIap2Connection();

  BluezIap2Connection(const BluezIap2Connection &) = delete;
  BluezIap2Connection &operator=(const BluezIap2Connection &) = delete;

  int fd() const;
  void close();

private:
  explicit BluezIap2Connection(std::unique_ptr<State> state);

  std::unique_ptr<State> state_;

  friend std::optional<BluezIap2Connection>
  connect_bluez_iap2(std::string_view, int, const PairingLogFunction &);
};

// Connects to the remote Apple iAP2 service using bluetoothd's cached/service
// discovery state rather than creating a competing SDP client connection.
std::optional<BluezIap2Connection>
connect_bluez_iap2(std::string_view mac, int timeout_seconds,
                   const PairingLogFunction &log = {});

} // namespace aa2acp::iap2
