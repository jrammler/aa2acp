#pragma once

#include "aa2acp/iap2/csm.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace aa2acp::iap2 {

class PhoneLink;

// The minimum CSM exchange needed to prove that the iAP2 control session and a
// development-only software-MFi implementation work end-to-end.
class BootstrapSession {
public:
  void attach(PhoneLink &link);
  void begin();
  void receive(std::span<const std::uint8_t> bytes);

  [[nodiscard]] bool done() const;
  [[nodiscard]] bool failed() const;
  [[nodiscard]] bool started() const;

private:
  enum class Stage {
    Idle,
    AwaitIdentification,
    AwaitCertificate,
    AwaitResponse,
    Done,
    Failed
  };

  void send_empty(std::uint16_t id);
  void fail(const char *message);
  void handle(const csm::Message &message);

  csm::Decoder decoder_;
  PhoneLink *link_{};
  std::array<std::uint8_t, 32> challenge_{};
  std::vector<std::uint8_t> certificate_;
  Stage stage_{Stage::Idle};
};

} // namespace aa2acp::iap2
