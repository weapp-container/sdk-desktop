#pragma once
#include "device_apis_common.hpp"
#include <condition_variable>

namespace weapp::desktop::detail {
constexpr size_t socket_message_limit = 1048576;
inline bool socket_token(const std::string& text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || std::string("!#$%&'*+-.^_`|~").find(c) != std::string::npos;
  });
}
inline void socket_utf8(const std::string& text) { (void)Json(text).dump(); }
inline std::string socket_terminal_text(const std::string& text, size_t limit) {
  // Native diagnostics can use a platform encoding. Repair them before they
  // reach the JSON queue, and truncate only at a complete UTF-8 boundary.
  auto valid = Json::parse(Json(text).dump(-1, ' ', false, Json::error_handler_t::replace)).get<std::string>();
  if (valid.size() > limit) {
    auto end = limit;
    while (end && (static_cast<unsigned char>(valid[end]) & 0xc0) == 0x80) --end;
    valid.resize(end);
  }
  return valid;
}
inline bool socket_peer_close_code(uint16_t code) {
  return (code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006) ||
      (code >= 3000 && code <= 4999);
}
struct SocketOptions {
  std::string url, https_url, origin;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::string> protocols;
  long timeout_ms = 60000;
};
inline SocketOptions socket_options(const Json& payload, const std::vector<std::string>& origins) {
  SocketOptions result;
  auto url = payload.at("url").get<std::string>();
  if (url.size() > 8192 || url.rfind("wss://", 0) != 0 ||
      !std::all_of(url.begin(), url.end(), [](unsigned char c) { return c > 32 && c < 127; }) ||
      url.find('#') != std::string::npos) throw std::invalid_argument("socket requires an ASCII credential-free WSS URL without a fragment");
  auto end = url.find_first_of("/?", 6);
  if (end == std::string::npos) end = url.size();
  auto authority = url.substr(6, end - 6);
  if (authority.empty() || authority.find_first_of("@%\\") != std::string::npos)
    throw std::invalid_argument("invalid socket authority");
  std::string host, port;
  if (authority.front() == '[') {
    auto bracket = authority.find(']');
    if (bracket == std::string::npos || bracket < 3) throw std::invalid_argument("invalid socket IPv6 host");
    host = authority.substr(0, bracket + 1);
    if (!std::all_of(host.begin() + 1, host.end() - 1, [](unsigned char c) {
          return std::isxdigit(c) || c == ':' || c == '.';
        })) throw std::invalid_argument("invalid socket IPv6 host");
    if (bracket + 1 < authority.size()) {
      if (authority[bracket + 1] != ':') throw std::invalid_argument("invalid socket port");
      port = authority.substr(bracket + 2);
      if (port.empty()) throw std::invalid_argument("invalid socket port");
    }
  } else {
    auto colon = authority.find(':');
    host = authority.substr(0, colon);
    if (host.empty() || !std::all_of(host.begin(), host.end(), [](unsigned char c) {
          return std::isalnum(c) || c == '.' || c == '-';
        })) throw std::invalid_argument("invalid socket host");
    if (colon != std::string::npos) {
      port = authority.substr(colon + 1);
      if (port.empty()) throw std::invalid_argument("invalid socket port");
    }
  }
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
  if (!port.empty()) {
    if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
      throw std::invalid_argument("invalid socket port");
    auto number = std::stoul(port);
    if (!number || number > 65535) throw std::invalid_argument("invalid socket port");
    port = number == 443 ? "" : ":" + std::to_string(number);
  }
  auto path = url.substr(end);
  if (path.empty() || path.front() == '?') path = "/" + path;
  if (path.find('\\') != std::string::npos) throw std::invalid_argument("invalid socket URL path");
  result.origin = "https://" + host + port;
  if (std::find(origins.begin(), origins.end(), result.origin) == origins.end())
    throw std::invalid_argument("socket origin denied by trusted host policy");
  result.url = "wss://" + host + port + path;
  result.https_url = result.origin + path;
  auto headers = payload.value("header", Json::object());
  if (!headers.is_object() || headers.size() > 32) throw std::invalid_argument("socket headers exceed budget");
  size_t bytes = 0;
  std::set<std::string> names, protocols;
  for (auto item = headers.begin(); item != headers.end(); ++item) {
    auto name = item.key(), lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    auto value = item.value().get<std::string>();
    if (!socket_token(name) || !names.insert(lower).second || lower == "host" || lower == "connection" ||
        lower == "upgrade" || lower == "content-length" || lower == "transfer-encoding" ||
        lower == "proxy-authorization" || lower.rfind("sec-websocket-", 0) == 0 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c == 9 || (c >= 32 && c < 127); }))
      throw std::invalid_argument("invalid or reserved socket header");
    bytes += name.size() + value.size();
    if (bytes > 16384) throw std::invalid_argument("socket header byte budget exceeded");
    result.headers.emplace_back(std::move(name), std::move(value));
  }
  auto offered = payload.value("protocols", Json::array());
  if (!offered.is_array() || offered.size() > 16) throw std::invalid_argument("socket subprotocol budget exceeded");
  for (const auto& value : offered) {
    auto protocol = value.get<std::string>();
    if (protocol.size() > 128 || !socket_token(protocol) || !protocols.insert(protocol).second)
      throw std::invalid_argument("invalid or duplicate socket subprotocol");
    result.protocols.push_back(std::move(protocol));
  }
  auto timeout = payload.value("timeout", int64_t(60000));
  if (timeout <= 0 || timeout > 300000) throw std::invalid_argument("socket timeout must be 1..300000 milliseconds");
  result.timeout_ms = long(timeout);
  return result;
}
inline std::string socket_protocol_header(const SocketOptions& options) {
  std::string value;
  for (const auto& protocol : options.protocols) { if (!value.empty()) value += ", "; value += protocol; }
  return value;
}
inline void socket_selected_protocol(const SocketOptions& options, const std::string& protocol) {
  if (!protocol.empty() && std::find(options.protocols.begin(), options.protocols.end(), protocol) == options.protocols.end())
    throw std::runtime_error("server selected an unoffered WebSocket subprotocol");
}
struct SocketCommand {
  enum Kind { Text, Binary, Close } kind = Text;
  std::string call;
  std::vector<uint8_t> bytes;
  uint16_t code = 1000;
  std::atomic<bool> cancelled{false};
};
struct NativeSocket {
  std::weak_ptr<DeviceQueue> queue;
  std::string object;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::shared_ptr<SocketCommand>> outgoing;
  size_t outgoing_bytes = 0;
  enum Phase { Connecting, Open, Closing, Closed } phase = Connecting;
  std::atomic<bool> cancelled{false}, finished{false};
  std::function<void()> interrupt;
  std::shared_ptr<SocketCommand> close_command;
  NativeSocket(std::weak_ptr<DeviceQueue> owner, std::string id) : queue(std::move(owner)), object(std::move(id)) {}
  void cancel() {
    std::function<void()> stop;
    { std::lock_guard<std::mutex> guard(mutex); cancelled = true; stop = interrupt; }
    changed.notify_all();
    if (stop) stop();
  }
  std::shared_ptr<SocketCommand> enqueue(const std::string& call, const Json& payload, bool close) {
    auto command = std::make_shared<SocketCommand>();
    command->call = call;
    if (close) {
      auto code = payload.value("code", int64_t(1000));
      auto reason = payload.value("reason", "");
      if ((code != 1000 && (code < 3000 || code > 4999)) || reason.size() > 123)
        throw std::invalid_argument("invalid socket close code or reason length");
      socket_utf8(reason);
      command->kind = SocketCommand::Close; command->code = uint16_t(code);
      command->bytes.assign(reason.begin(), reason.end());
    } else {
      const auto& value = payload.at("data");
      if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() > socket_message_limit) throw std::invalid_argument("socket message exceeds one MiB");
        socket_utf8(text);
        command->bytes.assign(text.begin(), text.end());
      } else { command->kind = SocketCommand::Binary; command->bytes = binary_bytes(value); }
      if (command->bytes.size() > socket_message_limit) throw std::invalid_argument("socket message exceeds one MiB");
    }
    std::lock_guard<std::mutex> guard(mutex);
    if (cancelled || phase == Closed || phase == Closing || (!close && phase != Open))
      throw std::invalid_argument("socket is not open");
    if (outgoing.size() >= 32 || command->bytes.size() > 4 * socket_message_limit - outgoing_bytes)
      throw std::invalid_argument("socket outgoing backpressure limit exceeded");
    outgoing_bytes += command->bytes.size(); outgoing.push_back(command);
    if (close) { phase = Closing; close_command = command; }
    changed.notify_one();
    return command;
  }
  std::shared_ptr<SocketCommand> take() {
    std::lock_guard<std::mutex> guard(mutex);
    if (outgoing.empty()) return {};
    auto value = outgoing.front(); outgoing.pop_front(); outgoing_bytes -= value->bytes.size(); return value;
  }
  bool closing() { std::lock_guard<std::mutex> guard(mutex); return phase == Closing; }
  bool opened(const SocketOptions& options, const std::string& protocol, Json headers = Json::object()) {
    socket_selected_protocol(options, protocol);
    { std::lock_guard<std::mutex> guard(mutex); if (cancelled || phase != Connecting) return false; phase = Open; }
    if (!queue_event(queue, "Open", object, {{"protocol", protocol}, {"header", headers}}))
      throw std::runtime_error("socket ingress backpressure limit exceeded");
    return true;
  }
  void delivered(const std::shared_ptr<SocketCommand>& command) {
    if (!cancelled && !command->cancelled) queue_complete(queue, command->call, device_ok("object:SocketTask.send"));
  }
  void message(bool binary, const std::vector<uint8_t>& bytes) {
    if (bytes.size() > socket_message_limit) throw std::runtime_error("socket message exceeds one MiB");
    if (cancelled) return;
    Json value;
    if (binary) value = inline_binary(bytes);
    else { auto text = std::string(bytes.begin(), bytes.end()); socket_utf8(text); value = text; }
    if (!queue_event(queue, "Message", object, {{"data", value}}))
      throw std::runtime_error("socket ingress backpressure limit exceeded");
  }
  // Call only after transport handles are closed and receive workers joined.
  void finish(uint16_t code, std::string reason, const std::string& error = "") {
    std::deque<std::shared_ptr<SocketCommand>> pending;
    std::shared_ptr<SocketCommand> close;
    { std::lock_guard<std::mutex> guard(mutex); phase = Closed; interrupt = {}; pending.swap(outgoing); outgoing_bytes = 0; close = close_command; }
    if (!cancelled) {
      for (const auto& command : pending) if (command->kind != SocketCommand::Close && !command->cancelled)
        queue_failure(queue, command->call, device_failure("object:SocketTask.send", "socket closed before send"));
      if (auto owner = queue.lock()) {
        if (!error.empty()) owner->push({DeviceQueued::NativeTerminalEvent, "", "Error", object, {{"errMsg", socket_terminal_text(error, 512)}}});
        owner->push({DeviceQueued::NativeTerminalEvent, "", "Close", object, {{"code", code}, {"reason", socket_terminal_text(reason, 123)}}});
      }
      if (close && !close->cancelled) {
        if (error.empty()) queue_complete(queue, close->call, device_ok("object:SocketTask.close"));
        else queue_failure(queue, close->call, device_failure("object:SocketTask.close", socket_terminal_text(error, 512)));
      }
    }
    finished = true; changed.notify_all();
  }
};
// Transport chunks and RFC6455 continuation frames are separate boundaries.
class SocketMessage {
  std::vector<uint8_t> bytes_;
  std::optional<bool> binary_;
 public:
  void append(NativeSocket& socket, bool binary, const uint8_t* data, size_t size, bool final) {
    if ((binary_ && *binary_ != binary) || size > socket_message_limit - bytes_.size())
      throw std::runtime_error("invalid fragmented or oversized socket message");
    binary_ = binary;
    if (size) bytes_.insert(bytes_.end(), data, data + size);
    if (final) { socket.message(binary, bytes_); bytes_.clear(); binary_.reset(); }
  }
};
class DeviceSockets : public DeviceBase {
  using Runner = void (*)(std::shared_ptr<NativeSocket>, SocketOptions);
  Runner socket_runner_ = nullptr;
  std::map<std::string, std::shared_ptr<NativeSocket>> sockets_;
 protected:
  DeviceSockets(DeviceApiOptions options, Command command) : DeviceBase(std::move(options), std::move(command)) {}
  void install_socket_runner(Runner runner) {
    socket_runner_ = runner;
    for (auto api : {"wx.connectSocket", "object:SocketTask.send", "object:SocketTask.close"}) add(api);
    for (auto event : {"Open", "Message", "Error", "Close"})
      add_event(std::string("object:SocketTask.on") + event, "", event);
  }
  bool socket_call(const std::string& call, const std::string& api, const Json& payload) {
    if (!socket_runner_) return false;
    if (api == "wx.connectSocket") {
      for (auto item = sockets_.begin(); item != sockets_.end();)
        if (item->second->finished) item = sockets_.erase(item); else ++item;
      if (sockets_.size() >= 8) throw std::invalid_argument("native socket concurrency limit exceeded");
      auto options = socket_options(payload, options_.allowed_network_origins);
      auto id = payload.at("objectId").get<std::string>();
      auto socket = std::make_shared<NativeSocket>(weak_queue(), id);
      // A cancelled owner still consumes a slot until its native worker has
      // actually drained; rapid cancellation cannot bypass the worker bound.
      retain_object(id, call, "SocketTask", [socket] { socket->cancel(); });
      sockets_[id] = socket;
      auto start = std::make_shared<std::promise<void>>();
      auto ready = start->get_future();
      workers_.push_back(std::async(std::launch::async, [socket, options = std::move(options), runner = socket_runner_, ready = std::move(ready)] {
        ready.wait();
        runner(socket, options);
      }));
      // The factory result must enter the owner queue before Open/Message.
      done(call, api, {{"objectId", id}});
      start->set_value();
      return true;
    }
    if (api != "object:SocketTask.send" && api != "object:SocketTask.close") return false;
    require_object(payload, "SocketTask");
    auto found = sockets_.find(payload.value("objectId", ""));
    if (found == sockets_.end()) throw std::invalid_argument("socket transport is closed");
    auto command = found->second->enqueue(call, payload, api == "object:SocketTask.close");
    calls_.at(call).cancel = [command] { command->cancelled = true; };
    return true;
  }
 public:
  ~DeviceSockets() override { retire(); }
  Status pump() override {
    auto status = DeviceBase::pump();
    if (!status) return status;
    for (auto item = sockets_.begin(); item != sockets_.end();)
      if (item->second->finished) item = sockets_.erase(item); else ++item;
    return status;
  }
};
}  // namespace weapp::desktop::detail
