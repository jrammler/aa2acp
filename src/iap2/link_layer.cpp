#include "aa2acp/iap2/link_layer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace aa2acp::iap2 {
namespace {

constexpr std::uint16_t kPacketStart = 0xff5a;

std::uint16_t read_u16(std::span<const std::uint8_t> bytes,
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

std::uint8_t checksum(const std::span<const std::uint8_t> bytes) {
  std::uint8_t sum = 0;
  for (const auto byte : bytes) {
    sum = static_cast<std::uint8_t>(sum + byte);
  }
  return static_cast<std::uint8_t>(0U - sum);
}

bool has_valid_checksum(const std::span<const std::uint8_t> bytes) {
  std::uint8_t sum = 0;
  for (const auto byte : bytes) {
    sum = static_cast<std::uint8_t>(sum + byte);
  }
  return sum == 0;
}

std::vector<std::uint8_t> encode_lsp(const Lsp &lsp) {
  std::vector<std::uint8_t> result(10 + (lsp.sessions.size() * 3));
  result[0] = 1;
  result[1] = lsp.max_outgoing;
  write_u16(result, 2, lsp.max_len);
  write_u16(result, 4, lsp.retransmission_timeout);
  write_u16(result, 6, lsp.ack_timeout);
  result[8] = lsp.max_retransmissions;
  result[9] = lsp.max_ack;
  std::size_t offset = 10;
  for (const auto &session : lsp.sessions) {
    result[offset++] = session.id;
    result[offset++] = session.type;
    result[offset++] = session.version;
  }
  return result;
}

std::optional<Lsp> decode_lsp(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 10 || bytes[0] != 1 || ((bytes.size() - 10) % 3) != 0) {
    return std::nullopt;
  }
  Lsp result;
  result.max_outgoing = bytes[1];
  result.max_len = read_u16(bytes, 2);
  result.retransmission_timeout = read_u16(bytes, 4);
  result.ack_timeout = read_u16(bytes, 6);
  result.max_retransmissions = bytes[8];
  result.max_ack = bytes[9];
  result.sessions.clear();
  for (std::size_t offset = 10; offset < bytes.size(); offset += 3) {
    result.sessions.push_back(
        {bytes[offset], bytes[offset + 1], bytes[offset + 2]});
  }
  return result;
}

std::array<std::uint8_t, 9> encode_header(const Header &header) {
  std::array<std::uint8_t, 9> result{};
  write_u16(result, 0, kPacketStart);
  write_u16(result, 2, header.length);
  result[4] = header.control;
  result[5] = header.sequence;
  result[6] = header.acknowledgement;
  result[7] = header.session_id;
  result[8] = checksum(std::span(result).first(8));
  return result;
}

std::optional<Header>
decode_header(const std::span<const std::uint8_t, 9> bytes) {
  if (!has_valid_checksum(bytes) || read_u16(bytes, 0) != kPacketStart) {
    return std::nullopt;
  }
  const auto length = read_u16(bytes, 2);
  if (length < 9) {
    return std::nullopt;
  }
  return Header{length, bytes[4], bytes[5], bytes[6], bytes[7]};
}

PhoneLink::PhoneLink(SendFunction send, LogFunction log,
                     ControlDataFunction control_data)
    : send_(std::move(send)), log_(std::move(log)),
      control_data_(std::move(control_data)) {}

void PhoneLink::start(const std::chrono::steady_clock::time_point now) {
  if (state_ != State::Detect) {
    return;
  }
  send_marker();
  next_marker_ = now + std::chrono::seconds(1);
  log("iAP2: sent initial marker; waiting for accessory marker");
}

void PhoneLink::receive(const std::span<const std::uint8_t> bytes,
                        const std::chrono::steady_clock::time_point now) {
  receive_buffer_.insert(receive_buffer_.end(), bytes.begin(), bytes.end());

  if (state_ == State::Detect) {
    const auto marker =
        std::search(receive_buffer_.begin(), receive_buffer_.end(),
                    kMarker.begin(), kMarker.end());
    if (marker == receive_buffer_.end()) {
      if (!bytes.empty() && !detect_dump_logged_ && log_ != nullptr) {
        // Dump unrecognized traffic while waiting for the accessory marker,
        // so an incompatible dialect is visible in debug logs.
        std::string hex = "iAP2: received " + std::to_string(bytes.size()) +
                          " byte(s) without marker:";
        constexpr std::size_t kMaxDumpBytes = 32;
        const auto dumped = std::min(bytes.size(), kMaxDumpBytes);
        char byte_hex[4];
        for (std::size_t index = 0; index < dumped; ++index) {
          std::snprintf(byte_hex, sizeof(byte_hex), " %02x", bytes[index]);
          hex += byte_hex;
        }
        if (bytes.size() > kMaxDumpBytes) {
          hex += " ...";
        }
        log_(hex.c_str());
        detect_dump_logged_ = true;
      }
      if (receive_buffer_.size() > kMarker.size() - 1) {
        receive_buffer_.erase(
            receive_buffer_.begin(),
            receive_buffer_.end() -
                static_cast<std::ptrdiff_t>(kMarker.size() - 1));
      }
      return;
    }
    receive_buffer_.erase(receive_buffer_.begin(),
                          marker + static_cast<std::ptrdiff_t>(kMarker.size()));
    state_ = State::Negotiate;
    log("iAP2: accessory marker detected; negotiating LSP");
    send_syn(now);
  }
  process_packets(now);
}

void PhoneLink::tick(const std::chrono::steady_clock::time_point now) {
  if (state_ == State::Detect && now >= next_marker_) {
    send_marker();
    next_marker_ = now + std::chrono::seconds(1);
  }
  if (state_ == State::Negotiate && now >= next_syn_) {
    send_syn(now);
  }
}

bool PhoneLink::send_control(const std::span<const std::uint8_t> payload) {
  if (state_ != State::Normal || payload.empty()) {
    return false;
  }
  ++sent_sequence_;
  write_packet(payload, sent_sequence_, kControlAck, kControlSessionId);
  return true;
}

void PhoneLink::send_marker() { (void)send_(kMarker); }

void PhoneLink::send_syn(const std::chrono::steady_clock::time_point now) {
  if (state_ != State::Negotiate) {
    return;
  }
  const auto payload = encode_lsp(lsp_);
  ++sent_sequence_;
  write_packet(payload, sent_sequence_, kControlSyn);
  next_syn_ = now + std::chrono::milliseconds(500);
}

void PhoneLink::send_ack() { write_packet({}, sent_sequence_, kControlAck); }

void PhoneLink::write_packet(const std::span<const std::uint8_t> payload,
                             const std::uint8_t sequence,
                             const std::uint8_t control,
                             const std::uint8_t session_id) {
  const auto header = encode_header(
      {static_cast<std::uint16_t>(payload.empty() ? 9 : payload.size() + 10),
       control, sequence, last_received_sequence_, session_id});
  std::vector<std::uint8_t> packet(header.begin(), header.end());
  if (!payload.empty()) {
    packet.insert(packet.end(), payload.begin(), payload.end());
    packet.push_back(checksum(payload));
  }
  (void)send_(packet);
}

void PhoneLink::process_packets(
    const std::chrono::steady_clock::time_point now) {
  while (receive_buffer_.size() >= 9) {
    const auto start =
        std::find_if(receive_buffer_.begin(), receive_buffer_.end() - 1,
                     [](const std::uint8_t byte) { return byte == 0xff; });
    if (start == receive_buffer_.end() - 1) {
      receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.end() - 1);
      return;
    }
    if (*(start + 1) != 0x5a) {
      receive_buffer_.erase(receive_buffer_.begin(), start + 1);
      continue;
    }
    if (start != receive_buffer_.begin()) {
      receive_buffer_.erase(receive_buffer_.begin(), start);
      if (receive_buffer_.size() < 9) {
        return;
      }
    }
    std::array<std::uint8_t, 9> header_bytes{};
    std::copy_n(receive_buffer_.begin(), header_bytes.size(),
                header_bytes.begin());
    const auto header = decode_header(header_bytes);
    if (!header) {
      receive_buffer_.erase(receive_buffer_.begin());
      continue;
    }
    if (receive_buffer_.size() < header->length) {
      return;
    }
    std::vector<std::uint8_t> payload;
    if (header->length > 9) {
      const auto payload_and_checksum =
          std::span(receive_buffer_).subspan(9, header->length - 9);
      if (!has_valid_checksum(payload_and_checksum)) {
        receive_buffer_.erase(receive_buffer_.begin(),
                              receive_buffer_.begin() + header->length);
        continue;
      }
      payload.assign(payload_and_checksum.begin(),
                     payload_and_checksum.end() - 1);
    }
    receive_buffer_.erase(receive_buffer_.begin(),
                          receive_buffer_.begin() + header->length);
    handle_packet(*header, payload, now);
  }
}

void PhoneLink::handle_packet(const Header &header,
                              const std::span<const std::uint8_t> payload,
                              const std::chrono::steady_clock::time_point now) {
  if ((header.control & kControlRst) != 0) {
    state_ = State::Dead;
    log("iAP2: accessory sent reset");
    return;
  }
  if ((header.control & kControlSyn) != 0) {
    if (const auto received_lsp = decode_lsp(payload)) {
      lsp_ = *received_lsp;
      last_received_sequence_ = header.sequence;
      send_ack();
    }
  }
  if ((header.control & kControlAck) != 0 && state_ == State::Negotiate) {
    state_ = State::Normal;
    log("iAP2: link established (NORMAL)");
  }
  if ((header.control & ~kControlAck) == 0 && !payload.empty()) {
    last_received_sequence_ = header.sequence;
    if (header.session_id == kControlSessionId && control_data_) {
      control_data_(payload);
    }
    // The production sender will implement cumulative ACK handling. ACKing
    // each control packet is conservative and keeps this bootstrap reliable.
    send_ack();
  }
  (void)now;
}

void PhoneLink::log(const char *message) const {
  if (log_) {
    log_(message);
  }
}

} // namespace aa2acp::iap2
