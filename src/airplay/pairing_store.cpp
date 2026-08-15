#include "aa2acp/airplay/pairing_store.hpp"

#include <array>
#include <fstream>
#include <sys/stat.h>

namespace aa2acp::airplay {
namespace {

constexpr std::array<char, 8> kMagic{'A', 'C', 'P', 'P', 'A', 'I', 'R', '1'};

void write_u16(std::ostream &stream, const std::uint16_t value) {
  stream.put(static_cast<char>(value));
  stream.put(static_cast<char>(value >> 8));
}

std::optional<std::uint16_t> read_u16(std::istream &stream) {
  const auto low = stream.get();
  const auto high = stream.get();
  if (low == EOF || high == EOF)
    return std::nullopt;
  return static_cast<std::uint16_t>(low | (high << 8));
}

bool read_bytes(std::istream &stream, Bytes &bytes, const std::size_t size) {
  bytes.resize(size);
  stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
  return stream.good();
}

} // namespace

std::optional<PairingRecord>
load_pairing_record(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  std::array<char, kMagic.size()> magic{};
  if (!stream.read(magic.data(), magic.size()) || magic != kMagic)
    return std::nullopt;
  const auto identifier_size = read_u16(stream);
  if (!identifier_size || *identifier_size == 0 || *identifier_size > 1024)
    return std::nullopt;
  PairingRecord record;
  record.controller_id.resize(*identifier_size);
  stream.read(record.controller_id.data(), record.controller_id.size());
  if (!stream.good() ||
      !read_bytes(stream, record.controller.private_key, 32) ||
      !read_bytes(stream, record.controller.public_key, 32) ||
      !read_bytes(stream, record.accessory_public_key, 32))
    return std::nullopt;
  return record;
}

bool save_pairing_record(const std::filesystem::path &path,
                         const PairingRecord &record) {
  if (record.controller_id.empty() || record.controller_id.size() > 1024 ||
      record.controller.private_key.size() != 32 ||
      record.controller.public_key.size() != 32 ||
      record.accessory_public_key.size() != 32)
    return false;
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error)
    return false;
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream)
      return false;
    stream.write(kMagic.data(), kMagic.size());
    write_u16(stream, static_cast<std::uint16_t>(record.controller_id.size()));
    stream.write(record.controller_id.data(), record.controller_id.size());
    stream.write(
        reinterpret_cast<const char *>(record.controller.private_key.data()),
        32);
    stream.write(
        reinterpret_cast<const char *>(record.controller.public_key.data()),
        32);
    stream.write(
        reinterpret_cast<const char *>(record.accessory_public_key.data()), 32);
    if (!stream.good())
      return false;
  }
  if (chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0 ||
      std::rename(temporary.c_str(), path.c_str()) != 0)
    return false;
  return true;
}

} // namespace aa2acp::airplay
