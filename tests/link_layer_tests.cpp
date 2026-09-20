#include "aa2acp/iap2/csm.hpp"
#include "aa2acp/iap2/link_layer.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

using aa2acp::iap2::Header;
using aa2acp::iap2::Lsp;
using aa2acp::iap2::PhoneLink;
using aa2acp::iap2::State;

void test_header_round_trip() {
  const Header expected{24, aa2acp::iap2::kControlSyn, 100, 42, 10};
  const auto encoded = aa2acp::iap2::encode_header(expected);
  const auto decoded = aa2acp::iap2::decode_header(encoded);
  assert(decoded.has_value());
  assert(decoded->length == expected.length);
  assert(decoded->control == expected.control);
  assert(decoded->sequence == expected.sequence);
  assert(decoded->acknowledgement == expected.acknowledgement);
  assert(decoded->session_id == expected.session_id);
}

void test_lsp_round_trip() {
  const auto encoded = aa2acp::iap2::encode_lsp(Lsp{});
  const auto decoded = aa2acp::iap2::decode_lsp(encoded);
  assert(decoded.has_value());
  assert(decoded->max_len == 65535);
  assert(decoded->sessions.size() == 3);
  assert(decoded->sessions[0].id == aa2acp::iap2::kControlSessionId);
}

void test_negotiation() {
  std::vector<std::vector<std::uint8_t>> writes;
  std::vector<std::uint8_t> received_control;
  PhoneLink link(
      [&writes](const std::span<const std::uint8_t> bytes) {
        writes.emplace_back(bytes.begin(), bytes.end());
        return true;
      },
      {},
      [&received_control](const std::span<const std::uint8_t> bytes) {
        received_control.assign(bytes.begin(), bytes.end());
      });
  const auto now = std::chrono::steady_clock::now();
  link.start(now);
  assert(writes.size() == 2);
  assert(writes[0] == std::vector<std::uint8_t>(aa2acp::iap2::kMarker.begin(),
                                                aa2acp::iap2::kMarker.end()));
  assert(link.state() == State::Negotiate);

  link.receive(aa2acp::iap2::kMarker, now);
  assert(writes.size() == 2);

  auto peer_lsp = Lsp{};
  peer_lsp.max_outgoing = 0;
  peer_lsp.sessions[0].id = 1;
  const auto lsp = aa2acp::iap2::encode_lsp(peer_lsp);
  const auto syn_header =
      aa2acp::iap2::encode_header({static_cast<std::uint16_t>(lsp.size() + 10),
                                   aa2acp::iap2::kControlSyn, 3, 100, 0});
  std::vector<std::uint8_t> accessory_syn(syn_header.begin(), syn_header.end());
  accessory_syn.insert(accessory_syn.end(), lsp.begin(), lsp.end());
  accessory_syn.push_back(aa2acp::iap2::checksum(lsp));
  link.receive(accessory_syn, now);
  assert(writes.size() == 3); // Our SYN|ACK response to the accessory SYN.
  const auto response =
      aa2acp::iap2::decode_header(std::span(writes[2]).first<9>());
  assert(response.has_value());
  assert(response->control ==
         (aa2acp::iap2::kControlSyn | aa2acp::iap2::kControlAck));
  assert(response->acknowledgement == 3);

  const auto ack =
      aa2acp::iap2::encode_header({9, aa2acp::iap2::kControlAck, 4, 100, 0});
  link.receive(ack, now);
  assert(link.state() == State::Normal);

  assert(link.send_control(
      aa2acp::iap2::csm::encode(aa2acp::iap2::csm::kStartIdentification)));
  const auto control_header =
      aa2acp::iap2::decode_header(std::span(writes.back()).first<9>());
  assert(control_header.has_value());
  assert(control_header->session_id == 1);

  const auto identification =
      aa2acp::iap2::csm::encode(aa2acp::iap2::csm::kIdentificationInformation);
  const auto identification_header = aa2acp::iap2::encode_header(
      {static_cast<std::uint16_t>(identification.size() + 10),
       aa2acp::iap2::kControlAck, 4, 101, 1});
  std::vector<std::uint8_t> identification_packet(identification_header.begin(),
                                                  identification_header.end());
  identification_packet.insert(identification_packet.end(),
                               identification.begin(), identification.end());
  identification_packet.push_back(aa2acp::iap2::checksum(identification));
  link.receive(identification_packet, now);
  assert(received_control == identification);

  // Do not advance receive state or deliver a later control message while a
  // sequence is missing; the ACK must name the last contiguous frame.
  const std::array<std::uint8_t, 1> skipped_payload{0x42};
  const auto skipped_header = aa2acp::iap2::encode_header(
      {static_cast<std::uint16_t>(skipped_payload.size() + 10),
       aa2acp::iap2::kControlAck, 6, 101, 1});
  std::vector<std::uint8_t> skipped_packet(skipped_header.begin(),
                                           skipped_header.end());
  skipped_packet.insert(skipped_packet.end(), skipped_payload.begin(),
                        skipped_payload.end());
  skipped_packet.push_back(aa2acp::iap2::checksum(skipped_payload));
  link.receive(skipped_packet, now);
  assert(received_control == identification);
  const auto gap_ack =
      aa2acp::iap2::decode_header(std::span(writes.back()).first<9>());
  assert(gap_ack.has_value());
  assert(gap_ack->control == aa2acp::iap2::kControlAck);
  assert(gap_ack->acknowledgement == 4);
}

