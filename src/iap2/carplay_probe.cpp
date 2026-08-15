#include "acp/iap2/carplay_probe.hpp"

#include "acp/iap2/link_layer.hpp"

#include <array>
#include <iostream>
#include <vector>

namespace acp::iap2 {
namespace {

constexpr std::uint16_t kCarPlayAvailability = 0x4300;
constexpr std::uint16_t kCarPlayStartSession = 0x4301;

void append_u16(std::vector<std::uint8_t>& output, const std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_parameter(std::vector<std::uint8_t>& output, const std::uint16_t id,
                      const std::span<const std::uint8_t> value) {
    append_u16(output, static_cast<std::uint16_t>(value.size() + 4));
    append_u16(output, id);
    output.insert(output.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> attributes(const bool available, const std::string& identifier) {
    std::vector<std::uint8_t> output;
    const std::array<std::uint8_t, 1> availability{static_cast<std::uint8_t>(available)};
    append_parameter(output, 0, availability);
    if (!identifier.empty()) {
        std::vector<std::uint8_t> name(identifier.begin(), identifier.end());
        name.push_back(0);
        append_parameter(output, 1, name);
    }
    return output;
}

std::string string_parameter(const csm::Message& message, const std::uint16_t id) {
    const auto value = csm::first_bytes_parameter(message.payload, id);
    if (!value) {
        return {};
    }
    const auto end = value->empty() ? value->end() : value->end() - (value->back() == 0 ? 1 : 0);
    return {value->begin(), end};
}

std::uint32_t u32_parameter(const csm::Message& message, const std::uint16_t id) {
    const auto value = csm::first_bytes_parameter(message.payload, id);
    if (!value || value->size() != 4) {
        return 0;
    }
    return (static_cast<std::uint32_t>((*value)[0]) << 24) |
           (static_cast<std::uint32_t>((*value)[1]) << 16) |
           (static_cast<std::uint32_t>((*value)[2]) << 8) | (*value)[3];
}

}  // namespace

CarPlayProbe::CarPlayProbe(std::string bluetooth_identifier)
    : bluetooth_identifier_(std::move(bluetooth_identifier)) {}

void CarPlayProbe::attach(PhoneLink& link) { link_ = &link; }

void CarPlayProbe::begin() {
    if (link_ == nullptr) {
        fail("not attached to iAP2 link");
        return;
    }
    const auto wired = attributes(true, "usb-001");
    std::vector<std::uint8_t> payload;
    append_parameter(payload, 0, wired);
    if (!bluetooth_identifier_.empty()) {
        const auto wireless = attributes(true, bluetooth_identifier_);
        append_parameter(payload, 1, wireless);
    }
    if (!link_->send_control(csm::encode(kCarPlayAvailability, payload))) {
        fail("unable to send CarPlayAvailability");
        return;
    }
    started_ = true;
    std::cout << "CSM: sent CarPlayAvailability (wired";
    std::cout << (bluetooth_identifier_.empty() ? ")\n" : " + wireless)\n");
}

void CarPlayProbe::receive(const std::span<const std::uint8_t> bytes) {
    decoder_.push(bytes);
    while (const auto message = decoder_.next()) {
        handle(*message);
    }
}

bool CarPlayProbe::done() const { return done_; }
bool CarPlayProbe::failed() const { return failed_; }
bool CarPlayProbe::started() const { return started_; }

void CarPlayProbe::fail(const char* message) {
    failed_ = true;
    std::cerr << "CSM: " << message << '\n';
}

void CarPlayProbe::handle(const csm::Message& message) {
    std::cout << "CSM: received 0x" << std::hex << message.id << std::dec << '\n';
    if (message.id == kCarPlayStartSession) {
        const auto port = u32_parameter(message, 2);
        const auto device = string_parameter(message, 3);
        const auto version = string_parameter(message, 5);
        std::cout << "CarPlayStartSession: port=" << port << " device=" << device
                  << " source_version=" << version << '\n';
        done_ = true;
    }
}

}  // namespace acp::iap2
