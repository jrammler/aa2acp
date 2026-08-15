#pragma once

#include "acp/iap2/csm.hpp"
#include "acp/iap2/network_manager.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace acp::iap2 {

class PhoneLink;

// A deliberately small post-authentication CarPlay probe. It records the
// accessory's subscriptions, advertises availability, and validates that the
// accessory returns CarPlayStartSession. Wi-Fi and AirPlay remain separate
// milestones.
class CarPlayProbe {
public:
  explicit CarPlayProbe(std::string bluetooth_identifier = {});

  void attach(PhoneLink &link);
  void request_wifi_configuration(bool enabled = true);
  void set_wifi_join_handler(
      std::function<bool(const AccessoryWifiConfiguration &)> handler);
  void begin();
  void receive(std::span<const std::uint8_t> bytes);

  [[nodiscard]] bool done() const;
  [[nodiscard]] bool failed() const;
  [[nodiscard]] bool started() const;

private:
  void fail(const char *message);
  void handle(const csm::Message &message);

  csm::Decoder decoder_;
  PhoneLink *link_{};
  std::string bluetooth_identifier_;
  std::function<bool(const AccessoryWifiConfiguration &)> wifi_join_handler_;
  bool request_wifi_{};
  bool awaiting_wifi_configuration_{};
  bool started_{};
  bool done_{};
  bool failed_{};
};

} // namespace acp::iap2
