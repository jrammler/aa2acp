#include "acp/airplay/bplist.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace acp::airplay {
namespace {

constexpr std::array<std::uint8_t, 8> magic{'b', 'p', 'l', 'i',
                                            's', 't', '0', '0'};

void append_be(Bytes &target, std::uint64_t value, const std::size_t size) {
  for (std::size_t index = size; index-- > 0;) {
    target.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
  }
}

std::optional<std::uint64_t> read_be(const std::span<const std::uint8_t> bytes,
                                     const std::size_t offset,
                                     const std::size_t size) {
  if (size > 8 || offset > bytes.size() || bytes.size() - offset < size)
    return std::nullopt;
  std::uint64_t result{};
  for (std::size_t index = 0; index < size; ++index)
    result = (result << 8) | bytes[offset + index];
  return result;
}

std::size_t bytes_for(const std::uint64_t value) {
  if (value <= 0xff)
    return 1;
  if (value <= 0xffff)
    return 2;
  if (value <= 0xffffffff)
    return 4;
  return 8;
}

std::uint8_t log2_bytes(const std::size_t size) {
  return size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
}

Bytes count_marker(const std::uint8_t type, const std::size_t count) {
  if (count < 15)
    return {static_cast<std::uint8_t>((type << 4) | count)};
  const auto size = bytes_for(count);
  Bytes output{static_cast<std::uint8_t>((type << 4) | 15),
               static_cast<std::uint8_t>(0x10 | log2_bytes(size))};
  append_be(output, count, size);
  return output;
}

Bytes integer(const std::uint64_t value) {
  const auto size = bytes_for(value);
  Bytes output{static_cast<std::uint8_t>(0x10 | log2_bytes(size))};
  append_be(output, value, size);
  return output;
}

struct Node {
  Bytes bytes;
  std::vector<std::size_t> refs;
};

class Encoder {
public:
  std::size_t add(const PlistValue &value) {
    const auto index = nodes_.size();
    nodes_.emplace_back();
    std::visit([this, index](const auto &item) { encode(index, item); },
               value.data);
    return index;
  }

  Bytes finish(const std::size_t top) const {
    const auto ref_size = bytes_for(nodes_.size() - 1);
    Bytes result(magic.begin(), magic.end());
    std::vector<std::size_t> offsets;
    for (const auto &node : nodes_) {
      offsets.push_back(result.size());
      result.insert(result.end(), node.bytes.begin(), node.bytes.end());
      for (const auto ref : node.refs)
        append_be(result, ref, ref_size);
    }
    const auto offset_table = result.size();
    const auto offset_size = bytes_for(offset_table);
    for (const auto offset : offsets)
      append_be(result, offset, offset_size);
    result.insert(result.end(), 6, 0);
    result.push_back(static_cast<std::uint8_t>(offset_size));
    result.push_back(static_cast<std::uint8_t>(ref_size));
    append_be(result, nodes_.size(), 8);
    append_be(result, top, 8);
    append_be(result, offset_table, 8);
    return result;
  }

private:
  void encode(const std::size_t index, const bool value) {
    nodes_[index].bytes = {static_cast<std::uint8_t>(value ? 0x09 : 0x08)};
  }
  void encode(const std::size_t index, const std::uint64_t value) {
    nodes_[index].bytes = integer(value);
  }
  void encode(const std::size_t index, const std::string &value) {
    auto header = count_marker(0x5, value.size());
    header.insert(header.end(), value.begin(), value.end());
    nodes_[index].bytes = std::move(header);
  }
  void encode(const std::size_t index, const Bytes &value) {
    auto header = count_marker(0x4, value.size());
    header.insert(header.end(), value.begin(), value.end());
    nodes_[index].bytes = std::move(header);
  }
  void encode(const std::size_t index, const PlistValue::Array &value) {
    std::vector<std::size_t> refs;
    refs.reserve(value.size());
    for (const auto &item : value)
      refs.push_back(add(item));
    auto header = count_marker(0xa, refs.size());
    nodes_[index].bytes = std::move(header);
    nodes_[index].refs = std::move(refs);
  }
  void encode(const std::size_t index, const PlistValue::Dictionary &value) {
    std::vector<std::size_t> refs;
    refs.reserve(value.size() * 2);
    for (const auto &[key, unused] : value) {
      static_cast<void>(unused);
      refs.push_back(add(PlistValue(key)));
    }
    for (const auto &[unused, item] : value) {
      static_cast<void>(unused);
      refs.push_back(add(item));
    }
    auto header = count_marker(0xd, value.size());
    nodes_[index].bytes = std::move(header);
    nodes_[index].refs = std::move(refs);
  }

