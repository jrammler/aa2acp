#pragma once

#include <cstdlib>
#include <string_view>

namespace aa2acp::bridge {

inline bool debug_logging_enabled() {
  const auto *level = std::getenv("AA2ACP_LOG_LEVEL");
  return level != nullptr && std::string_view(level) == "debug";
}

} // namespace aa2acp::bridge
