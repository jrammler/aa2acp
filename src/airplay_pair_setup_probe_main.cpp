#include "acp/airplay/rtsp.hpp"
#include "acp/airplay/srp.hpp"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <iostream>
#include <string>

namespace {

int connect_tcp(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0) {
        return -1;
    }
    int socket_fd = -1;
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd >= 0 && connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        if (socket_fd >= 0) close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    return socket_fd;
}

bool send_all(const int socket_fd, const std::span<const std::uint8_t> bytes) {
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = send(socket_fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "10.10.0.1";
    std::string port = "7000";
    int timeout_seconds = 10;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--host" && index + 1 < argc) host = argv[++index];
        else if (argument == "--port" && index + 1 < argc) port = argv[++index];
        else if (argument == "--timeout" && index + 1 < argc) timeout_seconds = std::stoi(argv[++index]);
        else {
            std::cerr << "usage: airplay-pair-setup-probe [--host HOST] [--port PORT] [--timeout SECONDS]\n";
            return 2;
        }
    }
    const auto socket_fd = connect_tcp(host, port);
    if (socket_fd < 0) {
        std::cerr << "Unable to connect to AirPlay " << host << ':' << port << '\n';
        return 1;
    }
    const auto m1 = acp::airplay::encode_tlv8({{0x06, {1}}, {0x00, {0}}});
    const auto request = acp::airplay::encode_request("POST", "/pair-setup", 1, m1, "application/pairing+tlv8");
    if (!send_all(socket_fd, request)) {
        std::cerr << "Unable to send Pair-Setup M1\n";
        close(socket_fd);
        return 1;
    }
    std::vector<std::uint8_t> response_bytes;
    std::array<std::uint8_t, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline && !acp::airplay::complete_response_size(response_bytes)) {
        pollfd descriptor{socket_fd, POLLIN, 0};
        if (poll(&descriptor, 1, 100) <= 0) continue;
        const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) break;
        response_bytes.insert(response_bytes.end(), buffer.begin(), buffer.begin() + count);
    }
    const auto response = acp::airplay::parse_response(response_bytes);
    if (!response || response->status != 200) {
        std::cerr << "Pair-Setup M1 did not receive RTSP 200\n";
        return 1;
    }
    const auto fields = acp::airplay::decode_tlv8(response->body);
    const auto state = fields.find(0x06);
    const auto salt = fields.find(0x02);
    const auto public_key = fields.find(0x03);
    if (state == fields.end() || state->second != acp::airplay::Bytes{2} || salt == fields.end() ||
        public_key == fields.end()) {
        std::cerr << "Pair-Setup M2 is missing state=2, salt, or SRP public key\n";
        return 1;
    }
    std::cout << "AirPlay: Pair-Setup M2 received (salt=" << salt->second.size()
              << "B, SRP public key=" << public_key->second.size() << "B)\n";
    acp::airplay::SrpClient srp;
    if (!srp.process_challenge(salt->second, public_key->second)) {
        std::cerr << "Unable to process Pair-Setup SRP challenge\n";
        close(socket_fd);
        return 1;
    }
    const auto m3 = acp::airplay::encode_tlv8(
        {{0x06, {3}}, {0x03, srp.public_key()}, {0x04, srp.client_proof()}});
    const auto m3_request = acp::airplay::encode_request("POST", "/pair-setup", 2, m3, "application/pairing+tlv8");
    if (!send_all(socket_fd, m3_request)) {
        std::cerr << "Unable to send Pair-Setup M3\n";
        close(socket_fd);
        return 1;
    }
    response_bytes.clear();
    const auto m4_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < m4_deadline && !acp::airplay::complete_response_size(response_bytes)) {
        pollfd descriptor{socket_fd, POLLIN, 0};
        if (poll(&descriptor, 1, 100) <= 0) continue;
        const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) break;
        response_bytes.insert(response_bytes.end(), buffer.begin(), buffer.begin() + count);
    }
    close(socket_fd);
    const auto m4_response = acp::airplay::parse_response(response_bytes);
    if (!m4_response || m4_response->status != 200) {
        std::cerr << "Pair-Setup M3 did not receive RTSP 200\n";
        return 1;
    }
    const auto m4_fields = acp::airplay::decode_tlv8(m4_response->body);
    const auto m4_state = m4_fields.find(0x06);
    const auto server_proof = m4_fields.find(0x04);
    if (m4_state == m4_fields.end() || m4_state->second != acp::airplay::Bytes{4} ||
        server_proof == m4_fields.end() || !srp.verify_server(server_proof->second)) {
        std::cerr << "Pair-Setup M4 server proof validation failed\n";
        return 1;
    }
    std::cout << "AirPlay: Pair-Setup M4 server proof validated\n";
    return 0;
}
