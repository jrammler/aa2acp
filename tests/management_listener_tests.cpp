#include "aa2acp/bridge/management_listener.hpp"

#include <cassert>

int main() {
  aa2acp::bridge::ManagementListener listener(0, "lo");
  assert(listener.ready());
  assert(listener.fd() >= 0);
  assert(listener.address_changed() == false);
  assert(listener.rebind("lo"));
  assert(listener.ready());
  assert(listener.fd() >= 0);
}
