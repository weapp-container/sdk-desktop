#include "device_socket_curl.hpp"
#include <iostream>
#include <limits>
using namespace weapp::desktop::detail;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool feed(CurlSocketHeaders& headers, std::string line) {
  return curl_socket_header(line.data(), 1, line.size(), &headers) == line.size();
}
}
int main() {
  try {
    CurlSocketHeaders valid;
    require(feed(valid, "HTTP/1.1 200 Connection established\r\n") && feed(valid, "X-Proxy: old\r\n"), "proxy headers rejected");
    require(feed(valid, "HTTP/1.1 101 Switching Protocols\r\n") && feed(valid, "Sec-WebSocket-Protocol: chat.v1\r\n"), "upgrade headers rejected");
    require(!valid.headers.contains("x-proxy") && valid.protocol == "chat.v1", "headers crossed handshake boundaries");
    require(!feed(valid, "Sec-WebSocket-Protocol: chat.v1\r\n") && valid.invalid, "duplicate protocol accepted");
    for (const auto& line : {std::string("Sec-WebSocket-Protocol: \r\n"), std::string("X-Test: \xff\r\n"), std::string("X-Test: a\rb\r\n")}) {
      CurlSocketHeaders invalid;
      require(!feed(invalid, line) && invalid.invalid, "invalid header crossed the C callback boundary");
    }
    CurlSocketHeaders overflow;
    require(curl_socket_header(nullptr, 2, std::numeric_limits<size_t>::max(), &overflow) == 0 && overflow.invalid, "header length overflow accepted");
    std::cout << "libcurl response callback: bounded validation passed (no network transfer)\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
