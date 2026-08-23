#include "aa2acp/airplay/crypto.hpp"
#include <algorithm>

#include <openssl/evp.h>
#include <openssl/kdf.h>

#include <array>
#include <memory>

namespace aa2acp::airplay {
namespace {

using PkeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using Pkey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using CipherContext =
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

std::array<unsigned char, 12> nonce_for_label(const std::string_view label) {
  std::array<unsigned char, 12> nonce{};
  for (std::size_t index = 0; index < label.size() && index < 8; ++index) {
    nonce[4 + index] = static_cast<unsigned char>(label[index]);
  }
  return nonce;
}

} // namespace

Bytes hkdf_sha512(const std::span<const std::uint8_t> ikm,
                  const std::string_view salt, const std::string_view info,
                  const std::size_t length) {
  Bytes output(length);
  PkeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr),
                      EVP_PKEY_CTX_free);
  std::size_t output_length = length;
  if (!context || EVP_PKEY_derive_init(context.get()) != 1 ||
      EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha512()) != 1 ||
      EVP_PKEY_CTX_set1_hkdf_salt(
          context.get(), reinterpret_cast<const unsigned char *>(salt.data()),
          salt.size()) != 1 ||
      EVP_PKEY_CTX_set1_hkdf_key(context.get(), ikm.data(), ikm.size()) != 1 ||
      EVP_PKEY_CTX_add1_hkdf_info(
          context.get(), reinterpret_cast<const unsigned char *>(info.data()),
          info.size()) != 1 ||
      EVP_PKEY_derive(context.get(), output.data(), &output_length) != 1) {
    return {};
  }
  output.resize(output_length);
  return output;
}

std::optional<Bytes> seal(const std::span<const std::uint8_t> key,
                          const std::string_view label,
                          const std::span<const std::uint8_t> plaintext) {
  return seal_with_nonce(key, nonce_for_label(label), plaintext);
}

std::optional<Bytes>
seal_with_nonce(const std::span<const std::uint8_t> key,
                const std::span<const std::uint8_t> nonce,
                const std::span<const std::uint8_t> plaintext,
                const std::span<const std::uint8_t> aad) {
  if (key.size() != 32 || nonce.size() != 12)
    return std::nullopt;
  CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  Bytes output(plaintext.size() + 16);
  int encrypted_length = 0;
  int final_length = 0;
  if (!context ||
      EVP_EncryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr,
                         key.data(), nonce.data()) != 1 ||
      (!aad.empty() &&
       EVP_EncryptUpdate(context.get(), nullptr, &encrypted_length, aad.data(),
                         aad.size()) != 1) ||
      EVP_EncryptUpdate(context.get(), output.data(), &encrypted_length,
                        plaintext.data(), plaintext.size()) != 1 ||
      EVP_EncryptFinal_ex(context.get(), output.data() + encrypted_length,
                          &final_length) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_GET_TAG, 16,
                          output.data() + encrypted_length + final_length) != 1)
    return std::nullopt;
  output.resize(encrypted_length + final_length + 16);
  return output;
}

std::optional<Bytes> open(const std::span<const std::uint8_t> key,
                          const std::string_view label,
                          const std::span<const std::uint8_t> ciphertext) {
  return open_with_nonce(key, nonce_for_label(label), ciphertext);
}

std::optional<Bytes>
open_with_nonce(const std::span<const std::uint8_t> key,
                const std::span<const std::uint8_t> nonce,
                const std::span<const std::uint8_t> ciphertext,
                const std::span<const std::uint8_t> aad) {
  if (key.size() != 32 || nonce.size() != 12 || ciphertext.size() < 16)
    return std::nullopt;
  CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  const auto encrypted_size = ciphertext.size() - 16;
  Bytes output(encrypted_size);
  int plaintext_length = 0;
  int final_length = 0;
  if (!context ||
      EVP_DecryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr,
                         key.data(), nonce.data()) != 1 ||
      (!aad.empty() &&
       EVP_DecryptUpdate(context.get(), nullptr, &plaintext_length, aad.data(),
                         aad.size()) != 1) ||
      EVP_DecryptUpdate(context.get(), output.data(), &plaintext_length,
                        ciphertext.data(), encrypted_size) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_TAG, 16,
                          const_cast<std::uint8_t *>(ciphertext.data() +
                                                     encrypted_size)) != 1 ||
      EVP_DecryptFinal_ex(context.get(), output.data() + plaintext_length,
                          &final_length) != 1)
    return std::nullopt;
  output.resize(plaintext_length + final_length);
  return output;
}

