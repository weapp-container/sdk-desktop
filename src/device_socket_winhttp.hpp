#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include "device_socket_common.hpp"

namespace weapp::desktop::detail {
inline std::wstring socket_wide(const std::string& value) {
  if (value.empty()) return {};
  auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
  if (!size) throw std::invalid_argument("invalid socket UTF-8");
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), result.data(), size);
  return result;
}
inline std::string socket_narrow(const std::wstring& value) {
  if (value.empty()) return {};
  auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
  if (!size) throw std::runtime_error("invalid socket response Unicode");
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), result.data(), size, nullptr, nullptr);
  return result;
}
struct WinSocketAsync {
  struct Read { std::vector<uint8_t> bytes; WINHTTP_WEB_SOCKET_BUFFER_TYPE type; };
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<uint8_t> buffer = std::vector<uint8_t>(16384);
  std::optional<Read> read;
  bool sent = false, headers = false, written = false, closed = false;
  DWORD error = 0;
  size_t handles = 0;
  static void CALLBACK status(HINTERNET, DWORD_PTR context, DWORD event, LPVOID information, DWORD length) noexcept {
    auto state = reinterpret_cast<WinSocketAsync*>(context);
    if (!state) return;
    try {
      std::lock_guard<std::mutex> guard(state->mutex);
      if (event == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { if (state->handles) --state->handles; }
      else if (event == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE) state->sent = true;
      else if (event == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) state->headers = true;
      else if (event == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE) state->written = true;
      else if (event == WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE) state->closed = true;
      else if (event == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
        if (length != sizeof(WINHTTP_WEB_SOCKET_STATUS)) state->error = ERROR_INVALID_DATA;
        else {
          auto result = static_cast<WINHTTP_WEB_SOCKET_STATUS*>(information);
          if (result->dwBytesTransferred > state->buffer.size() || state->read) state->error = ERROR_INVALID_DATA;
          else state->read = Read{std::vector<uint8_t>(state->buffer.begin(), state->buffer.begin() + result->dwBytesTransferred), result->eBufferType};
        }
      } else if (event == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && length == sizeof(WINHTTP_ASYNC_RESULT))
        state->error = static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
      // The drain waiter takes this same mutex. No access to callback state
      // may follow its release on the final HANDLE_CLOSING notification.
      state->changed.notify_all();
    } catch (...) {
      std::lock_guard<std::mutex> guard(state->mutex); state->error = ERROR_NOT_ENOUGH_MEMORY; state->changed.notify_all();
    }
  }
};
struct WinSocketHandle {
  HINTERNET value = nullptr;
  WinSocketHandle() = default;
  WinSocketHandle(const WinSocketHandle&) = delete;
  WinSocketHandle& operator=(const WinSocketHandle&) = delete;
  void close() { auto handle = value; value = nullptr; if (handle) WinHttpCloseHandle(handle); }
  ~WinSocketHandle() { close(); }
};
inline void socket_win_error(DWORD error) { if (error != ERROR_SUCCESS && error != ERROR_IO_PENDING) throw std::system_error(error, std::system_category()); }
inline void socket_win_close_status(HINTERNET transport, uint16_t& code, std::string& reason) {
  USHORT peer = 1006; uint8_t bytes[123]; DWORD size = 0;
  auto error = WinHttpWebSocketQueryCloseStatus(transport, &peer, bytes, sizeof(bytes), &size);
  if (error != ERROR_SUCCESS) throw std::system_error(error, std::system_category());
  if (size > sizeof(bytes) || (!socket_peer_close_code(peer) && !(peer == 1005 && !size)))
    throw std::runtime_error("invalid socket close status");
  code = peer; reason.assign(bytes, bytes + size); socket_utf8(reason);
}
inline void socket_win_wait(WinSocketAsync& state, NativeSocket& socket,
    const std::function<bool()>& ready, std::chrono::steady_clock::time_point deadline, bool allow_close = false) {
  std::unique_lock<std::mutex> guard(state.mutex);
  while (!ready()) {
    if (socket.cancelled || (!allow_close && socket.closing())) throw std::runtime_error("socket cancelled");
    if (state.error) throw std::system_error(state.error, std::system_category());
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("socket operation timed out");
    state.changed.wait_for(guard, std::chrono::milliseconds(50));
  }
}
inline void run_winhttp_socket(std::shared_ptr<NativeSocket> socket, SocketOptions options) {
  auto state = std::make_shared<WinSocketAsync>();
  WinSocketHandle session, connection, request, transport;
  std::shared_ptr<SocketCommand> active;
  uint16_t close_code = 1006;
  std::string close_reason, close_payload, failure;
  try {
    { std::lock_guard<std::mutex> guard(socket->mutex); socket->interrupt = [state] { state->changed.notify_all(); }; }
    auto url = socket_wide(options.https_url);
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
      throw std::invalid_argument("invalid native socket URL");
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    session.value = WinHttpOpen(L"WeappContainer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!session.value) throw std::system_error(GetLastError(), std::system_category());
    if (!WinHttpSetTimeouts(session.value, 15000, 15000, int(options.timeout_ms), int(options.timeout_ms)))
      throw std::system_error(GetLastError(), std::system_category());
    connection.value = WinHttpConnect(session.value, host.c_str(), parts.nPort, 0);
    if (!connection.value) throw std::system_error(GetLastError(), std::system_category());
    request.value = WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request.value) throw std::system_error(GetLastError(), std::system_category());
    auto context = reinterpret_cast<DWORD_PTR>(state.get());
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
        WinHttpSetStatusCallback(request.value, WinSocketAsync::status, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
      throw std::system_error(GetLastError(), std::system_category());
    { std::lock_guard<std::mutex> guard(state->mutex); ++state->handles; }
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
      throw std::system_error(GetLastError(), std::system_category());
    std::wstring headers;
    for (const auto& header : options.headers) headers += socket_wide(header.first + ": " + header.second + "\r\n");
    if (!options.protocols.empty()) headers += socket_wide("Sec-WebSocket-Protocol: " + socket_protocol_header(options) + "\r\n");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeout_ms);
    if (!WinHttpSendRequest(request.value, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        DWORD(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, context)) socket_win_error(GetLastError());
    socket_win_wait(*state, *socket, [&] { return state->sent; }, deadline);
    if (!WinHttpReceiveResponse(request.value, nullptr)) socket_win_error(GetLastError());
    socket_win_wait(*state, *socket, [&] { return state->headers; }, deadline);
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX) || status != 101)
      throw std::runtime_error("WebSocket upgrade was not accepted");
    std::wstring selected(256, L'\0'); length = DWORD(selected.size() * sizeof(wchar_t)); DWORD header_index = 0;
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CUSTOM, L"Sec-WebSocket-Protocol",
        selected.data(), &length, &header_index)) {
      selected.resize(length / sizeof(wchar_t));
      DWORD duplicate_length = 0;
      if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CUSTOM, L"Sec-WebSocket-Protocol", nullptr,
          &duplicate_length, &header_index) || GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND)
        throw std::runtime_error("duplicate socket subprotocol response");
      if (selected.empty()) throw std::runtime_error("empty socket subprotocol response");
    }
    else { if (GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND) throw std::runtime_error("invalid socket subprotocol response"); selected.clear(); }
    while (!selected.empty() && selected.back() == L'\0') selected.pop_back();
    auto protocol = socket_narrow(selected); socket_selected_protocol(options, protocol);
    transport.value = WinHttpWebSocketCompleteUpgrade(request.value, context);
    if (!transport.value) throw std::system_error(GetLastError(), std::system_category());
    { std::lock_guard<std::mutex> guard(state->mutex); ++state->handles; }
    request.close();
    DWORD close_timeout = 5000;
    if (!WinHttpSetOption(transport.value, WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT, &close_timeout, sizeof(close_timeout)))
      throw std::system_error(GetLastError(), std::system_category());
    if (socket->opened(options, protocol)) {
      SocketMessage message;
      bool receiving = false, closing = false, close_started = false;
      auto close_deadline = std::chrono::steady_clock::time_point::max();
      auto send_deadline = std::chrono::steady_clock::time_point::max();
      while (!socket->cancelled) {
        std::optional<WinSocketAsync::Read> read;
        bool written = false, closed = false; DWORD error = 0;
        { std::lock_guard<std::mutex> guard(state->mutex); read = std::move(state->read); state->read.reset(); written = state->written; state->written = false; closed = state->closed; error = state->error; }
        if (closed) { socket_win_close_status(transport.value, close_code, close_reason); break; }
        if (error && !(closing && error == ERROR_OPERATION_ABORTED)) throw std::system_error(error, std::system_category());
        if (written && active) { socket->delivered(active); active.reset(); }
        if (read) {
          receiving = false;
          if (read->type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            socket_win_close_status(transport.value, close_code, close_reason);
            if (!closing) {
              closing = true; close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
          } else if (!closing) {
            if (read->type != WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE && read->type != WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE &&
                read->type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE && read->type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
              throw std::runtime_error("invalid socket message type");
            const bool binary = read->type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || read->type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
            const bool final = read->type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || read->type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
            message.append(*socket, binary, read->bytes.data(), read->bytes.size(), final);
          }
        }
        if (!active && !closing) if (auto command = socket->take()) {
          if (command->kind == SocketCommand::Close) {
            closing = true; close_code = command->code; close_reason.assign(command->bytes.begin(), command->bytes.end());
            close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
          } else if (!command->cancelled) {
            active = command;
            send_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeout_ms);
            socket_win_error(WinHttpWebSocketSend(transport.value,
                command->kind == SocketCommand::Binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                command->bytes.empty() ? static_cast<void*>(const_cast<char*>("")) : command->bytes.data(), DWORD(command->bytes.size())));
          }
        }
        // WinHTTP rejects Close while a send is pending. A peer Close can
        // arrive during an async write, so retain its status and drain that
        // write before starting the close operation.
        if (closing && !close_started && !active) {
          close_started = true;
          // The async Close buffer remains immutable through callback drain,
          // even if an already queued receive supplies a different peer reason.
          close_payload = close_reason;
          socket_win_error(WinHttpWebSocketClose(transport.value, close_code,
              close_payload.empty() ? nullptr : close_payload.data(), DWORD(close_payload.size())));
        }
        if (!receiving && !closing) {
          receiving = true;
          socket_win_error(WinHttpWebSocketReceive(transport.value, state->buffer.data(), DWORD(state->buffer.size()), nullptr, nullptr));
        }
        if (std::chrono::steady_clock::now() >= close_deadline) throw std::runtime_error("socket close handshake timed out");
        if (active && std::chrono::steady_clock::now() >= send_deadline) throw std::runtime_error("socket send timed out");
        std::unique_lock<std::mutex> guard(state->mutex); state->changed.wait_for(guard, std::chrono::milliseconds(25));
      }
    }
  } catch (const std::exception& error) {
    if (!socket->cancelled) { if (!(socket->closing() && !transport.value)) { close_code = 1006; failure = error.what(); } }
    if (active && !socket->cancelled && !active->cancelled)
      queue_failure(socket->queue, active->call, device_failure("object:SocketTask.send", "socket send failed"));
  }
  // HANDLE_CLOSING is WinHTTP's final callback. Keep all callback state and
  // buffers alive until both the request and upgraded socket report it.
  transport.close(); request.close();
  { std::unique_lock<std::mutex> guard(state->mutex); state->changed.wait(guard, [&] { return state->handles == 0; }); }
  connection.close(); session.close();
  { std::lock_guard<std::mutex> guard(socket->mutex); socket->interrupt = {}; }
  socket->finish(close_code, close_reason, failure);
}
}  // namespace weapp::desktop::detail
