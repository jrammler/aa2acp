#include "acp/iap2/link_layer.hpp"
#include "acp/iap2/csm.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

using acp::iap2::Header;
using acp::iap2::Lsp;
using acp::iap2::PhoneLink;
using acp::iap2::State;

void test_header_round_trip() {
    const Header expected{24, acp::iap2::kControlSyn, 100, 42, 10};
    const auto encoded = acp::iap2::encode_header(expected);
    const auto decoded = acp::iap2::decode_header(encoded);
    assert(decoded.has_value());
    assert(decoded->length == expected.length);
    assert(decoded->control == expected.control);
    assert(decoded->sequence == expected.sequence);
    assert(decoded->acknowledgement == expected.acknowledgement);
    assert(decoded->session_id == expected.session_id);
}

void test_lsp_round_trip() {
    const auto encoded = acp::iap2::encode_lsp(Lsp{});
    const auto decoded = acp::iap2::decode_lsp(encoded);
    assert(decoded.has_value());
    assert(decoded->max_len == 65535);
    assert(decoded->sessions.size() == 3);
    assert(decoded->sessions[0].id == acp::iap2::kControlSessionId);
}

void test_negotiation() {
    std::vector<std::vector<std::uint8_t>> writes;
    PhoneLink link([&writes](const std::span<const std::uint8_t> bytes) {
        writes.emplace_back(bytes.begin(), bytes.end());
        return true;
    });
    const auto now = std::chrono::steady_clock::now();
    link.start(now);
    assert(writes.size() == 1);
    assert(writes[0] == std::vector<std::uint8_t>(acp::iap2::kMarker.begin(), acp::iap2::kMarker.end()));

    link.receive(acp::iap2::kMarker, now);
    assert(link.state() == State::Negotiate);
    assert(writes.size() == 2);

    const auto lsp = acp::iap2::encode_lsp(Lsp{});
    const auto syn_header = acp::iap2::encode_header(
        {static_cast<std::uint16_t>(lsp.size() + 10), acp::iap2::kControlSyn, 3, 100, 0});
    std::vector<std::uint8_t> accessory_syn(syn_header.begin(), syn_header.end());
    accessory_syn.insert(accessory_syn.end(), lsp.begin(), lsp.end());
    accessory_syn.push_back(acp::iap2::checksum(lsp));
    link.receive(accessory_syn, now);
    assert(writes.size() == 3);  // Our ACK of the accessory SYN.

    const auto ack = acp::iap2::encode_header({9, acp::iap2::kControlAck, 4, 100, 0});
    link.receive(ack, now);
    assert(link.state() == State::Normal);
}

void test_csm_codec() {
    const std::array<std::uint8_t, 3> challenge{1, 2, 3};
    const auto encoded = acp::iap2::csm::encode_bytes_parameter(
        acp::iap2::csm::kRequestAuthenticationChallengeResponse, 0, challenge);
    acp::iap2::csm::Decoder decoder;
    decoder.push(std::span(encoded).first(5));
    assert(!decoder.next().has_value());
    decoder.push(std::span(encoded).subspan(5));
    const auto decoded = decoder.next();
    assert(decoded.has_value());
    assert(decoded->id == acp::iap2::csm::kRequestAuthenticationChallengeResponse);
    assert(decoded->payload.size() == 7);
    const auto parameter = acp::iap2::csm::first_bytes_parameter(decoded->payload, 0);
    assert(parameter.has_value());
    assert(*parameter == std::vector<std::uint8_t>(challenge.begin(), challenge.end()));
}

int main() {
    test_header_round_trip();
    test_lsp_round_trip();
    test_negotiation();
    test_csm_codec();
    std::cout << "iap2 link-layer tests passed\n";
}
