#include "aa2acp/iap2/csm.hpp"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>

namespace aa2acp::iap2::csm {
namespace {

std::uint16_t read_u16(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1]);
}

void write_u16(std::span<std::uint8_t> bytes, const std::size_t offset,
               const std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value >> 8U);
  bytes[offset + 1] = static_cast<std::uint8_t>(value & 0xffU);
}

} // namespace

std::vector<std::uint8_t> encode(const std::uint16_t id,
                                 const std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> result(6 + payload.size());
  write_u16(result, 0, kStart);
  write_u16(result, 2, static_cast<std::uint16_t>(result.size()));
  write_u16(result, 4, id);
  std::copy(payload.begin(), payload.end(), result.begin() + 6);
  return result;
}

std::vector<std::uint8_t>
encode_bytes_parameter(const std::uint16_t id, const std::uint16_t parameter_id,
                       const std::span<const std::uint8_t> value) {
  std::vector<std::uint8_t> payload(4 + value.size());
  write_u16(payload, 0, static_cast<std::uint16_t>(payload.size()));
  write_u16(payload, 2, parameter_id);
  std::copy(value.begin(), value.end(), payload.begin() + 4);
  return encode(id, payload);
}

std::optional<std::vector<std::uint8_t>>
first_bytes_parameter(const std::span<const std::uint8_t> payload,
                      const std::uint16_t parameter_id) {
  std::size_t offset = 0;
  while (offset + 4 <= payload.size()) {
    const auto length = read_u16(payload, offset);
    const auto id = read_u16(payload, offset + 2);
    if (length < 4 || offset + length > payload.size()) {
      return std::nullopt;
    }
    if (id == parameter_id) {
      return std::vector<std::uint8_t>(
          payload.begin() + static_cast<std::ptrdiff_t>(offset + 4),
          payload.begin() + static_cast<std::ptrdiff_t>(offset + length));
    }
    offset += length;
  }
  return std::nullopt;
}

bool verify_mfi_v2_signature(
    const std::span<const std::uint8_t> challenge,
    const std::span<const std::uint8_t> signature,
    const std::span<const std::uint8_t> certificate_der) {
  const auto *certificate_ptr = certificate_der.data();
  X509 *certificate = d2i_X509(nullptr, &certificate_ptr,
                               static_cast<long>(certificate_der.size()));
  if (certificate == nullptr) {
    return false;
  }
  EVP_PKEY *key = X509_get_pubkey(certificate);
  X509_free(certificate);
  if (key == nullptr) {
    return false;
  }
  if (EVP_PKEY_base_id(key) != EVP_PKEY_RSA) {
    EVP_PKEY_free(key);
    return false;
  }
  std::array<std::uint8_t, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  const auto hashed =
      EVP_Digest(challenge.data(), challenge.size(), digest.data(),
                 &digest_size, EVP_sha1(), nullptr) == 1;
  EVP_PKEY_CTX *context = EVP_PKEY_CTX_new(key, nullptr);
  std::array<std::uint8_t, EVP_MAX_MD_SIZE + 16> recovered{};
  std::size_t recovered_size = recovered.size();
  const auto initialized =
      context != nullptr && EVP_PKEY_verify_recover_init(context) == 1 &&
      EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PADDING) == 1;
  const auto recovered_signature =
      hashed && initialized &&
      EVP_PKEY_verify_recover(context, recovered.data(), &recovered_size,
                              signature.data(), signature.size()) == 1;
  EVP_PKEY_CTX_free(context);
  EVP_PKEY_free(key);
  if (!recovered_signature) {
    return false;
  }
  if (recovered_size == digest_size &&
      std::equal(digest.begin(), digest.begin() + digest_size,
                 recovered.begin())) {
    return true;
  }
  constexpr std::array<std::uint8_t, 15> kSha1DigestInfoPrefix{
      0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
      0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};
  return recovered_size == kSha1DigestInfoPrefix.size() + digest_size &&
         std::equal(kSha1DigestInfoPrefix.begin(), kSha1DigestInfoPrefix.end(),
                    recovered.begin()) &&
         std::equal(digest.begin(), digest.begin() + digest_size,
                    recovered.begin() + kSha1DigestInfoPrefix.size());
}

void Decoder::push(const std::span<const std::uint8_t> bytes) {
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

std::optional<Message> Decoder::next() {
  while (buffer_.size() >= 6) {
    std::size_t start = 0;
    while (start + 1 < buffer_.size() &&
           (buffer_[start] != static_cast<std::uint8_t>(kStart >> 8U) ||
            buffer_[start + 1] != static_cast<std::uint8_t>(kStart & 0xffU))) {
      ++start;
    }
    if (start + 1 == buffer_.size()) {
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(start));
      return std::nullopt;
    }
    if (start != 0) {
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(start));
      continue;
    }
    const auto length = read_u16(buffer_, 2);
    if (length < 6) {
      buffer_.erase(buffer_.begin());
      continue;
    }
    if (buffer_.size() < length) {
      return std::nullopt;
    }
    Message result{read_u16(buffer_, 4),
                   {buffer_.begin() + 6, buffer_.begin() + length}};
    buffer_.erase(buffer_.begin(), buffer_.begin() + length);
    return result;
  }
  return std::nullopt;
}

} // namespace aa2acp::iap2::csm
