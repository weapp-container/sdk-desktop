#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace weapp::desktop {
// Business data only. No serialized command names or JSON envelopes cross this
// API.
struct Value {
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value>;
  using Data =
      std::variant<std::monostate, bool, double, std::string, Array, Object>;
  Data data;
  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool v) : data(v) {}
  Value(int v) : data(double(v)) {}
  Value(double v) : data(v) {}
  Value(const char* v) : data(std::string(v)) {}
  Value(std::string v) : data(std::move(v)) {}
  Value(Array v) : data(std::move(v)) {}
  Value(Object v) : data(std::move(v)) {}
};
struct Status {
  std::string code, message;
  explicit operator bool() const { return code.empty(); }
  static Status error(std::string code, std::string message) {
    return {std::move(code), std::move(message)};
  }
};
struct Metrics {
  uint32_t width = 0, height = 0;
  double pixel_ratio = 1;
  uint32_t screen_width = 0, screen_height = 0;
  double screen_top = 0;
};
struct Config {
  // Optional when a View supplies actual metrics before load().
  Metrics metrics;
  std::string storage_key, storage_root, session_id;
};
struct LoadOptions {
  std::string route;
  Value::Object options;
};
enum class SessionState { Empty, Ready, Closing, Closed };
struct State {
  SessionState status = SessionState::Empty;
  uint64_t generation = 0, revision = 0;
  std::string route;
  bool foreground = false;
};
enum class EventType {
  Ready,
  Navigation,
  Frame,
  Message,
  HostCall,
  HostCallCancelled,
  Error,
  StorageFlushed,
  Closed,
  Unknown
};
struct Event {
  EventType type = EventType::Unknown;
  // Typed tree containing business payload, event metadata, or error details.
  Value::Object fields;
};
struct Capabilities {
  bool quickjs = false, skia_cpu = false, metal = false;
  bool gpu_presented = false;
  std::string renderer;
  std::vector<std::string> native_apis;
};
enum class PointerAction { Down, Move, Up, Cancel };
struct PointerInput {
  PointerAction action;
  uint32_t id = 0;
  double x = 0, y = 0;
};
struct WheelInput {
  double x = 0, y = 0, delta_x = 0, delta_y = 0;
};
struct KeyInput {
  bool down = true, repeat = false;
  std::string key, code;
  bool shift = false, control = false, alt = false, meta = false;
};
class Frame {
 public:
  Frame();
  ~Frame();
  Frame(const Frame&);
  Frame& operator=(const Frame&);
  Frame(Frame&&) noexcept;
  Frame& operator=(Frame&&) noexcept;
  const uint8_t* pixels() const;
  size_t byte_size() const;
  uint32_t width() const;
  uint32_t height() const;
  uint32_t stride() const;
  uint64_t revision() const;
  explicit operator bool() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  friend class Session;
};
class View;
namespace detail {
struct Core;
class Presenter;
}  // namespace detail
class Session final : public std::enable_shared_from_this<Session> {
 public:
  static std::shared_ptr<Session> create(const Config&, Status&);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Status load(const std::string& directory, const LoadOptions& = {});
  Status navigate(const std::string& route, const Value::Object& query = {},
                  bool replace = false);
  Status back();
  Status set_foreground(bool);
  Status set_metrics(const Metrics&);
  Status send_message(const Value&);
  Status register_host_method(const std::string&);
  Status complete_host_call(const std::string& call_id, const Value&);
  Status fail_host_call(const std::string& call_id, const std::string& message);
  Status pointer(const PointerInput&);
  Status wheel(const WheelInput&);
  Status key(const KeyInput&);
  Status flush_storage();
  // Asynchronous. Keep the host event loop alive until EventType::Closed.
  Status close();
  // Automatically called by a platform timer, including when hidden/closing.
  Status pump();
  Status snapshot(Frame&);
  State state() const;
  Status capabilities(Capabilities&);
  void on_event(std::function<void(const Event&)>);

 private:
  explicit Session(std::unique_ptr<detail::Core>);
  std::unique_ptr<detail::Core> core_;
  friend class View;
  friend class detail::Presenter;
};
struct Rect {
  double x = 0, y = 0, width = 0, height = 0;
};
class View final {
 public:
  // parent: NSView*, HWND, or GtkContainer*. SDK creates only a child view.
  // Windows requires a parent; macOS/GTK permit null for subsequent host
  // insertion.
  static std::unique_ptr<View> create(std::shared_ptr<Session>, void* parent,
                                      const Rect&, Status&);
  ~View();
  View(const View&) = delete;
  View& operator=(const View&) = delete;
  void* native_handle() const;
  Status set_bounds(const Rect&);
  Status set_visible(bool);

 private:
  View(std::shared_ptr<Session>, std::unique_ptr<detail::Presenter>);
  std::shared_ptr<Session> session_;
  std::unique_ptr<detail::Presenter> presenter_;
};
}  // namespace weapp::desktop
