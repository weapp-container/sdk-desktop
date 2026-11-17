#pragma once
#include "device_socket_common.hpp"
#include <curl/curl.h>
#include <curl/websockets.h>
#include <poll.h>
#include <sys/socket.h>
#if LIBCURL_VERSION_NUM < 0x081000
#error Native WebSocket transport requires libcurl 8.16 or newer
#endif

namespace weapp::desktop::detail {
inline bool curl_socket_available() {
  auto version = curl_version_info(CURLVERSION_NOW);
  if (!version || version->version_num < 0x081000 || !version->protocols) return false;
  for (auto protocol = version->protocols; *protocol; ++protocol)
    if (std::string(*protocol) == "wss") return true;
  return false;
}
template<class Value> inline void curl_socket_option(CURL* curl, CURLoption option, Value value) {
  auto code = curl_easy_setopt(curl, option, value);
  if (code != CURLE_OK) throw std::runtime_error(curl_easy_strerror(code));
}
struct CurlSocketInterrupt {
  std::mutex mutex;
  curl_socket_t socket = CURL_SOCKET_BAD;
  void stop() { std::lock_guard<std::mutex> guard(mutex); if (socket != CURL_SOCKET_BAD) ::shutdown(socket, SHUT_RDWR); }
  void clear() { std::lock_guard<std::mutex> guard(mutex); socket = CURL_SOCKET_BAD; }
};
inline void curl_socket_wait(CURL* curl, bool write, NativeSocket& socket,
    std::chrono::steady_clock::time_point deadline) {
  if (socket.cancelled) throw std::runtime_error("socket cancelled");
  if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("socket operation timed out");
  curl_socket_t fd = CURL_SOCKET_BAD;
  if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &fd) != CURLE_OK || fd == CURL_SOCKET_BAD)
    throw std::runtime_error("socket transport is unavailable");
  pollfd pending{fd, short(write ? POLLOUT : POLLIN), 0};
  auto result = ::poll(&pending, 1, 50);
  if (result < 0 && errno != EINTR) throw std::system_error(errno, std::generic_category());
}
inline void curl_socket_send(CURL* curl, NativeSocket& socket, const std::vector<uint8_t>& bytes,
    unsigned type, long timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  size_t offset = 0;
  do {
    // Bound each RFC6455 fragment; transport writes may split it further.
    auto size = std::min<size_t>(16384, bytes.size() - offset);
    const bool more = offset + size < bytes.size();
    auto flags = type | (more ? CURLWS_CONT : 0);
    size_t sent_total = 0;
    do {
      if (socket.cancelled) throw std::runtime_error("socket cancelled");
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("socket send timed out");
      size_t sent = 0;
      const void* data = bytes.empty() ? static_cast<const void*>("") : bytes.data() + offset + sent_total;
      // 8.16+ retains the current frame across short writes, including an
      // empty frame whose header initially returns CURLE_AGAIN.
      auto code = curl_ws_send(curl, data, size - sent_total, &sent, 0, flags);
      if (code != CURLE_OK && code != CURLE_AGAIN) throw std::runtime_error(curl_easy_strerror(code));
      if (sent > size - sent_total) throw std::runtime_error("invalid socket transport write length");
      sent_total += sent;
      if (sent_total == size && code == CURLE_OK) break;
      curl_socket_wait(curl, true, socket, deadline);
    } while (true);
    offset += size;
  } while (offset < bytes.size());
}
struct CurlSocketHeaders {
  Json headers = Json::object();
  size_t bytes = 0;
  std::string protocol;
  bool invalid = false, protocol_seen = false;
};
inline size_t curl_socket_header(char* data, size_t size, size_t count, void* pointer) noexcept {
  auto& result = *static_cast<CurlSocketHeaders*>(pointer);
  try {
    if (size && count > 65536 / size) { result.invalid = true; return 0; }
    auto bytes = size * count;
    if (bytes > 65536 - std::min<size_t>(result.bytes, 65536)) { result.invalid = true; return 0; }
    result.bytes += bytes;
    std::string line(data, bytes);
    if (line.rfind("HTTP/", 0) == 0) { result.headers = Json::object(); result.protocol.clear(); result.protocol_seen = false; }
    auto colon = line.find(':');
    if (colon != std::string::npos) {
      auto name = line.substr(0, colon), value = line.substr(colon + 1);
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
      while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
      if (!socket_token(name) || result.headers.size() >= 128 ||
          std::any_of(value.begin(), value.end(), [](unsigned char c) { return (c < 32 && c != 9) || c == 127; })) {
        result.invalid = true; return 0;
      }
      socket_utf8(value);
      if (name == "sec-websocket-protocol") {
        if (result.protocol_seen || !socket_token(value)) { result.invalid = true; return 0; }
        result.protocol = value; result.protocol_seen = true;
      }
      result.headers[name] = value;
    }
    return bytes;
  } catch (...) { result.invalid = true; return 0; }  // Never unwind through libcurl's C callback.
}
inline void run_curl_socket(std::shared_ptr<NativeSocket> socket, SocketOptions options) {
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(nullptr, curl_easy_cleanup);
  auto interrupt = std::make_shared<CurlSocketInterrupt>();
  uint16_t close_code = 1006;
  std::string close_reason, failure;
  bool upgraded = false;
  try {
    curl.reset(curl_easy_init());
    if (!curl) throw std::runtime_error("cannot initialize WebSocket transport");
    CurlSocketHeaders response;
    auto raw = curl.get();
    curl_socket_option(raw, CURLOPT_URL, options.url.c_str());
    curl_socket_option(raw, CURLOPT_PROTOCOLS_STR, "wss");
    curl_socket_option(raw, CURLOPT_FOLLOWLOCATION, 0L);
    curl_socket_option(raw, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_socket_option(raw, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_socket_option(raw, CURLOPT_CONNECT_ONLY, 2L);
    curl_socket_option(raw, CURLOPT_NOSIGNAL, 1L);
    curl_socket_option(raw, CURLOPT_TIMEOUT_MS, options.timeout_ms);
    curl_socket_option(raw, CURLOPT_CONNECTTIMEOUT_MS, std::min<long>(options.timeout_ms, 15000));
    curl_socket_option(raw, CURLOPT_HEADERFUNCTION, curl_socket_header);
    curl_socket_option(raw, CURLOPT_HEADERDATA, &response);
    curl_socket_option(raw, CURLOPT_XFERINFOFUNCTION,
        +[](void* pointer, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
          auto state = static_cast<NativeSocket*>(pointer); return state->cancelled || state->closing() ? 1 : 0;
        });
    curl_socket_option(raw, CURLOPT_XFERINFODATA, socket.get());
    curl_socket_option(raw, CURLOPT_NOPROGRESS, 0L);
    curl_slist* list = nullptr;
    auto free_headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>(nullptr, curl_slist_free_all);
    for (const auto& header : options.headers) {
      auto next = curl_slist_append(list, (header.first + ": " + header.second).c_str());
      if (!next) { curl_slist_free_all(list); throw std::bad_alloc(); } list = next;
    }
    if (!options.protocols.empty()) {
      auto next = curl_slist_append(list, ("Sec-WebSocket-Protocol: " + socket_protocol_header(options)).c_str());
      if (!next) { curl_slist_free_all(list); throw std::bad_alloc(); } list = next;
    }
    free_headers.reset(list); curl_socket_option(raw, CURLOPT_HTTPHEADER, list);
    auto code = curl_easy_perform(raw);
    if (code != CURLE_OK || response.invalid) throw std::runtime_error(response.invalid ? "invalid WebSocket response headers" : curl_easy_strerror(code));
    curl_socket_option(raw, CURLOPT_NOPROGRESS, 1L);
    long status = 0; curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &status);
    if (status != 101) throw std::runtime_error("WebSocket upgrade was not accepted");
    upgraded = true;
    curl_socket_t fd = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(raw, CURLINFO_ACTIVESOCKET, &fd) != CURLE_OK || fd == CURL_SOCKET_BAD)
      throw std::runtime_error("WebSocket connection has no native socket");
    { std::lock_guard<std::mutex> guard(interrupt->mutex); interrupt->socket = fd; }
    { std::lock_guard<std::mutex> guard(socket->mutex); socket->interrupt = [interrupt] { interrupt->stop(); }; }
    if (socket->cancelled) interrupt->stop();
    if (socket->opened(options, response.protocol, response.headers)) {
      SocketMessage message;
      std::vector<uint8_t> control;
      bool close_sent = false;
      auto close_deadline = std::chrono::steady_clock::time_point::max();
      while (!socket->cancelled) {
        if (auto command = socket->take()) {
          if (command->kind == SocketCommand::Close) {
            std::vector<uint8_t> bytes{uint8_t(command->code >> 8), uint8_t(command->code)};
            bytes.insert(bytes.end(), command->bytes.begin(), command->bytes.end());
            curl_socket_send(raw, *socket, bytes, CURLWS_CLOSE, std::min<long>(options.timeout_ms, 5000));
            close_sent = true; close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
          } else if (!command->cancelled) {
            try { curl_socket_send(raw, *socket, command->bytes, command->kind == SocketCommand::Binary ? CURLWS_BINARY : CURLWS_TEXT, options.timeout_ms); socket->delivered(command); }
            catch (...) { if (!socket->cancelled && !command->cancelled) queue_failure(socket->queue, command->call, device_failure("object:SocketTask.send", "socket send failed")); throw; }
          }
        }
        uint8_t bytes[16384]; size_t received = 0; const curl_ws_frame* metadata = nullptr;
        code = curl_ws_recv(raw, bytes, sizeof(bytes), &received, &metadata);
        if (code == CURLE_AGAIN) { curl_socket_wait(raw, false, *socket, close_deadline); continue; }
        if (code != CURLE_OK || !metadata) throw std::runtime_error(curl_easy_strerror(code));
        const auto flags = metadata->flags;
        if (flags & CURLWS_CLOSE) {
          if (received > 125 - control.size()) throw std::runtime_error("invalid socket close frame");
          control.insert(control.end(), bytes, bytes + received);
          if (metadata->bytesleft) continue;
          if (control.size() == 1) throw std::runtime_error("invalid socket close payload");
          close_code = control.empty() ? 1005 : uint16_t(uint16_t(control[0]) << 8 | control[1]);
          if (!control.empty() && !socket_peer_close_code(close_code)) throw std::runtime_error("invalid socket close code");
          if (control.size() > 2) close_reason.assign(control.begin() + 2, control.end());
          socket_utf8(close_reason);
          if (!close_sent) curl_socket_send(raw, *socket, control, CURLWS_CLOSE, 1000);
          break;
        }
        if (flags & (CURLWS_PING | CURLWS_PONG)) continue;  // libcurl supplies automatic pong.
        if (!(flags & (CURLWS_TEXT | CURLWS_BINARY))) throw std::runtime_error("invalid socket data frame");
        message.append(*socket, bool(flags & CURLWS_BINARY), bytes, received, !metadata->bytesleft && !(flags & CURLWS_CONT));
      }
    }
  } catch (const std::exception& error) {
    if (!socket->cancelled && !(socket->closing() && !upgraded)) { close_code = 1006; failure = error.what(); }
  }
  // Disable the shared FD before cleanup to prevent cancellation touching a
  // recycled descriptor. curl itself is accessed only by this worker.
  { std::lock_guard<std::mutex> guard(socket->mutex); socket->interrupt = {}; }
  interrupt->clear(); curl.reset();
  socket->finish(close_code, close_reason, failure);
}
}  // namespace weapp::desktop::detail
