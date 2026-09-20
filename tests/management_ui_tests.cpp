#include "aa2acp/bridge/management_ui.hpp"

#include <cassert>

int main() {
  using aa2acp::bridge::management::frame_http_request;
  using aa2acp::bridge::management::HttpRequestFraming;
  using aa2acp::bridge::management::valid_hotspot_passphrase;

  assert(!valid_hotspot_passphrase("changeme"));
  assert(!valid_hotspot_passphrase("short"));
  assert(!valid_hotspot_passphrase("valid\npassword"));
  assert(!valid_hotspot_passphrase("valid=password"));
  assert(valid_hotspot_passphrase("valid-password"));

  const auto complete = frame_http_request(
      "POST /config HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc", 1024, 1024);
  assert(complete.status == HttpRequestFraming::Status::complete);
  assert(complete.body == "abc");
  assert(frame_http_request(
             "POST /config HTTP/1.1\r\nContent-Length: 3\r\n\r\nab", 1024, 1024)
             .status == HttpRequestFraming::Status::incomplete);
  assert(frame_http_request(
             "POST /config HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcNEXT", 1024,
             1024)
             .status == HttpRequestFraming::Status::invalid);
  assert(frame_http_request("POST / HTTP/1.1\r\nContent-Length: 1\r\n"
                            "Content-Length: 2\r\n\r\na",
                            1024, 1024)
             .status == HttpRequestFraming::Status::invalid);
  assert(frame_http_request("GET / HTTP/1.1\r\n\r\n", 1024, 1024).status ==
         HttpRequestFraming::Status::complete);
}