std::optional<Ed25519> ed25519_generate() {
  PkeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr),
                      EVP_PKEY_CTX_free);
  EVP_PKEY *raw_key = nullptr;
  if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
      EVP_PKEY_keygen(context.get(), &raw_key) != 1)
    return std::nullopt;
  Pkey key(raw_key, EVP_PKEY_free);
  Ed25519 result;
  std::size_t length = 32;
  result.private_key.resize(length);
  result.public_key.resize(length);
  if (EVP_PKEY_get_raw_private_key(key.get(), result.private_key.data(),
                                   &length) != 1)
    return std::nullopt;
  result.private_key.resize(length);
  length = 32;
  if (EVP_PKEY_get_raw_public_key(key.get(), result.public_key.data(),
                                  &length) != 1)
    return std::nullopt;
  result.public_key.resize(length);
  return result;
}

std::optional<Bytes>
ed25519_sign(const std::span<const std::uint8_t> private_key,
             const std::span<const std::uint8_t> data) {
  Pkey key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                        private_key.data(), private_key.size()),
           EVP_PKEY_free);
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  Bytes signature(64);
  std::size_t length = signature.size();
  if (!key || !context ||
      EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) !=
          1 ||
      EVP_DigestSign(context.get(), signature.data(), &length, data.data(),
                     data.size()) != 1)
    return std::nullopt;
  signature.resize(length);
  return signature;
}

bool ed25519_verify(const std::span<const std::uint8_t> public_key,
                    const std::span<const std::uint8_t> data,
                    const std::span<const std::uint8_t> signature) {
  Pkey key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                       public_key.data(), public_key.size()),
           EVP_PKEY_free);
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  return key && context &&
         EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr,
                              key.get()) == 1 &&
         EVP_DigestVerify(context.get(), signature.data(), signature.size(),
                          data.data(), data.size()) == 1;
}

std::optional<X25519> x25519_generate() {
  PkeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr),
                      EVP_PKEY_CTX_free);
  EVP_PKEY *raw_key = nullptr;
  if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
      EVP_PKEY_keygen(context.get(), &raw_key) != 1)
    return std::nullopt;
  Pkey key(raw_key, EVP_PKEY_free);
  X25519 result;
  std::size_t length = 32;
  result.private_key.resize(length);
  result.public_key.resize(length);
  if (EVP_PKEY_get_raw_private_key(key.get(), result.private_key.data(),
                                   &length) != 1)
    return std::nullopt;
  result.private_key.resize(length);
  length = 32;
  if (EVP_PKEY_get_raw_public_key(key.get(), result.public_key.data(),
                                  &length) != 1)
    return std::nullopt;
  result.public_key.resize(length);
  return result;
}

std::optional<Bytes>
x25519_shared(const std::span<const std::uint8_t> private_key,
              const std::span<const std::uint8_t> public_key) {
  Pkey private_pkey(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                                 private_key.data(),
                                                 private_key.size()),
                    EVP_PKEY_free);
  Pkey public_pkey(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                               public_key.data(),
                                               public_key.size()),
                   EVP_PKEY_free);
  PkeyContext context(EVP_PKEY_CTX_new(private_pkey.get(), nullptr),
                      EVP_PKEY_CTX_free);
  Bytes shared(32);
  std::size_t length = shared.size();
  if (!private_pkey || !public_pkey || !context ||
      EVP_PKEY_derive_init(context.get()) != 1 ||
      EVP_PKEY_derive_set_peer(context.get(), public_pkey.get()) != 1 ||
      EVP_PKEY_derive(context.get(), shared.data(), &length) != 1)
    return std::nullopt;
  shared.resize(length);
  // RFC 7748 §6.1: reject an all-zero shared secret, which indicates a
  // low-order peer point and a failed contribution to the key.
  const auto all_zero =
      std::all_of(shared.begin(), shared.end(),
                  [](const std::uint8_t byte) { return byte == 0; });
  if (all_zero)
    return std::nullopt;
  return shared;
}

} // namespace aa2acp::airplay
