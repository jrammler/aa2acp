#include "aa2acp/airplay/pairing_store.hpp"

#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include <array>

namespace aa2acp::airplay {
namespace {

constexpr std::array<char, 8> kMagic{'A', 'C', 'P', 'P', 'A', 'I', 'R', '1'};

bool write_u16(const int fd, const std::uint16_t value) {
  const char bytes[2] = {static_cast<char>(value & 0xff),
                         static_cast<char>((value >> 8) & 0xff)};
  std::size_t written = 0;
  while (written < 2) {
    const auto n = ::write(fd, bytes + written, 2 - written);
    if (n <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(n);
  }
  return true;
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
  // The file contains the Ed25519 private key: create it exclusively with
  // owner-only permissions so it is never briefly world-readable.
  const auto temporary = path.string() + ".tmp";
  const int fd =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_TRUNC,
             S_IRUSR | S_IWUSR);
  if (fd < 0) {
    return false;
  }
  const auto close_and_remove = [&]() {
    ::close(fd);
    ::unlink(temporary.c_str());
  };
  const auto write_all = [&](const void *data, std::size_t size) {
    const auto *bytes = static_cast<const char *>(data);
    std::size_t written = 0;
    while (written < size) {
      const auto n = ::write(fd, bytes + written, size - written);
      if (n <= 0) {
        return false;
      }
      written += static_cast<std::size_t>(n);
    }
    return true;
  };
  bool ok =
      write_all(kMagic.data(), kMagic.size()) &&
      write_u16(fd, static_cast<std::uint16_t>(record.controller_id.size())) &&
      write_all(record.controller_id.data(), record.controller_id.size()) &&
      write_all(record.controller.private_key.data(), 32) &&
      write_all(record.controller.public_key.data(), 32) &&
      write_all(record.accessory_public_key.data(), 32) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok) {
    close_and_remove();
    return false;
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    close_and_remove();
    return false;
  }
  return true;
}

} // namespace aa2acp::airplay
