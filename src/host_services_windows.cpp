#include <windows.h>
#include <winhttp.h>

#include <condition_variable>
#include <mutex>

#include "host_services_internal.hpp"

namespace weapp::desktop::detail {
bool host_http_available() { return true; }
namespace {
std::wstring wide(const std::string& text) {
  auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                   int(text.size()), nullptr, 0);
  if (!count && !text.empty())
    throw std::invalid_argument("Invalid UTF-8 host text");
  std::wstring result(size_t(count), L'\0');
  if (count)
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        int(text.size()), result.data(), count);
  return result;
}
struct Handle {
  HINTERNET value = nullptr;
  ~Handle() {
    if (value) WinHttpCloseHandle(value);
  }
};
struct RequestHandle {
  std::mutex mutex;
  HINTERNET value = nullptr;
  void close() {
    std::lock_guard<std::mutex> lock(mutex);
    if (value) {
      WinHttpCloseHandle(value);
      value = nullptr;
    }
  }
  ~RequestHandle() { close(); }
};
// WinHTTP's four phase timeouts do not bound the entire response. Interrupt a
// slow streaming peer at the overall deadline, using the same owned handle as
// explicit cancellation, and join the watchdog before releasing its state.
class RequestDeadline {
 public:
  RequestDeadline(std::shared_ptr<RequestHandle> request,
                  std::chrono::milliseconds timeout)
      : worker_([this, request = std::move(request), timeout] {
          std::unique_lock<std::mutex> lock(mutex_);
          if (condition_.wait_for(lock, timeout, [this] { return finished_; }))
            return;
          expired_.store(true);
          lock.unlock();
          request->close();
        }) {}
  ~RequestDeadline() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      finished_ = true;
    }
    condition_.notify_one();
    worker_.join();
  }
  bool expired() const { return expired_.load(); }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool finished_ = false;
  std::atomic<bool> expired_{false};
  std::thread worker_;
};
}  // namespace
HostHttpResponse host_http_post(const std::string& endpoint,
                                const std::string& path,
                                const std::string& bearer,
                                const std::string& body,
                                HostCancellation cancel,
                                std::chrono::milliseconds timeout) {
  try {
    auto url = wide(endpoint);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.data(), DWORD(url.size()), 0, &parts))
      return {
          Status::error("ERR_INVALID_REQUEST", "Invalid native backend URL"),
          0,
          {}};
    Handle session{WinHttpOpen(
        L"WeappContainerDesktop/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value)
      return {Status::error("ERR_NETWORK", "Cannot initialize WinHTTP"), 0, {}};
    WinHttpSetTimeouts(session.value, int(timeout.count()),
                       int(timeout.count()), int(timeout.count()),
                       int(timeout.count()));
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength),
        request_path = wide(path);
    Handle connection{
        WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    if (!connection.value)
      return {Status::error("ERR_NETWORK", "Cannot connect to host backend"),
              0,
              {}};
    auto request = std::make_shared<RequestHandle>();
    request->value = WinHttpOpenRequest(
        connection.value, L"POST", request_path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request->value)
      return {
          Status::error("ERR_NETWORK", "Cannot allocate host request"), 0, {}};
    const auto handle = request->value;
    RequestDeadline deadline(request, timeout);
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER,
          disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirects,
                          sizeof(redirects)) ||
        !WinHttpSetOption(handle, WINHTTP_OPTION_DISABLE_FEATURE, &disabled,
                          sizeof(disabled)))
      return {
          Status::error("ERR_NETWORK",
                        "Cannot enforce native HTTP redirect/cookie policy"),
          0,
          {}};
    std::weak_ptr<RequestHandle> weak = request;
    cancel.on_cancel([weak] {
      if (auto request = weak.lock()) request->close();
    });
    const auto headers =
        wide("Content-Type: application/json\r\nAuthorization: Bearer " +
             bearer + "\r\n");
    HostHttpResponse response;
    auto network_failure = [&] {
      return HostHttpResponse{
          Status::error(cancel.cancelled()   ? "ERR_CANCELLED"
                        : deadline.expired() ? "ERR_TIMEOUT"
                                             : "ERR_NETWORK",
                        cancel.cancelled() ? "Host request cancelled"
                        : deadline.expired()
                            ? "Host request deadline exceeded"
                            : "WinHTTP backend request failed"),
          0,
          {}};
    };
    if (cancel.cancelled() ||
        !WinHttpSendRequest(handle, headers.c_str(), DWORD(headers.size()),
                            const_cast<char*>(body.data()), DWORD(body.size()),
                            DWORD(body.size()), 0) ||
        !WinHttpReceiveResponse(handle, nullptr))
      return network_failure();
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(
            handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &length,
            WINHTTP_NO_HEADER_INDEX))
      return network_failure();
    response.http_status = int(status);
    for (;;) {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(handle, &available))
        return network_failure();
      if (!available) break;
      if (available > 262144 - response.body.size())
        return {Status::error("ERR_RESOURCE_LIMIT",
                              "Backend response exceeds 256 KiB"),
                0,
                {}};
      std::string bytes(size_t(available), '\0');
      DWORD read = 0;
      if (!WinHttpReadData(handle, bytes.data(), available, &read))
        return network_failure();
      response.body.append(bytes.data(), read);
      if (!read) break;
    }
    return response;
  } catch (const std::exception& e) {
    return {Status::error("ERR_INVALID_REQUEST", e.what()), 0, {}};
  }
}
namespace {
struct ConsentWindow {
  std::atomic<HWND> window{nullptr};
  HostCancellation cancellation;
  std::function<void(HostResult<bool>)> complete;
  bool done = false;
  void finish(int decision) {
    if (done) return;
    done = true;
    if (cancellation.cancelled() || decision < 0)
      complete(
          {Status::error("ERR_CANCELLED", "Consent cancelled"), false, {}});
    else
      complete({{}, decision == 1, {}});
    if (auto hwnd = window.load()) DestroyWindow(hwnd);
  }
  static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w,
                                    LPARAM l) {
    auto* pointer = reinterpret_cast<std::shared_ptr<ConsentWindow>*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      pointer = static_cast<std::shared_ptr<ConsentWindow>*>(
          reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(pointer));
      (*pointer)->window.store(hwnd);
    }
    if (!pointer) return DefWindowProcW(hwnd, message, w, l);
    auto state = *pointer;
    if (message == WM_COMMAND) {
      if (LOWORD(w) == 1 || LOWORD(w) == 2) {
        state->finish(LOWORD(w) == 1 ? 1 : 0);
        return 0;
      }
    }
    if (message == WM_CLOSE) {
      state->finish(-1);
      return 0;
    }
    if (message == WM_NCDESTROY) {
      state->window.store(nullptr);
      if (!state->done) {
        state->done = true;
        state->complete(
            {Status::error("ERR_CANCELLED", "Host consent window destroyed"),
             false,
             {}});
      }
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      delete pointer;
    }
    return DefWindowProcW(hwnd, message, w, l);
  }
};
class WindowsHostConsent final : public HostConsentPresenter {
 public:
  explicit WindowsHostConsent(HWND parent)
      : parent_(parent), thread_(GetCurrentThreadId()) {}
  void present(const HostConsentRequest& request, HostCancellation cancellation,
               std::function<void(HostResult<bool>)> complete) override {
    if (thread_ != GetCurrentThreadId() || !IsWindow(parent_) ||
        !IsWindowVisible(parent_) || cancellation.cancelled()) {
      complete({Status::error(
                    "ERR_INVALID_REQUEST",
                    "Visible host HWND on its UI thread required for consent"),
                false,
                {}});
      return;
    }
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, [] {
      WNDCLASSW cls{};
      cls.lpfnWndProc = ConsentWindow::procedure;
      cls.hInstance = GetModuleHandleW(nullptr);
      cls.lpszClassName = L"WeappContainerHostConsent";
      cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
      cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
      registered = RegisterClassW(&cls) != 0;
    });
    if (!registered) {
      complete({Status::error("ERR_NATIVE_UI",
                              "Cannot register native consent window"),
                false,
                {}});
      return;
    }
    const bool phone = request.policy.kind == HostConsentKind::Phone;
    auto state = std::make_shared<ConsentWindow>();
    state->cancellation = cancellation;
    state->complete = std::move(complete);
    RECT parent{};
    GetWindowRect(parent_, &parent);
    auto* retained = new std::shared_ptr<ConsentWindow>(state);
    HWND hwnd =
        CreateWindowExW(WS_EX_DLGMODALFRAME, L"WeappContainerHostConsent",
                        phone ? L"授权手机号" : L"授权昵称和头像",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                        parent.left + 20, parent.top + 20, 600, 330, parent_,
                        nullptr, GetModuleHandleW(nullptr), retained);
    if (!hwnd) {
      delete retained;
      state->complete(
          {Status::error("ERR_NATIVE_UI", "Cannot create host consent window"),
           false,
           {}});
      return;
    }
    const auto text = wide(request.context.application.app_id + "\n\n" +
                           (phone ? "允许此小程序获取宿主账号已验证的手机号？"
                                  : "允许此小程序获取宿主账号的昵称和头像？") +
                           "\n\n用途：" + request.policy.purpose +
                           "\n权限版本：" + request.policy.permission_version);
    auto label = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_READONLY,
        20, 20, 550, 210, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto deny =
        CreateWindowExW(0, L"BUTTON", L"拒绝",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        350, 245, 100, 30, hwnd, reinterpret_cast<HMENU>(2),
                        GetModuleHandleW(nullptr), nullptr);
    auto allow = CreateWindowExW(0, L"BUTTON", L"允许",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP, 465, 245,
                                 100, 30, hwnd, reinterpret_cast<HMENU>(1),
                                 GetModuleHandleW(nullptr), nullptr);
    for (auto control : {label, deny, allow})
      SendMessageW(control, WM_SETFONT,
                   reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                   TRUE);
    std::weak_ptr<ConsentWindow> weak = state;
    cancellation.on_cancel([weak] {
      if (auto state = weak.lock())
        if (auto hwnd = state->window.load())
          PostMessageW(hwnd, WM_CLOSE, 0, 0);
    });
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(deny);
  }

 private:
  HWND parent_;
  DWORD thread_;
};
}  // namespace
std::shared_ptr<HostConsentPresenter> host_consent_presenter(void* parent,
                                                             Status& status) {
  if (!parent || !IsWindow(static_cast<HWND>(parent)) ||
      GetWindowThreadProcessId(static_cast<HWND>(parent), nullptr) !=
          GetCurrentThreadId()) {
    status =
        Status::error("ERR_INVALID_REQUEST", "Configure a native host HWND");
    return {};
  }
  status = {};
  return std::make_shared<WindowsHostConsent>(static_cast<HWND>(parent));
}
}  // namespace weapp::desktop::detail
