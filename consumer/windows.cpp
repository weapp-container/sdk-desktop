#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "smoke.hpp"
int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: desktop_consumer PACK [REPORT]\n";
    return 2;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"WeappInstalledConsumer";
  RegisterClassW(&wc);
  HWND window =
      CreateWindowW(wc.lpszClassName, L"Weapp Container installed consumer",
                    WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 900, 600, nullptr,
                    nullptr, wc.hInstance, nullptr);
  if (!window) return 3;
  Host host;
  host.parent = window;
  host.spin = [](double seconds) {
    auto until = std::chrono::steady_clock::now() +
                 std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
      MSG m{};
      while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  host.click = [](View& view, double x, double y) {
    auto w = static_cast<HWND>(view.native_handle());
    double d = GetDpiForWindow(w) / 96.0;
    LPARAM p = MAKELPARAM(int(std::lround(x * d)), int(std::lround(y * d)));
    SendMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, p);
    SendMessageW(w, WM_LBUTTONUP, 0, p);
  };
  host.drag = [](View& view, double x, double from, double to) {
    auto w = static_cast<HWND>(view.native_handle());
    double d = GetDpiForWindow(w) / 96.0;
    const auto point = [=](double y) { return MAKELPARAM(int(std::lround(x*d)), int(std::lround(y*d))); };
    SendMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, point(from));
    for (int step=1; step<=6; ++step) SendMessageW(w, WM_MOUSEMOVE, MK_LBUTTON, point(from+(to-from)*step/6));
    SendMessageW(w, WM_LBUTTONUP, 0, point(to));
  };
  host.key = [](View& view) {
    SendMessageW(static_cast<HWND>(view.native_handle()), WM_KEYDOWN, 'A', 0);
  };
  host.window_visible = [window](bool visible) {
    ShowWindow(window, visible ? SW_SHOWNA : SW_HIDE);
  };
  int result = smoke(argv[1], host, argc > 2 ? argv[2] : "");
  DestroyWindow(window);
  return result;
}
