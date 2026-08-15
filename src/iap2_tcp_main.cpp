#include "acp/iap2/link_layer.hpp"
#include "acp/iap2/bootstrap.hpp"
#include "acp/iap2/carplay_probe.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int connect_tcp(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
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
        if (socket_fd >= 0) {
            close(socket_fd);
        }
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    return socket_fd;
}

bool send_all(const int socket_fd, const std::span<const std::uint8_t> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = send(socket_fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::string port = "12346";
    int timeout_seconds = 10;
    bool bootstrap = false;
    bool carplay = false;
    bool wifi_config = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--host" && index + 1 < argc) {
            host = argv[++index];
        } else if (argument == "--port" && index + 1 < argc) {
            port = argv[++index];
        } else if (argument == "--timeout" && index + 1 < argc) {
            timeout_seconds = std::stoi(argv[++index]);
        } else if (argument == "--bootstrap") {
            bootstrap = true;
        } else if (argument == "--carplay") {
            bootstrap = true;
            carplay = true;
        } else if (argument == "--wifi-config") {
            bootstrap = true;
            carplay = true;
            wifi_config = true;
        } else {
            std::cerr << "usage: iap2-tcp [--host HOST] [--port PORT] [--timeout SECONDS] [--bootstrap] [--carplay] [--wifi-config]\n";
            return 2;
        }
    }

    const auto socket_fd = connect_tcp(host, port);
    if (socket_fd < 0) {
        std::cerr << "Unable to connect to " << host << ':' << port << '\n';
        return 1;
    }
    std::cout << "Connected to " << host << ':' << port << '\n';
    acp::iap2::BootstrapSession session;
    acp::iap2::CarPlayProbe carplay_probe;
    acp::iap2::PhoneLink link(
        [socket_fd](const std::span<const std::uint8_t> bytes) { return send_all(socket_fd, bytes); },
        [](const char* message) { std::cout << message << '\n'; },
        [&session, &carplay_probe, &carplay](const std::span<const std::uint8_t> bytes) {
            if (carplay && session.done()) {
                carplay_probe.receive(bytes);
            } else {
                session.receive(bytes);
            }
        });
    session.attach(link);
    carplay_probe.attach(link);
    carplay_probe.request_wifi_configuration(wifi_config);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    link.start(std::chrono::steady_clock::now());

    std::array<std::uint8_t, 1024> buffer{};
    while (std::chrono::steady_clock::now() < deadline && link.state() != acp::iap2::State::Dead &&
           (!bootstrap || (carplay ? (!carplay_probe.done() && !carplay_probe.failed())
                                   : (!session.done() && !session.failed()))) &&
           (bootstrap || link.state() != acp::iap2::State::Normal)) {
        pollfd descriptor{socket_fd, POLLIN, 0};
        const auto result = poll(&descriptor, 1, 100);
        const auto now = std::chrono::steady_clock::now();
        if (result > 0 && (descriptor.revents & POLLIN) != 0) {
            const auto count = recv(socket_fd, buffer.data(), buffer.size(), 0);
            if (count <= 0) {
                std::cerr << "Connection closed by accessory\n";
                break;
            }
            link.receive(std::span(buffer).first(static_cast<std::size_t>(count)), now);
        }
        link.tick(now);
        if (bootstrap && link.state() == acp::iap2::State::Normal && !session.started()) {
            session.begin();
        }
        if (carplay && session.done() && !carplay_probe.started()) {
            carplay_probe.begin();
        }
    }
    close(socket_fd);
    if (carplay) {
        return carplay_probe.done() ? 0 : 1;
    }
    return bootstrap ? (session.done() ? 0 : 1) : (link.state() == acp::iap2::State::Normal ? 0 : 1);
}
