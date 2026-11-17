#include "device_socket_common.hpp"
#include <iostream>
using namespace weapp::desktop;
using namespace weapp::desktop::detail;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejected(Action action) {
  bool failed = false; try { action(); } catch (const std::exception&) { failed = true; }
  require(failed, "invalid socket operation was accepted");
}
void strict_options() {
  auto valid = socket_options({{"url", "wss://EXAMPLE.com:443/path?q=1"}, {"protocols", {"chat.v1"}}, {"header", {{"Authorization", "Bearer local"}}}}, {"https://example.com"});
  require(valid.url == "wss://example.com/path?q=1", "socket URL was not canonicalized");
  for (auto url : {"ws://example.com/", "wss://user@example.com/", "wss://example.com/#fragment", "wss://example.com:0/", "wss://example.com:65536/", "wss://example.com\\other/"})
    rejected([&] { socket_options({{"url", url}}, {"https://example.com"}); });
  rejected([&] { socket_options({{"url", "wss://foreign.example/"}}, {"https://example.com"}); });
  for (auto header : {Json{{"Connection", "upgrade"}}, Json{{"Sec-WebSocket-Protocol", "chat"}}, Json{{"X-Test", "bad\r\nInjected: yes"}}, Json{{"X-Test", "one"}, {"x-test", "two"}}})
    rejected([&] { socket_options({{"url", "wss://example.com/"}, {"header", header}}, {"https://example.com"}); });
  rejected([&] { socket_options({{"url", "wss://example.com/"}, {"protocols", {"chat", "chat"}}}, {"https://example.com"}); });
  rejected([&] { socket_selected_protocol(valid, "unoffered"); });
  socket_selected_protocol(valid, "chat.v1");
}
void bounded_messages_and_fragments() {
  auto queue = std::make_shared<DeviceQueue>();
  NativeSocket socket(queue, "socket");
  auto options = socket_options({{"url", "wss://example.com/"}}, {"https://example.com"});
  socket.opened(options, "");
  SocketMessage message;
  const uint8_t first[] = {0, 127}, second[] = {255};
  message.append(socket, true, first, sizeof(first), false);
  message.append(socket, true, second, sizeof(second), true);
  require(queue->items.back().value.at("data").at("__weappBinary").at("bytes") == Json({0, 127, 255}), "binary fragments changed bytes");
  const uint8_t invalid[] = {255};
  rejected([&] { SocketMessage text; text.append(socket, false, invalid, 1, true); });
  rejected([&] { SocketMessage mixed; mixed.append(socket, true, first, 1, false); mixed.append(socket, false, first, 1, true); });
  rejected([&] { socket.enqueue("large", {{"data", std::string(socket_message_limit + 1, 'x')}}, false); });
  for (size_t index = 0; index < 4; ++index) socket.enqueue(std::to_string(index), {{"data", std::string(socket_message_limit, 'x')}}, false);
  rejected([&] { socket.enqueue("overflow", {{"data", "x"}}, false); });
  while (socket.take()) {}
  rejected([&] { socket.enqueue("close", {{"code", 1006}}, true); });
  rejected([&] { socket.enqueue("close", {{"reason", std::string(124, 'x')}}, true); });
  socket.enqueue("close", {{"code", 1000}}, true);
  rejected([&] { socket.enqueue("late", {{"data", "x"}}, false); });
}
void ingress_backpressure_preserves_terminal_events() {
  auto queue = std::make_shared<DeviceQueue>();
  NativeSocket socket(queue, "socket");
  while (queue_event(queue, "Other", "", Json::object())) {}
  require(queue->items.size() == 512, "event ingress did not stop at budget");
  rejected([&] { socket.message(false, {'h', 'i'}); });
  socket.finish(1013, "backpressure", "socket ingress backpressure limit exceeded");
  require(queue->items.size() == 514, "terminal events lost reserved queue capacity");
  require(queue->items.at(512).event == "Error" && queue->items.at(513).event == "Close", "terminal ordering changed");
}
void terminal_unicode_and_peer_codes() {
  auto queue = std::make_shared<DeviceQueue>();
  NativeSocket socket(queue, "socket");
  socket.finish(1000, std::string(122, 'a') + "\xe2\x82\xac", std::string(511, 'b') + "\xe2\x82\xac");
  require(queue->items.at(0).kind == DeviceQueued::NativeTerminalEvent &&
      queue->items.at(0).value.at("errMsg") == std::string(511, 'b'), "diagnostic truncation split UTF-8");
  require(queue->items.at(1).event == "Close" && queue->items.at(1).value.at("reason") == std::string(122, 'a'), "close truncation lost terminal event");
  require(socket_terminal_text("bad\xff", 32) == "bad\xef\xbf\xbd", "native diagnostic repair failed");
  for (auto code : {1000, 1001, 1011, 1012, 1013, 1014, 3000, 4999}) require(socket_peer_close_code(code), "valid peer close code rejected");
  for (auto code : {0, 999, 1004, 1005, 1006, 1015, 1016, 2999, 5000}) require(!socket_peer_close_code(code), "invalid peer close code accepted");
}
}
int main() {
  try {
    strict_options(); bounded_messages_and_fragments(); ingress_backpressure_preserves_terminal_events(); terminal_unicode_and_peer_codes();
    std::cout << "Socket protocol: 4 groups passed (no OS transport)\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
