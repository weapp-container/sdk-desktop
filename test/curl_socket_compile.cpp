// Compile the actual optional Linux transport with explicit libcurl headers,
// independently of GTK availability. This is not a Linux target/OS acceptance.
#include "device_socket_curl.hpp"
void compile_curl_socket_transport() {
  (void)&weapp::desktop::detail::run_curl_socket;
  (void)&weapp::desktop::detail::curl_socket_available;
}
