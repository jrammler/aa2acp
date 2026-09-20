#include "aa2acp/bridge/management_listener.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <unistd.h>

#include <optional>

namespace aa2acp::bridge {
namespace {

std::optional<in_addr_t>
ipv4_address_for_interface(const std::string &interface_name) {
  ifaddrs *addresses{};
  if (getifaddrs(&addresses) != 0)
    return std::nullopt;
  std::optional<in_addr_t> result;
  for (auto *entry = addresses; entry != nullptr; entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || interface_name != entry->ifa_name ||
        entry->ifa_addr->sa_family != AF_INET)
      continue;
    result =
        reinterpret_cast<const sockaddr_in *>(entry->ifa_addr)->sin_addr.s_addr;
    break;
  }
  freeifaddrs(addresses);
  return result;
}

} // namespace

ManagementListener::ManagementListener(const std::uint16_t port,
                                       std::string interface_name)
    : port_(port), interface_name_(std::move(interface_name)) {
  in_addr_t address{};
  fd_ = open_listener(interface_name_, address);
  if (fd_ >= 0)
    address_ = address;
}

ManagementListener::~ManagementListener() {
  if (fd_ >= 0)
    close(fd_);
}

bool ManagementListener::address_changed() const {
  if (interface_name_.empty())
    return false;
  if (fd_ < 0)
    return true;
  const auto current = ipv4_address_for_interface(interface_name_);
  return current ? *current != address_ : address_ != htonl(INADDR_ANY);
}

int ManagementListener::open_listener(const std::string &interface_name,
                                      in_addr_t &address) const {
  const int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0)
    return -1;
  int enabled = 1;
  (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled));
  if (!interface_name.empty() &&
      setsockopt(listener, SOL_SOCKET, SO_BINDTODEVICE, interface_name.c_str(),
                 interface_name.size() + 1) != 0) {
    close(listener);
    return -1;
  }
  sockaddr_in socket_address{};
  socket_address.sin_family = AF_INET;
  socket_address.sin_port = htons(port_);
  const auto interface_address = ipv4_address_for_interface(interface_name);
  socket_address.sin_addr.s_addr = interface_address.value_or(
      interface_name.empty() ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY));
  if (bind(listener, reinterpret_cast<const sockaddr *>(&socket_address),
           sizeof(socket_address)) != 0 ||
      listen(listener, 8) != 0) {
    close(listener);
    return -1;
  }
  address = socket_address.sin_addr.s_addr;
  return listener;
}

bool ManagementListener::rebind(std::string interface_name) {
  const auto previous_fd = fd_;
  const auto previous_interface = interface_name_;
  const auto previous_address = address_;

  // Keep the current listener alive while a replacement can be opened. This
  // avoids dropping the management service on transient address/interface
  // changes when both sockets can coexist during the handover.
  in_addr_t replacement_address{};
  bool previous_closed = false;
  auto replacement = open_listener(interface_name, replacement_address);
  if (replacement < 0 && previous_fd >= 0) {
    // The old listener normally owns the port, so retry after closing it. If
    // this second attempt fails, restore the old binding before reporting the
    // rebind failure.
    close(previous_fd);
    previous_closed = true;
    fd_ = -1;
    replacement = open_listener(interface_name, replacement_address);
  }
  if (replacement < 0) {
    if (previous_fd >= 0 && fd_ < 0) {
      in_addr_t restored_address{};
      fd_ = open_listener(previous_interface, restored_address);
      if (fd_ >= 0)
        address_ = restored_address;
      else
        address_ = previous_address;
    }
    return false;
  }
  if (previous_fd >= 0 && !previous_closed && previous_fd != replacement)
    close(previous_fd);
  fd_ = replacement;
  address_ = replacement_address;
  interface_name_ = std::move(interface_name);
  return true;
}

} // namespace aa2acp::bridge
