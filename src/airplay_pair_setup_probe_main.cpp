#include "aa2acp/airplay/session.hpp"

#include <iostream>
#include <string>

int main(int argc, char **argv) {
  aa2acp::airplay::SessionOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--host" && index + 1 < argc)
      options.host = argv[++index];
    else if (argument == "--port" && index + 1 < argc)
      options.port = static_cast<std::uint16_t>(std::stoi(argv[++index]));
    else if (argument == "--timeout" && index + 1 < argc)
      options.timeout_seconds = std::stoi(argv[++index]);
    else if (argument == "--video" && index + 1 < argc)
      options.video_path = argv[++index];
    else if (argument == "--pairing-store" && index + 1 < argc)
      options.pairing_store = argv[++index];
    else {
      std::cerr
          << "usage: aa2acp-airplay-pair-setup-probe [--host HOST] [--port "
             "PORT] [--timeout SECONDS] [--video H264_FILE] "
             "[--pairing-store FILE]\n";
      return 2;
    }
  }
  return aa2acp::airplay::run_session(options);
}
