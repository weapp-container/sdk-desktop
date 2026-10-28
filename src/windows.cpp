#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <GL/gl.h>
#include <algorithm>
#include <cctype>

#include <cmath>
#include <cstring>

#include "internal.hpp"
#include "weapp_container_sdk_gl.h"

namespace weapp::desktop::detail {
const char* platform_name() { return "windows"; }
static Status win_error(const char* operation) {
  return Status::error("ERR_WIN32", std::string(operation) + ": " +
                                        std::to_string(GetLastError()));
}
class WinLoop final : public Loop {
 public:
  static LRESULT CALLBACK proc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
    auto* self =
        reinterpret_cast<WinLoop*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<WinLoop*>(
          reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_TIMER && self) {
      auto callback = self->tick_;
      callback();
      return 0;
    }
    return DefWindowProcW(w, message, wp, lp);
  }
  explicit WinLoop(std::function<void()> tick) : tick_(std::move(tick)) {}
  Status start() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WeappDesktopServiceV1";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      return win_error("RegisterClassW");
    window_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, wc.hInstance, this);
    if (!window_) return win_error("Create service window");
    if (!SetTimer(window_, 1, 16, nullptr)) return win_error("SetTimer");
    return {};
  }
  ~WinLoop() override {
    if (window_) {
      KillTimer(window_, 1);
      DestroyWindow(window_);
    }
  }

 private:
  HWND window_ = nullptr;
  std::function<void()> tick_;
};
std::unique_ptr<Loop> create_loop(std::function<void()> tick, Status& status) {
  auto loop = std::make_unique<WinLoop>(std::move(tick));
  status = loop->start();
  if (!status) return {};
  return loop;
}
class WinPresenter final : public Presenter {
 public:
  explicit WinPresenter(Core& c) : Presenter(c) {}
  ~WinPresenter() override {
    core_.report(detach());
    if (context_) {
      wglDeleteContext(context_);
      context_ = nullptr;
    }
    if (dc_ && window_) ReleaseDC(window_, dc_);
    dc_ = nullptr;
    if (window_) DestroyWindow(window_);
  }
  Status initialize(void* parent, const Rect& r) override {
    if (!parent || !IsWindow(static_cast<HWND>(parent)))
      return Status::error("ERR_INVALID_ARGUMENT",
                           "Windows adapter needs a live parent HWND");
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WeappDesktopViewV1";
    wc.style = CS_OWNDC;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      return win_error("RegisterClassW");
    double scale = GetDpiForWindow(static_cast<HWND>(parent)) / 96.0;
    window_ = CreateWindowExW(
        0, wc.lpszClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
        int(std::lround(r.x * scale)), int(std::lround(r.y * scale)),
        std::max(0, int(std::lround(r.width * scale))),
        std::max(0, int(std::lround(r.height * scale))),
        static_cast<HWND>(parent), nullptr, wc.hInstance, this);
    if (!window_) return win_error("Create child HWND");
    if (core_.config.require_gpu) {
      auto gpu = initialize_gpu();
      if (!gpu) return gpu;
    }
    update();
    return {};
  }
  void* native_handle() const override { return window_; }
  Status set_bounds(const Rect& r) override {
    if (!window_)
      return Status::error("ERR_VIEW_DESTROYED",
                           "native HWND was destroyed by its host");
    if (!std::isfinite(r.width) || !std::isfinite(r.height) ||
        r.width > 32767 || r.height > 32767)
      return Status::error("ERR_INVALID_ARGUMENT", "invalid child HWND bounds");
    double d = GetDpiForWindow(window_) / 96.0;
    if (!SetWindowPos(window_, nullptr, int(std::lround(r.x * d)),
                      int(std::lround(r.y * d)),
                      std::max(0, int(std::lround(r.width * d))),
                      std::max(0, int(std::lround(r.height * d))),
                      SWP_NOZORDER | SWP_NOACTIVATE))
      return win_error("SetWindowPos");
    update();
    return {};
  }
  Status set_visible(bool show) override {
    if (!window_)
      return Status::error("ERR_VIEW_DESTROYED", "native HWND was destroyed");
    ShowWindow(window_, show ? SW_SHOWNA : SW_HIDE);
    update();
    return {};
  }
  Status attach() override {
    if (core_.config.require_gpu) {
      if (!context_ || !dc_)
        return Status::error("ERR_RENDER", "hardware OpenGL context unavailable");
      Current current(dc_, context_);
      if (!current) return win_error("Make OpenGL context current");
      Buffer result;
      auto status = result.read(weapp_container_sdk_gl_attach_with_resolver(
          core_.handle, resolve_gl, nullptr, &result.value));
      if (!status) return status;
      gpu_attached_ = true;
    }
    attached_ = true;
    return {};
  }
  Status detach() override {
    attached_ = false;
    if (context_ && gpu_attached_) {
      Current current(dc_, context_);
      Buffer result;
      auto status = result.read(current
          ? weapp_container_sdk_gles_detach(core_.handle, &result.value)
          : weapp_container_sdk_gles_abandon(core_.handle, &result.value));
      if (!status) return status;
      gpu_attached_ = false;
    }
    if (context_ && pending_) {
      Current current(dc_, context_);
      if (current) delete_sync_(pending_);
      pending_ = nullptr;
    }
    gpu_presented_ = false;
    has_presented_ = false;
    raster_.clear();
    if (bitmap_) {
      DeleteObject(bitmap_);
      bitmap_ = nullptr;
    }
    return {};
  }
  Status present() override {
    update();
    if (!window_ || !attached_ || !core_.surface_visible) return {};
    if (core_.config.require_gpu) return present_gpu();
    bool changed = false;
    auto s = raster_.update(core_, changed);
    if (!s || (!changed && bitmap_)) return s;
    if (bitmap_) {
      DeleteObject(bitmap_);
      bitmap_ = nullptr;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = LONG(raster_.info.width);
    info.bmiHeader.biHeight = -LONG(raster_.info.height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    bitmap_ =
        CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap_) return win_error("CreateDIBSection");
    std::memcpy(bits, raster_.bgra.data(), raster_.bgra.size());
    InvalidateRect(window_, nullptr, FALSE);
    return {};
  }
  void refresh() override { update(); }
  void update() {
    if (!window_) return;
    RECT r{};
    GetClientRect(window_, &r);
    visibility(r.right > 0 && r.bottom > 0 && IsWindowVisible(window_) &&
               !IsIconic(GetAncestor(window_, GA_ROOT)));
    if (r.right <= 0 || r.bottom <= 0 ||
        core_.state.status == SessionState::Closed ||
        core_.state.status == SessionState::Closing)
      return;
    double d = GetDpiForWindow(window_) / 96.0;
    if (d <= 0) d = 1;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                         &mi))
      return;
    POINT origin{};
    ClientToScreen(window_, &origin);
    Metrics m{
        uint32_t(std::max(1L, std::lround(r.right / d))),
        uint32_t(std::max(1L, std::lround(r.bottom / d))),
        d,
        uint32_t(std::lround((mi.rcMonitor.right - mi.rcMonitor.left) / d)),
        uint32_t(std::lround((mi.rcMonitor.bottom - mi.rcMonitor.top) / d)),
        (origin.y - mi.rcMonitor.top) / d};
    core_.report(metrics(m));
  }

 private:
  // No shared client memory is referenced by GPU commands. Normal detach
  // deletes GL objects while their context is current; the driver retains
  // queued references until completion. Pending receipts are always revoked.
  struct Current {
    HDC old_dc = wglGetCurrentDC();
    HGLRC old_context = wglGetCurrentContext();
    bool active;
    Current(HDC dc, HGLRC context) : active(wglMakeCurrent(dc, context) != FALSE) {}
    ~Current() { if (active) wglMakeCurrent(old_dc, old_context); }
    explicit operator bool() const { return active; }
  };
  using Sync = void*;
  using Fence = Sync(APIENTRY*)(GLenum, GLbitfield);
  using Wait = GLenum(APIENTRY*)(Sync, GLbitfield, uint64_t);
  using DeleteSync = void(APIENTRY*)(Sync);
  static const void* resolve_gl(void*, const char* name) {
    auto pointer = wglGetProcAddress(name);
    auto value = reinterpret_cast<uintptr_t>(pointer);
    if (!pointer || value <= 3 || value == uintptr_t(-1))
      pointer = GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name);
    return reinterpret_cast<const void*>(pointer);
  }
  Status initialize_gpu() {
    dc_ = GetDC(window_);
    if (!dc_) return win_error("Get OpenGL DC");
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = static_cast<WORD>(sizeof(format));
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cAlphaBits = 8;
    auto index = ChoosePixelFormat(dc_, &format);
    if (!index || !SetPixelFormat(dc_, index, &format))
      return win_error("Set OpenGL pixel format");
    auto bootstrap = wglCreateContext(dc_);
    if (!bootstrap) return win_error("Create bootstrap OpenGL context");
    using Create = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
    HGLRC next = nullptr;
    {
      Current current(dc_, bootstrap);
      if (current) {
        auto create = reinterpret_cast<Create>(
            const_cast<void*>(resolve_gl(nullptr, "wglCreateContextAttribsARB")));
        const int attributes[] = {0x2091, 3, 0x2092, 3, 0x9126, 1, 0};
        if (create) next = create(dc_, nullptr, attributes);
      }
    }
    wglDeleteContext(bootstrap);
    if (!next)
      return Status::error("ERR_RENDER", "hardware OpenGL 3.3 core is required");
    context_ = next;
    Current current(dc_, context_);
    if (!current) return win_error("Make OpenGL 3.3 context current");
    const auto* bytes = glGetString(GL_RENDERER);
    std::string renderer = bytes ? reinterpret_cast<const char*>(bytes) : "";
    std::transform(renderer.begin(), renderer.end(), renderer.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (renderer.empty() || renderer.find("gdi generic") != std::string::npos ||
        renderer.find("llvmpipe") != std::string::npos ||
        renderer.find("softpipe") != std::string::npos ||
        renderer.find("swiftshader") != std::string::npos ||
        renderer.find("software") != std::string::npos)
      return Status::error("ERR_RENDER", "software OpenGL does not satisfy GPU-only preview");
    fence_ = reinterpret_cast<Fence>(const_cast<void*>(resolve_gl(nullptr, "glFenceSync")));
    wait_ = reinterpret_cast<Wait>(const_cast<void*>(resolve_gl(nullptr, "glClientWaitSync")));
    delete_sync_ = reinterpret_cast<DeleteSync>(const_cast<void*>(resolve_gl(nullptr, "glDeleteSync")));
    if (!fence_ || !wait_ || !delete_sync_)
      return Status::error("ERR_RENDER", "OpenGL fence functions are unavailable");
    return {};
  }
  Status present_gpu() {
    Current current(dc_, context_);
    if (!current) return win_error("Make OpenGL context current");
    if (pending_) {
      auto result = wait_(pending_, 0, 0);  // Poll only; never wait on the UI thread.
      if (result == 0x911B) return {};       // GL_TIMEOUT_EXPIRED
      delete_sync_(pending_);
      pending_ = nullptr;
      if (result == 0x911D)
        return Status::error("ERR_RENDER", "OpenGL completion fence failed");
      if (result != 0x911A && result != 0x911C)
        return Status::error("ERR_RENDER", "invalid OpenGL fence result");
      auto receipt = core_.acknowledge_presentation(pending_generation_, pending_revision_);
      if (!receipt && receipt.code != "ERR_CANCELLED") return receipt;
      gpu_presented_ = bool(receipt);
      presented_generation_ = pending_generation_;
      presented_revision_ = pending_revision_;
    }
    RECT bounds{};
    if (!GetClientRect(window_, &bounds)) return win_error("Read OpenGL drawable bounds");
    const auto width = uint32_t(std::max(0L, bounds.right));
    const auto height = uint32_t(std::max(0L, bounds.bottom));
    if (!width || !height) return {};
    if (gpu_presented_ && presented_generation_ == core_.state.generation &&
        presented_revision_ == core_.state.revision && gpu_width_ == width && gpu_height_ == height)
      return {};
    Buffer buffer;
    auto status = buffer.read(weapp_container_sdk_gles_present(
        core_.handle, 0, width, height, &buffer.value));
    if (!status) return status;
    pending_ = fence_(0x9117, 0);  // GL_SYNC_GPU_COMMANDS_COMPLETE
    if (!pending_) return Status::error("ERR_RENDER", "OpenGL fence allocation failed");
    glFlush();
    if (!SwapBuffers(dc_)) {
      delete_sync_(pending_);
      pending_ = nullptr;
      return win_error("Swap OpenGL buffers");
    }
    pending_generation_ = core_.state.generation;
    pending_revision_ = core_.state.revision;
    gpu_width_ = width;
    gpu_height_ = height;
    return {};
  }
  void pointer(PointerAction action, UINT32 id, POINT p) {
    double d = GetDpiForWindow(window_) / 96.0;
    core_.report(core_.pointer({action, id, p.x / d, p.y / d}));
  }
  void paint() {
    if (core_.config.require_gpu) {
      ValidateRect(window_, nullptr);
      core_.report(present());
      return;
    }
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window_, &ps);
    RECT r{};
    GetClientRect(window_, &r);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    bool drawn = false;
    if (bitmap_) {
      HDC memory = CreateCompatibleDC(dc);
      auto old = SelectObject(memory, bitmap_);
      BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
      drawn = AlphaBlend(dc, 0, 0, r.right, r.bottom, memory, 0, 0,
                         int(raster_.info.width), int(raster_.info.height), blend) != FALSE;
      SelectObject(memory, old);
      DeleteDC(memory);
    }
    const bool ended = EndPaint(window_, &ps) != FALSE;
    if (drawn && ended && GdiFlush() && attached_ && core_.surface_visible &&
        (!has_presented_ || presented_generation_ != raster_.generation ||
         presented_revision_ != raster_.info.revision)) {
      auto receipt = core_.acknowledge_presentation(raster_.generation, raster_.info.revision);
      if (receipt) {
        presented_generation_ = raster_.generation;
        presented_revision_ = raster_.info.revision;
        has_presented_ = true;
      } else if (receipt.code != "ERR_CANCELLED") core_.report(receipt);
    }
  }
  void key(UINT message, WPARAM code, LPARAM flags) {
    std::string k;
    switch (code) {
      case VK_LEFT:
        k = "ArrowLeft";
        break;
      case VK_RIGHT:
        k = "ArrowRight";
        break;
      case VK_UP:
        k = "ArrowUp";
        break;
      case VK_DOWN:
        k = "ArrowDown";
        break;
      case VK_RETURN:
        k = "Enter";
        break;
      case VK_ESCAPE:
        k = "Escape";
        break;
      case VK_BACK:
        k = "Backspace";
        break;
      case VK_TAB:
        k = "Tab";
        break;
      case VK_SPACE:
        k = " ";
        break;
      default:
        if (code >= 32 && code < 127)
          k = char(code);
        else
          k = "Native" + std::to_string(code);
    }
    core_.report(core_.key(
        {message == WM_KEYDOWN || message == WM_SYSKEYDOWN,
         bool(flags & (1LL << 30)), k, "Native" + std::to_string(code),
         bool(GetKeyState(VK_SHIFT) & 0x8000),
         bool(GetKeyState(VK_CONTROL) & 0x8000),
         bool(GetKeyState(VK_MENU) & 0x8000),
         bool((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)}));
  }
  static LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self =
        reinterpret_cast<WinPresenter*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = static_cast<WinPresenter*>(
          reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      self->window_ = w;
      SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(w, msg, wp, lp);
    switch (msg) {
      case WM_PAINT:
        self->paint();
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_SIZE:
      case WM_SHOWWINDOW:
      case WM_DPICHANGED_AFTERPARENT:
        self->update();
        return 0;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_MOUSEMOVE: {
        if ((GetMessageExtraInfo() & 0xffffff00) == 0xff515700)
          return 0;  // promoted touch mouse event
        if (msg == WM_MOUSEMOVE && !(wp & MK_LBUTTON)) return 0;
        if (msg == WM_LBUTTONDOWN) {
          SetFocus(w);
          SetCapture(w);
        }
        self->pointer(msg == WM_LBUTTONDOWN ? PointerAction::Down
                      : msg == WM_LBUTTONUP ? PointerAction::Up
                                            : PointerAction::Move,
                      0, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (msg == WM_LBUTTONUP && GetCapture() == w) ReleaseCapture();
        return 0;
      }
      case WM_POINTERDOWN:
      case WM_POINTERUPDATE:
      case WM_POINTERUP: {
        auto id = GET_POINTERID_WPARAM(wp);
        POINTER_INFO info{};
        if (!GetPointerInfo(id, &info)) return 0;
        if (info.pointerType == PT_MOUSE) return DefWindowProcW(w, msg, wp, lp);
        POINT p = info.ptPixelLocation;
        ScreenToClient(w, &p);
        if (msg == WM_POINTERDOWN) SetFocus(w);
        self->pointer(msg == WM_POINTERDOWN ? PointerAction::Down
                      : msg == WM_POINTERUP ? PointerAction::Up
                                            : PointerAction::Move,
                      id, p);
        return 0;
      }
      case WM_POINTERCAPTURECHANGED:
      case WM_CAPTURECHANGED:
      case WM_KILLFOCUS:
        self->core_.cancel_touches();
        return 0;
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(w, &p);
        double d = GetDpiForWindow(w) / 96.0;
        double delta = double(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 40;
        self->core_.report(self->core_.wheel(
            {p.x / d, p.y / d, msg == WM_MOUSEHWHEEL ? delta : 0,
             msg == WM_MOUSEWHEEL ? -delta : 0}));
        return 0;
      }
      case WM_KEYDOWN:
      case WM_KEYUP:
      case WM_SYSKEYDOWN:
      case WM_SYSKEYUP:
        self->key(msg, wp, lp);
        return 0;
      case WM_NCDESTROY:
        self->detach();
        self->visibility(false);
        self->window_ = nullptr;
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(w, msg, wp, lp);
  }
  HWND window_ = nullptr;
  HDC dc_ = nullptr;
  HGLRC context_ = nullptr;
  Fence fence_ = nullptr;
  Wait wait_ = nullptr;
  DeleteSync delete_sync_ = nullptr;
  Sync pending_ = nullptr;
  bool gpu_attached_ = false, gpu_presented_ = false;
  uint64_t pending_generation_ = 0, pending_revision_ = 0;
  uint32_t gpu_width_ = 0, gpu_height_ = 0;
  HBITMAP bitmap_ = nullptr;
  Raster raster_;
  bool attached_ = false;
  bool has_presented_ = false;
  uint64_t presented_generation_ = 0, presented_revision_ = 0;
};
std::unique_ptr<Presenter> create_presenter(Core& core) {
  return std::make_unique<WinPresenter>(core);
}
}  // namespace weapp::desktop::detail
