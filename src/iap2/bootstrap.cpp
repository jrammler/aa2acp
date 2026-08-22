#include "aa2acp/iap2/bootstrap.hpp"
#include "aa2acp/bridge/logging.hpp"

#include "aa2acp/iap2/link_layer.hpp"

#include <iostream>
#include <random>

namespace aa2acp::iap2 {

void BootstrapSession::attach(PhoneLink &link) { link_ = &link; }

void BootstrapSession::begin() {
  stage_ = Stage::AwaitIdentification;
  send_empty(csm::kStartIdentification);
  if (aa2acp::bridge::debug_logging_enabled())
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
        << "CSM: sent StartIdentification\n";
}

void BootstrapSession::receive(const std::span<const std::uint8_t> bytes) {
  decoder_.push(bytes);
  while (const auto message = decoder_.next()) {
    handle(*message);
  }
}

bool BootstrapSession::done() const { return stage_ == Stage::Done; }
bool BootstrapSession::failed() const { return stage_ == Stage::Failed; }
bool BootstrapSession::started() const { return stage_ != Stage::Idle; }

void BootstrapSession::send_empty(const std::uint16_t id) {
  if (link_ == nullptr || !link_->send_control(csm::encode(id))) {
    fail("unable to send CSM message");
  }
}

void BootstrapSession::fail(const char *message) {
  stage_ = Stage::Failed;
  aa2acp::bridge::log(aa2acp::bridge::LogLevel::error)
      << "CSM: " << message << '\n';
}

void BootstrapSession::handle(const csm::Message &message) {
  if (aa2acp::bridge::debug_logging_enabled())
    aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
        << "CSM: received 0x" << std::hex << message.id << std::dec << '\n';
  if (stage_ == Stage::AwaitIdentification) {
    if (message.id == csm::kIdentificationRejected) {
      fail("identification rejected");
    } else if (message.id == csm::kIdentificationInformation) {
      send_empty(csm::kIdentificationAccepted);
      send_empty(csm::kRequestAuthenticationCertificate);
      stage_ = Stage::AwaitCertificate;
    }
    return;
  }
  if (stage_ == Stage::AwaitCertificate) {
    if (message.id != csm::kAuthenticationCertificate) {
      return;
    }
    const auto certificate = csm::first_bytes_parameter(message.payload, 0);
    if (!certificate) {
      fail("certificate message has no certificate parameter");
      return;
    }
    certificate_ = *certificate;
    std::random_device random;
    for (auto &byte : challenge_) {
      byte = static_cast<std::uint8_t>(random());
    }
    const auto request = csm::encode_bytes_parameter(
        csm::kRequestAuthenticationChallengeResponse, 0, challenge_);
    if (link_ == nullptr || !link_->send_control(request)) {
      fail("unable to send authentication challenge");
      return;
    }
    if (aa2acp::bridge::debug_logging_enabled())
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::debug)
          << "CSM: sent authentication challenge\n";
    stage_ = Stage::AwaitResponse;
    return;
  }
  if (stage_ == Stage::AwaitResponse) {
    if (message.id == csm::kAuthenticationFailed) {
      fail("authentication rejected");
    } else if (message.id == csm::kAuthenticationResponse) {
      const auto signature = csm::first_bytes_parameter(message.payload, 0);
      if (!signature ||
          !csm::verify_ecdsa_sha256(challenge_, *signature, certificate_)) {
        fail("authentication signature validation failed");
        return;
      }
      send_empty(csm::kAuthenticationSucceeded);
      stage_ = Stage::Done;
      aa2acp::bridge::log(aa2acp::bridge::LogLevel::info)
          << "CSM: identification and software-MFi signature validation "
             "complete\n";
    }
  }
}

} // namespace aa2acp::iap2