void test_csm_codec() {
  const std::vector<std::uint8_t> oversized(65 * 1024);
  assert(aa2acp::iap2::csm::encode(1, oversized).empty());
  assert(aa2acp::iap2::csm::encode_bytes_parameter(1, 2, oversized).empty());
  const std::array<std::uint8_t, 3> challenge{1, 2, 3};
  const auto encoded = aa2acp::iap2::csm::encode_bytes_parameter(
      aa2acp::iap2::csm::kRequestAuthenticationChallengeResponse, 0, challenge);
  aa2acp::iap2::csm::Decoder decoder;
  decoder.push(std::span(encoded).first(5));
  assert(!decoder.next().has_value());
  decoder.push(std::span(encoded).subspan(5));
  const auto decoded = decoder.next();
  assert(decoded.has_value());
  assert(decoded->id ==
         aa2acp::iap2::csm::kRequestAuthenticationChallengeResponse);
  assert(decoded->payload.size() == 7);
  const auto parameter =
      aa2acp::iap2::csm::first_bytes_parameter(decoded->payload, 0);
  assert(parameter.has_value());
  assert(*parameter ==
         std::vector<std::uint8_t>(challenge.begin(), challenge.end()));
}

void test_initial_marker_failure_is_dead() {
  PhoneLink link([](const std::span<const std::uint8_t>) { return false; });
  link.start(std::chrono::steady_clock::now());
  assert(link.state() == State::Dead);
}

void test_send_failure_is_not_queued() {
  std::vector<std::vector<std::uint8_t>> writes;
  bool fail_sends = false;
  PhoneLink link(
      [&writes, &fail_sends](const std::span<const std::uint8_t> bytes) {
        if (fail_sends)
          return false;
        writes.emplace_back(bytes.begin(), bytes.end());
        return true;
      });
  const auto now = std::chrono::steady_clock::now();
  link.start(now);
  link.receive(aa2acp::iap2::kMarker, now);
  const auto lsp = aa2acp::iap2::encode_lsp(Lsp{});
  auto syn = std::vector<std::uint8_t>{};
  const auto header =
      aa2acp::iap2::encode_header({static_cast<std::uint16_t>(lsp.size() + 10),
                                   aa2acp::iap2::kControlSyn, 3, 100, 0});
  syn.assign(header.begin(), header.end());
  syn.insert(syn.end(), lsp.begin(), lsp.end());
  syn.push_back(aa2acp::iap2::checksum(lsp));
  link.receive(syn, now);
  link.receive(
      aa2acp::iap2::encode_header({9, aa2acp::iap2::kControlAck, 4, 100, 0}),
      now);
  assert(link.state() == State::Normal);
  fail_sends = true;
  const std::array<std::uint8_t, 1> payload{1};
  assert(!link.send_control(payload));
  assert(link.state() == State::Dead);
  assert(!link.send_control(payload));
}

int main() {
  test_header_round_trip();
  test_lsp_round_trip();
  test_negotiation();
  test_csm_codec();
  test_initial_marker_failure_is_dead();
  test_send_failure_is_not_queued();
  std::cout << "iap2 link-layer tests passed\n";
}