  std::vector<Node> nodes_;
};

class Decoder {
public:
  explicit Decoder(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  std::optional<PlistValue> decode() {
    if (bytes_.size() < 40 ||
        !std::equal(magic.begin(), magic.end(), bytes_.begin()))
      return std::nullopt;
    const auto trailer = bytes_.size() - 32;
    offset_size_ = bytes_[trailer + 6];
    ref_size_ = bytes_[trailer + 7];
    const auto count = read_be(bytes_, trailer + 8, 8);
    const auto top = read_be(bytes_, trailer + 16, 8);
    const auto table = read_be(bytes_, trailer + 24, 8);
    if (!count || !top || !table || *count == 0 || *top >= *count ||
        offset_size_ == 0 || ref_size_ == 0 || *count > 65536 ||
        *table > bytes_.size() ||
        *count > (bytes_.size() - *table) / offset_size_)
      return std::nullopt;
    offsets_.reserve(*count);
    for (std::size_t index = 0; index < *count; ++index) {
      const auto offset =
          read_be(bytes_, *table + index * offset_size_, offset_size_);
      if (!offset || *offset >= *table)
        return std::nullopt;
      offsets_.push_back(*offset);
    }
    return object(*top);
  }

private:
  std::optional<std::pair<std::size_t, std::size_t>>
  count(const std::size_t offset, const std::uint8_t nibble) const {
    if (nibble != 15)
      return std::pair{static_cast<std::size_t>(nibble), offset + 1};
    if (offset + 2 > bytes_.size() || (bytes_[offset + 1] >> 4) != 1)
      return std::nullopt;
    const auto size = std::size_t{1} << (bytes_[offset + 1] & 15);
    const auto value = read_be(bytes_, offset + 2, size);
    if (!value || *value > std::numeric_limits<std::size_t>::max())
      return std::nullopt;
    return std::pair{static_cast<std::size_t>(*value), offset + 2 + size};
  }

  std::optional<PlistValue> object(const std::size_t index) const {
    if (index >= offsets_.size())
      return std::nullopt;
    const auto offset = offsets_[index];
    const auto marker = bytes_[offset];
    const auto type = marker >> 4;
    const auto nibble = marker & 15;
    if (type == 0 && (nibble == 8 || nibble == 9))
      return PlistValue(nibble == 9);
    if (type == 1) {
      const auto size = std::size_t{1} << nibble;
      const auto value = read_be(bytes_, offset + 1, size);
      return value ? std::optional<PlistValue>(PlistValue(*value))
                   : std::nullopt;
    }
    const auto parsed_count = count(offset, nibble);
    if (!parsed_count)
      return std::nullopt;
    const auto [items, data_offset] = *parsed_count;
    if ((type == 4 || type == 5) && data_offset <= bytes_.size() &&
        items <= bytes_.size() - data_offset) {
      if (type == 4)
        return PlistValue(Bytes(bytes_.begin() + data_offset,
                                bytes_.begin() + data_offset + items));
      return PlistValue(std::string(bytes_.begin() + data_offset,
                                    bytes_.begin() + data_offset + items));
    }
    if ((type == 0xa || type == 0xd) && data_offset <= bytes_.size() &&
        items <= (bytes_.size() - data_offset) / ref_size_ &&
        (type != 0xd ||
         items <= (bytes_.size() - data_offset) / (2 * ref_size_))) {
      std::vector<std::size_t> refs;
      const auto ref_count = type == 0xd ? items * 2 : items;
      for (std::size_t item = 0; item < ref_count; ++item) {
        const auto ref =
            read_be(bytes_, data_offset + item * ref_size_, ref_size_);
        if (!ref || *ref >= offsets_.size())
          return std::nullopt;
        refs.push_back(*ref);
      }
      if (type == 0xa) {
        PlistValue::Array result;
        for (const auto ref : refs) {
          const auto value = object(ref);
          if (!value)
            return std::nullopt;
          result.push_back(*value);
        }
        return PlistValue(std::move(result));
      }
      PlistValue::Dictionary result;
      for (std::size_t item = 0; item < items; ++item) {
        const auto key = object(refs[item]);
        const auto value = object(refs[items + item]);
        const auto key_string =
            key ? std::get_if<std::string>(&key->data) : nullptr;
        if (!key_string || !value)
          return std::nullopt;
        result.emplace(*key_string, *value);
      }
      return PlistValue(std::move(result));
    }
    return std::nullopt;
  }

  std::span<const std::uint8_t> bytes_;
  std::size_t offset_size_{};
  std::size_t ref_size_{};
  std::vector<std::size_t> offsets_;
};

} // namespace

Bytes encode_bplist(const PlistValue &root) {
  Encoder encoder;
  return encoder.finish(encoder.add(root));
}

std::optional<PlistValue>
decode_bplist(const std::span<const std::uint8_t> bytes) {
  return Decoder(bytes).decode();
}

} // namespace acp::airplay
