#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace aa2acp::iap2 {

constexpr std::array<std::uint8_t, 6> kMarker{0xff, 0x55, 0x02,
                                              0x00, 0xee, 0x10};
constexpr std::uint8_t kControlSyn = 0x80;
constexpr std::uint8_t kControlAck = 0x40;
constexpr std::uint8_t kControlEak = 0x20;
constexpr std::uint8_t kControlRst = 0x10;
constexpr std::uint8_t kControlSessionId = 10;

enum class State : std::uint8_t { Detect, Negotiate, Normal, Dead };

struct Session {
  std::uint8_t id;
  std::uint8_t type;
  std::uint8_t version;
};

struct Lsp {
  std::uint8_t max_outgoing{30};
  std::uint16_t max_len{65535};
  std::uint16_t retransmission_timeout{4000};
  std::uint16_t ack_timeout{500};
  std::uint8_t max_retransmissions{4};
  std::uint8_t max_ack{3};
  std::vector<Session> sessions{
      {kControlSessionId, 0, 2}, {11, 2, 1}, {12, 1, 2}};
};

struct Header {
  std::uint16_t length{};
  std::uint8_t control{};
  std::uint8_t sequence{};
  std::uint8_t acknowledgement{};
  std::uint8_t session_id{};
};

using SendFunction = std::function<bool(std::span<const std::uint8_t>)>;
using LogFunction = std::function<void(const char *)>;
using ControlDataFunction = std::function<void(std::span<const std::uint8_t>)>;

std::uint8_t checksum(std::span<const std::uint8_t> bytes);
bool has_valid_checksum(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_lsp(const Lsp &lsp);
std::optional<Lsp> decode_lsp(std::span<const std::uint8_t> bytes);
std::array<std::uint8_t, 9> encode_header(const Header &header);
std::optional<Header> decode_header(std::span<const std::uint8_t, 9> bytes);

// A portable, phone-side iAP2 negotiation implementation. The caller owns the
// transport and calls receive() for bytes read from it and tick() periodically.
class PhoneLink {
public:
  explicit PhoneLink(SendFunction send, LogFunction log = {},
                     ControlDataFunction control_data = {});

  void start(std::chrono::steady_clock::time_point now);
  void receive(std::span<const std::uint8_t> bytes,
               std::chrono::steady_clock::time_point now);
  void tick(std::chrono::steady_clock::time_point now);
  bool send_control(std::span<const std::uint8_t> payload);

  [[nodiscard]] State state() const { return state_; }
  [[nodiscard]] const Lsp &lsp() const { return lsp_; }

private:
  void send_marker();
  void send_syn(std::chrono::steady_clock::time_point now);
  void send_ack();
  void write_packet(std::span<const std::uint8_t> payload,
                    std::uint8_t sequence, std::uint8_t control,
                    std::uint8_t session_id = 0);
  void process_packets(std::chrono::steady_clock::time_point now);
  void handle_packet(const Header &header,
                     std::span<const std::uint8_t> payload,
                     std::chrono::steady_clock::time_point now);
  void log(const char *message) const;

  SendFunction send_;
  LogFunction log_;
  ControlDataFunction control_data_;
  State state_{State::Detect};
  bool detect_dump_logged_{false};
  Lsp lsp_{};
  std::uint8_t sent_sequence_{99};
  std::uint8_t last_received_sequence_{};
  std::vector<std::uint8_t> receive_buffer_;
  std::chrono::steady_clock::time_point next_marker_{};
  std::chrono::steady_clock::time_point next_syn_{};
};

} // namespace aa2acp::iap2
