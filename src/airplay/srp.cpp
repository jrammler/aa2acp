#include "aa2acp/airplay/srp.hpp"

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <memory>
#include <string_view>

namespace aa2acp::airplay {
namespace {

constexpr std::size_t kGroupBytes = 384;
constexpr char kPrimeHex[] =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183"
    "995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A"
    "85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7AB"
    "F5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D8"
    "7602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208"
    "E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

using BnPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
using CtxPtr = std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)>;

BnPtr bn_new() { return {BN_new(), BN_free}; }
BnPtr bn_from(std::span<const std::uint8_t> bytes) {
  return {BN_bin2bn(bytes.data(), bytes.size(), nullptr), BN_free};
}

Bytes padded(const BIGNUM *value) {
  Bytes result(kGroupBytes);
  return BN_bn2binpad(value, result.data(), result.size()) ==
                 static_cast<int>(result.size())
             ? result
             : Bytes{};
}

Bytes sha512(const std::initializer_list<std::span<const std::uint8_t>> parts) {
  std::array<std::uint8_t, EVP_MAX_MD_SIZE> digest{};
  unsigned int size{};
  auto context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
      EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha512(), nullptr) != 1)
    return {};
  for (const auto part : parts) {
    if (EVP_DigestUpdate(context.get(), part.data(), part.size()) != 1)
      return {};
  }
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1)
    return {};
  return {digest.begin(), digest.begin() + static_cast<std::ptrdiff_t>(size)};
}

bool equal(std::span<const std::uint8_t> left,
           std::span<const std::uint8_t> right) {
  return left.size() == right.size() &&
         CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

} // namespace

bool SrpClient::process_challenge(
    const std::span<const std::uint8_t> salt,
    const std::span<const std::uint8_t> server_public_key) {
  public_key_.clear();
  client_proof_.clear();
  session_key_.clear();
  if (salt.empty() || server_public_key.size() != kGroupBytes)
    return false;
  BIGNUM *raw_prime = nullptr;
  if (BN_hex2bn(&raw_prime, kPrimeHex) == 0)
    return false;
  BnPtr prime(raw_prime, BN_free);
  auto generator = bn_new();
  auto server = bn_from(server_public_key);
  auto context = CtxPtr(BN_CTX_new(), BN_CTX_free);
  if (!prime || !generator || !server || !context ||
      BN_set_word(generator.get(), 5) != 1)
    return false;
  auto remainder = bn_new();
  if (!remainder ||
      BN_mod(remainder.get(), server.get(), prime.get(), context.get()) != 1 ||
      BN_is_zero(remainder.get()))
    return false;

  std::array<std::uint8_t, 32> private_random{};
  if (RAND_bytes(private_random.data(), private_random.size()) != 1)
    return false;
  auto private_key = bn_from(private_random);
  auto client = bn_new();
  if (!private_key || !client ||
      BN_mod_exp(client.get(), generator.get(), private_key.get(), prime.get(),
                 context.get()) != 1)
    return false;
  public_key_ = padded(client.get());
  const auto padded_prime = padded(prime.get());
  const auto padded_generator = padded(generator.get());
  const auto multiplier_hash = sha512({padded_prime, padded_generator});
  auto multiplier = bn_from(multiplier_hash);

  constexpr std::string_view username = "Pair-Setup";
  constexpr std::string_view password = "3939";
  constexpr std::array<std::uint8_t, 1> colon{':'};
  const auto user_password =
      sha512({std::span(reinterpret_cast<const std::uint8_t *>(username.data()),
                        username.size()),
              colon,
              std::span(reinterpret_cast<const std::uint8_t *>(password.data()),
                        password.size())});
  const auto x_hash = sha512({salt, user_password});
  auto x = bn_from(x_hash);
  auto verifier = bn_new();
  auto u = bn_from(sha512({public_key_, server_public_key}));
  if (!multiplier || !x || !verifier || !u ||
      BN_mod_exp(verifier.get(), generator.get(), x.get(), prime.get(),
                 context.get()) != 1)
    return false;

  auto multiplier_verifier = bn_new();
  auto base = bn_new();
  auto ux = bn_new();
  auto exponent = bn_new();
  auto shared = bn_new();
  if (!multiplier_verifier || !base || !ux || !exponent || !shared ||
      BN_mod_mul(multiplier_verifier.get(), multiplier.get(), verifier.get(),
                 prime.get(), context.get()) != 1 ||
      BN_mod_sub(base.get(), server.get(), multiplier_verifier.get(),
                 prime.get(), context.get()) != 1 ||
      BN_mod_mul(ux.get(), u.get(), x.get(), prime.get(), context.get()) != 1 ||
      BN_mod_add(exponent.get(), private_key.get(), ux.get(), prime.get(),
                 context.get()) != 1 ||
      BN_mod_exp(shared.get(), base.get(), exponent.get(), prime.get(),
                 context.get()) != 1)
    return false;
  session_key_ = sha512({padded(shared.get())});
  const auto h_prime = sha512({padded_prime});
  constexpr std::array<std::uint8_t, 1> raw_generator{5};
  const auto h_generator = sha512({raw_generator});
  Bytes xor_hash(h_prime.size());
  for (std::size_t index = 0; index < xor_hash.size(); ++index)
    xor_hash[index] = h_prime[index] ^ h_generator[index];
  const auto h_username =
      sha512({std::span(reinterpret_cast<const std::uint8_t *>(username.data()),
                        username.size())});
  client_proof_ = sha512({xor_hash, h_username, salt, public_key_,
                          server_public_key, session_key_});
  return client_proof_.size() == 64;
}

bool SrpClient::verify_server(
    const std::span<const std::uint8_t> server_proof) const {
  if (public_key_.empty() || client_proof_.empty() || session_key_.empty())
    return false;
  return equal(server_proof,
               sha512({public_key_, client_proof_, session_key_}));
}

} // namespace aa2acp::airplay
