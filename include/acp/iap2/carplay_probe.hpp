#pragma once

#include "acp/iap2/csm.hpp"

#include <cstdint>
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

    void attach(PhoneLink& link);
    void begin();
    void receive(std::span<const std::uint8_t> bytes);

    [[nodiscard]] bool done() const;
    [[nodiscard]] bool failed() const;
    [[nodiscard]] bool started() const;

  private:
    void fail(const char* message);
    void handle(const csm::Message& message);

    csm::Decoder decoder_;
    PhoneLink* link_{};
    std::string bluetooth_identifier_;
    bool started_{};
    bool done_{};
    bool failed_{};
};

}  // namespace acp::iap2
