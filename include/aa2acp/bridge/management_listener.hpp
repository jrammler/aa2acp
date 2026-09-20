#pragma once

#include <cstdint>
#include <string>

#include <netinet/in.h>

namespace aa2acp::bridge {

class ManagementListener final {
public:
  ManagementListener(std::uint16_t port, std::string interface_name);
  ~ManagementListener();

  ManagementListener(const ManagementListener &) = delete;
  ManagementListener &operator=(const ManagementListener &) = delete;

  [[nodiscard]] bool ready() const { return fd_ >= 0; }
  [[nodiscard]] int fd() const { return fd_; }
  [[nodiscard]] in_addr_t address() const { return address_; }
  [[nodiscard]] const std::string &interface_name() const {
    return interface_name_;
  }
  [[nodiscard]] bool address_changed() const;

  // Rebinds the listener to the interface's current IPv4 address. On
  // failure, the existing listener is retained whenever the new address is
  // different; callers can retry later.
  bool rebind(std::string interface_name);

private:
  int open_listener(const std::string &interface_name,
                    in_addr_t &address) const;

  const std::uint16_t port_;
  int fd_{-1};
  in_addr_t address_{htonl(INADDR_LOOPBACK)};
  std::string interface_name_;
};

} // namespace aa2acp::bridge
