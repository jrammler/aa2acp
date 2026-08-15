#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace acp::iap2::csm {

constexpr std::uint16_t kStart = 0x4040;
constexpr std::uint16_t kStartIdentification = 0x1d00;
constexpr std::uint16_t kIdentificationInformation = 0x1d01;
constexpr std::uint16_t kIdentificationAccepted = 0x1d02;
constexpr std::uint16_t kIdentificationRejected = 0x1d03;
constexpr std::uint16_t kRequestAuthenticationCertificate = 0xaa00;
constexpr std::uint16_t kAuthenticationCertificate = 0xaa01;
constexpr std::uint16_t kRequestAuthenticationChallengeResponse = 0xaa02;
constexpr std::uint16_t kAuthenticationResponse = 0xaa03;
constexpr std::uint16_t kAuthenticationFailed = 0xaa04;
constexpr std::uint16_t kAuthenticationSucceeded = 0xaa05;

struct Message {
    std::uint16_t id{};
    std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> encode(std::uint16_t id, std::span<const std::uint8_t> payload = {});
std::vector<std::uint8_t> encode_bytes_parameter(std::uint16_t id, std::uint16_t parameter_id,
                                                  std::span<const std::uint8_t> value);
std::optional<std::vector<std::uint8_t>> first_bytes_parameter(std::span<const std::uint8_t> payload,
                                                                std::uint16_t parameter_id);
bool verify_ecdsa_sha256(std::span<const std::uint8_t> challenge,
                         std::span<const std::uint8_t> signature,
                         std::span<const std::uint8_t> certificate_der);

// Incrementally parses complete CSM messages from an iAP2 control-session stream.
class Decoder {
  public:
    void push(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::optional<Message> next();

  private:
    std::vector<std::uint8_t> buffer_;
};

}  // namespace acp::iap2::csm
