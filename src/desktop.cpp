#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "internal.hpp"

namespace weapp::desktop::detail {
Json encode(const Value& v) {
  return std::visit(
      [](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          return nullptr;
        else if constexpr (std::is_same_v<T, Value::Array>) {
          Json a = Json::array();
          for (const auto& x : item) a.push_back(encode(x));
          return a;
        } else if constexpr (std::is_same_v<T, Value::Object>) {
          Json a = Json::object();
          for (const auto& [k, x] : item) a[k] = encode(x);
          return a;
        } else if constexpr (std::is_same_v<T, double>) {
          if (!std::isfinite(item))
            throw std::invalid_argument("business numbers must be finite");
          return item;
        } else
          return item;
      },
      v.data);
}
Value decode(const Json& j) {
  if (j.is_null()) return {};
  if (j.is_boolean()) return j.get<bool>();
  if (j.is_number()) return j.get<double>();
  if (j.is_string()) return j.get<std::string>();
  if (j.is_array()) {
    Value::Array a;
    for (const auto& x : j) a.push_back(decode(x));
    return a;
  }
  Value::Object a;
  for (auto i = j.begin(); i != j.end(); ++i) a[i.key()] = decode(i.value());
  return a;
}
Status Buffer::read(int32_t rc, Json* out) {
  try {
    if (!value.data)
      return Status::error("ERR_ABI", "native SDK did not return an envelope");
    auto j = Json::parse(value.data, value.data + value.len);
    if (rc != 0 || !j.value("ok", false)) {
      auto e = j.value("error", Json::object());
      return Status::error(e.value("code", "ERR_NATIVE"),
                           e.value("message", "native operation failed"));
    }
    if (out) *out = j.at("value");
    return {};
  } catch (const std::exception& e) {
    return Status::error("ERR_ABI", e.what());
  }
}
Core::~Core() {
  if (owner != std::this_thread::get_id()) std::terminate();
  loop.reset();
  if (handle) {
    Buffer b;
    b.read(weapp_container_sdk_session_destroy(&handle, &b.value));
    // A Session cannot die while its View owns a shared reference. The fallback
    // destroy is synchronous; explicit close() exposes durable
    // completion/errors.
  }
}
Status Core::check() const {
  if (owner != std::this_thread::get_id())
    return Status::error(
        "ERR_WRONG_THREAD",
        "desktop sessions and views belong to their creating UI thread");
  return {};
}
static Json display(const Metrics& m) {
  return {{"pixelRatio", m.pixel_ratio},
          {"screenWidth", m.screen_width},
          {"screenHeight", m.screen_height},
          {"screenTop", m.screen_top},
          {"statusBarHeight", 0},
          {"deviceOrientation",
           m.screen_width >= m.screen_height ? "landscape" : "portrait"}};
}
Status Core::create_native() {
  if (auto s = check(); !s) return s;
  if (handle) return {};
  if (state.status == SessionState::Closed)
    return Status::error("ERR_CLOSED", "session is closed");
  auto& m = config.metrics;
  if (!m.width || !m.height || !m.screen_width || !m.screen_height ||
      !std::isfinite(m.pixel_ratio) || m.pixel_ratio <= 0 ||
      !std::isfinite(m.screen_top))
    return Status::error("ERR_NOT_LAID_OUT",
                         "a positive native view size and display observation "
                         "are required before load");
  Json j = {{"engine", "quickjs"},
            {"host",
             {{"kind", "desktop"},
              {"platform", platform_name()},
              {"windowWidth", m.width},
              {"windowHeight", m.height},
              {"displayMetrics", display(m)}}}};
  if (!config.session_id.empty()) j["sessionId"] = config.session_id;
  if (!config.storage_key.empty() || !config.storage_root.empty()) {
    j["storageKey"] = config.storage_key;
    j["storageRoot"] = config.storage_root;
  }
  auto bytes = j.dump();
  Buffer b;
  auto s = b.read(weapp_container_sdk_session_new(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &handle,
      &b.value));
  if (!s) return s;
  for (const auto& name : methods)
    if (auto e = call("registerHostMethod", {{"name", name}}); !e) return e;
  return {};
}
Status Core::call(const char* op, const Json& payload, Json* result) {
  if (auto s = check(); !s) return s;
  if (!handle) return Status::error("ERR_NOT_LOADED", "load a pack first");
  try {
    auto bytes = payload.dump();
    Buffer b;
    Json value;
    auto s = b.read(weapp_container_sdk_session_command(
                        handle, reinterpret_cast<const uint8_t*>(op),
                        std::char_traits<char>::length(op),
                        reinterpret_cast<const uint8_t*>(bytes.data()),
                        bytes.size(), &b.value),
                    &value);
    if (!s) return s;
    if (value.is_object() && value.contains("status")) {
      const auto status = value.value("status", "");
      state.status = status == "closed"    ? SessionState::Closed
                     : status == "closing" ? SessionState::Closing
                     : status == "ready"   ? SessionState::Ready
                                           : SessionState::Empty;
      state.revision = value.value("frameRevision", uint64_t(0));
      state.generation = value.value("generation", uint64_t(0));
      presentation_required = value.value("presentationRequired", false);
      state.foreground = value.value("foreground", false);
      state.route = value.contains("page") && value["page"].is_object()
                        ? value["page"].value("route", "")
                        : "";
    }
    if (result) *result = std::move(value);
    return {};
  } catch (const std::exception& e) {
    return Status::error("ERR_INVALID_ARGUMENT", e.what());
  }
}
Status Core::read_state() { return handle ? call("state") : Status{}; }
Status Core::acknowledge_presentation(uint64_t generation, uint64_t revision) {
  // Raster mode already accepts its runtime frame synchronously; platform
  // paint callbacks must not pretend a native GPU surface was attached.
  if (!presentation_required) return {};
  return call("presented", {{"generation", generation}, {"revision", revision}});
}
void Core::report(const Status& s) {
  if (!s)
    pending.push_back(
        {EventType::Error,
         {{"error", Value::Object{{"code", s.code}, {"message", s.message}}}}});
}
static EventType event_type(const std::string& t) {
  static const std::map<std::string, EventType> types = {
      {"ready", EventType::Ready},
      {"navigation", EventType::Navigation},
      {"frame", EventType::Frame},
      {"message", EventType::Message},
      {"hostCall", EventType::HostCall},
      {"hostCallCancelled", EventType::HostCallCancelled},
      {"error", EventType::Error},
      {"storageFlushed", EventType::StorageFlushed},
      {"closed", EventType::Closed}};
  auto i = types.find(t);
  return i == types.end() ? EventType::Unknown : i->second;
}
Status Core::drain() {
  if (delivering)
    return {};  // nested host event loops cannot recursively drain callbacks
  std::vector<Event> events;
  events.swap(pending);
  if (handle) {
    Json a;
    auto s = call("takeEvents", Json::object(), &a);
    if (!s) return s;
    for (auto& j : a) {
      Event e;
      e.type = event_type(j.value("type", ""));
      j.erase("type");
      e.fields = std::get<Value::Object>(decode(j).data);
      events.push_back(std::move(e));
    }
  }
  delivering = true;
  for (const auto& e : events) {
    auto callback = listener;
    if (callback) try {
        callback(e);
      } catch (const std::exception& error) {
        report(Status::error("ERR_CALLBACK", error.what()));
      } catch (...) {
        report(Status::error("ERR_CALLBACK", "host callback threw"));
      }
  }
  delivering = false;
  return {};
}
Status Core::foreground() {
  if (state.status != SessionState::Ready) return {};
  bool visible = requested_foreground && surface_visible;
  if (visible == state.foreground) return {};
  if (!visible) cancel_touches();
  return call("lifecycle", {{"event", visible ? "show" : "hide"}});
}
Status Core::set_metrics(const Metrics& m) {
  if (auto s = check(); !s) return s;
  if (!m.width || !m.height || !m.screen_width || !m.screen_height ||
      !std::isfinite(m.pixel_ratio) || m.pixel_ratio <= 0 ||
      !std::isfinite(m.screen_top))
    return Status::error(
        "ERR_INVALID_ARGUMENT",
        "metrics must contain a positive viewport, screen, and pixel ratio");
  auto old = display(config.metrics);
  old["windowWidth"] = config.metrics.width;
  old["windowHeight"] = config.metrics.height;
  auto j = display(m);
  j["windowWidth"] = m.width;
  j["windowHeight"] = m.height;
  if (old == j) return {};
  if (state.status == SessionState::Closing ||
      state.status == SessionState::Closed)
    return Status::error("ERR_CLOSED", "session is closing or closed");
  if (state.status == SessionState::Ready) {
    auto s = call("windowMetrics", j);
    // Hook failure can follow an accepted update. Always read runtime state;
    // positive valid native observations remain authoritative for presentation.
    Json observed;
    auto state_result = call("state", Json::object(), &observed);
    if (state_result &&
        observed["viewport"].value("width", uint32_t(0)) == m.width &&
        observed["viewport"].value("height", uint32_t(0)) == m.height)
      config.metrics = m;
    return !s ? s : state_result;
  }
  if (handle) {
    Buffer b;
    auto s = b.read(weapp_container_sdk_session_destroy(&handle, &b.value));
    if (!s) return s;
  }
  config.metrics = m;
  return {};
}
Status Core::pointer(const PointerInput& p) {
  if (auto s = check(); !s) return s;
  if (!std::isfinite(p.x) || !std::isfinite(p.y))
    return Status::error("ERR_INVALID_ARGUMENT",
                         "pointer coordinates must be finite");
  if (state.status != SessionState::Ready || !state.foreground)
    return Status::error("ERR_BACKGROUND",
                         "input requires a visible loaded session");
  auto now = std::chrono::steady_clock::now();
  if (p.action == PointerAction::Down)
    touches[p.id] = {p.x, p.y, p.x, p.y, now, false};
  auto i = touches.find(p.id);
  bool tap = false;
  if (i != touches.end()) {
    auto& t = i->second;
    t.x = p.x;
    t.y = p.y;
    if (std::hypot(t.x - t.start_x, t.y - t.start_y) > 10) t.moved = true;
    tap = p.action == PointerAction::Up && !t.moved &&
          now - t.start < std::chrono::milliseconds(500);
  }
  Json point = {{"identifier", p.id},
                {"clientX", p.x},
                {"clientY", p.y},
                {"pageX", p.x},
                {"pageY", p.y}};
  if (p.action == PointerAction::Up || p.action == PointerAction::Cancel)
    touches.erase(p.id);
  Json active = Json::array();
  for (const auto& [id, t] : touches)
    active.push_back({{"identifier", id},
                      {"clientX", t.x},
                      {"clientY", t.y},
                      {"pageX", t.x},
                      {"pageY", t.y}});
  const char* type = p.action == PointerAction::Down   ? "touchstart"
                     : p.action == PointerAction::Move ? "touchmove"
                     : p.action == PointerAction::Up   ? "touchend"
                                                       : "touchcancel";
  Json event = {{"type", type},
                {"x", p.x},
                {"y", p.y},
                {"touches", active},
                {"changedTouches", Json::array({point})},
                {"detail", {{"x", p.x}, {"y", p.y}}}};
  Json events = Json::array({event});
  if (tap)
    events.push_back({{"type", "tap"},
                      {"x", p.x},
                      {"y", p.y},
                      {"detail", {{"x", p.x}, {"y", p.y}}}});
  return call("input", {{"events", events}});
}
void Core::cancel_touches() {
  auto old = touches;
  for (const auto& [id, t] : old)
    report(pointer({PointerAction::Cancel, id, t.x, t.y}));
  touches.clear();
}
Status Core::wheel(const WheelInput& e) {
  return call("input",
              {{"events", Json::array({{{"type", "wheel"},
                                        {"x", e.x},
                                        {"y", e.y},
                                        {"deltaX", e.delta_x},
                                        {"deltaY", e.delta_y},
                                        {"deltaMode", 0},
                                        {"detail", Json::object()}}})}});
}
Status Core::key(const KeyInput& e) {
  return call("input",
              {{"events", Json::array({{{"type", e.down ? "keydown" : "keyup"},
                                        {"detail",
                                         {{"key", e.key},
                                          {"code", e.code},
                                          {"repeat", e.repeat},
                                          {"shiftKey", e.shift},
                                          {"ctrlKey", e.control},
                                          {"altKey", e.alt},
                                          {"metaKey", e.meta}}}}})}});
}
void Raster::clear() {
  if (frame) {
    Buffer b;
    b.read(weapp_container_sdk_frame_release(&frame, &b.value));
  }
  info = {};
  bgra.clear();
  generation = 0;
}
Status Raster::update(Core& c, bool& changed) {
  changed = false;
  auto w = uint32_t(
      std::lround(c.config.metrics.width * c.config.metrics.pixel_ratio));
  auto h = uint32_t(
      std::lround(c.config.metrics.height * c.config.metrics.pixel_ratio));
  if (frame && generation == c.state.generation && info.revision == c.state.revision && info.width == w &&
      info.height == h)
    return {};
  const weapp_container_sdk_frame* next = nullptr;
  Buffer b;
  auto s = b.read(weapp_container_sdk_frame_acquire(c.handle, &next, &b.value));
  if (!s) return s;
  weapp_container_sdk_frame_info_data data{};
  Buffer bi;
  s = bi.read(weapp_container_sdk_frame_info(next, &data, &bi.value));
  if (!s) {
    Buffer br;
    br.read(weapp_container_sdk_frame_release(&next, &br.value));
    return s;
  }
  clear();
  frame = next;
  info = data;
  generation = c.state.generation;
  bgra.resize(info.len);
  for (size_t k = 0; k + 3 < info.len; k += 4) {
    bgra[k] = info.data[k + 2];
    bgra[k + 1] = info.data[k + 1];
    bgra[k + 2] = info.data[k];
    bgra[k + 3] = info.data[k + 3];
  }
  changed = true;
  return {};
}
}  // namespace weapp::desktop::detail

namespace weapp::desktop {
using detail::Buffer;
using detail::Json;
struct Frame::Impl {
  const weapp_container_sdk_frame* handle = nullptr;
  weapp_container_sdk_frame_info_data info{};
  ~Impl() {
    if (handle) {
      Buffer b;
      b.read(weapp_container_sdk_frame_release(&handle, &b.value));
    }
  }
};
Frame::Frame() = default;
Frame::~Frame() = default;
Frame::Frame(const Frame&) = default;
Frame& Frame::operator=(const Frame&) = default;
Frame::Frame(Frame&&) noexcept = default;
Frame& Frame::operator=(Frame&&) noexcept = default;
const uint8_t* Frame::pixels() const {
  return impl_ ? impl_->info.data : nullptr;
}
size_t Frame::byte_size() const { return impl_ ? impl_->info.len : 0; }
uint32_t Frame::width() const { return impl_ ? impl_->info.width : 0; }
uint32_t Frame::height() const { return impl_ ? impl_->info.height : 0; }
uint32_t Frame::stride() const { return impl_ ? impl_->info.stride : 0; }
uint64_t Frame::revision() const { return impl_ ? impl_->info.revision : 0; }
Frame::operator bool() const { return bool(impl_); }
Session::Session(std::unique_ptr<detail::Core> c) : core_(std::move(c)) {}
Session::~Session() = default;
std::shared_ptr<Session> Session::create(const Config& config, Status& status) {
  auto c = std::make_unique<detail::Core>();
  c->config = config;
  auto s = std::shared_ptr<Session>(new Session(std::move(c)));
  s->core_->loop = detail::create_loop(
      [weak = std::weak_ptr<Session>(s)] {
        if (auto live = weak.lock()) live->core_->report(live->pump());
      },
      status);
  if (!status || !s->core_->loop) return {};
  return s;
}
Status Session::load(const std::string& path, const LoadOptions& options) {
  if (auto s = core_->check(); !s) return s;
  if (core_->state.status == SessionState::Closed ||
      core_->state.status == SessionState::Closing)
    return Status::error("ERR_CLOSED", "session is closing or closed");
  try {
    auto payload =
        Json{{"packPath", path}, {"options", detail::encode(options.options)}};
    if (!options.route.empty()) payload["route"] = options.route;
    if (auto s = core_->create_native(); !s) return s;
    if (core_->presenter) {
      auto detached = core_->presenter->detach();
      if (!detached) return detached;
    }
    auto result = core_->call("load", payload);
    core_->read_state();
    if (core_->state.status == SessionState::Ready) {
      auto visibility = core_->foreground();
      if (!visibility && result) result = visibility;
      if (core_->presenter) {
        auto attachment = core_->presenter->attach();
        if (!attachment && result) result = attachment;
      }
    }
    return result;
  } catch (const std::exception& e) {
    return Status::error("ERR_INVALID_ARGUMENT", e.what());
  }
}
Status Session::navigate(const std::string& r, const Value::Object& q,
                         bool replace) {
  try {
    return core_->call(
        "navigate",
        {{"route", r}, {"query", detail::encode(q)}, {"replace", replace}});
  } catch (const std::exception& e) {
    return Status::error("ERR_INVALID_ARGUMENT", e.what());
  }
}
Status Session::back() { return core_->call("back"); }
Status Session::set_foreground(bool visible) {
  if (auto s = core_->check(); !s) return s;
  core_->requested_foreground = visible;
  return core_->foreground();
}
Status Session::set_metrics(const Metrics& m) { return core_->set_metrics(m); }
Status Session::send_message(const Value& value) {
  try {
    return core_->call("sendMessage", {{"message", detail::encode(value)}});
  } catch (const std::exception& e) {
    return Status::error("ERR_INVALID_ARGUMENT", e.what());
  }
}
Status Session::register_host_method(const std::string& name) {
  if (auto s = core_->check(); !s) return s;
  if (name.empty() || name.rfind("wx.", 0) == 0)
    return Status::error("ERR_INVALID_ARGUMENT",
                         "host method must be nonempty and cannot use wx.*");
  if (core_->state.status == SessionState::Closed ||
      core_->state.status == SessionState::Closing)
    return Status::error("ERR_CLOSED", "session is closing or closed");
  if (core_->handle) {
    auto s = core_->call("registerHostMethod", {{"name", name}});
    if (!s) return s;
  }
  core_->methods.insert(name);
  return {};
}
Status Session::complete_host_call(const std::string& id, const Value& result) {
  try {
    return core_->call("completeHostCall",
                       {{"callId", id}, {"result", detail::encode(result)}});
  } catch (const std::exception& e) {
    return Status::error("ERR_INVALID_ARGUMENT", e.what());
  }
}
Status Session::fail_host_call(const std::string& id,
                               const std::string& message) {
  return core_->call("completeHostCall", {{"callId", id},
                                          {"error",
                                           {{"code", "invalid_request"},
                                            {"message", message},
                                            {"retryable", false}}}});
}
Status Session::pointer(const PointerInput& e) { return core_->pointer(e); }
Status Session::wheel(const WheelInput& e) { return core_->wheel(e); }
Status Session::key(const KeyInput& e) { return core_->key(e); }
Status Session::flush_storage() { return core_->call("flushStorage"); }
Status Session::close() {
  if (auto s = core_->check(); !s) return s;
  if (core_->state.status == SessionState::Closed) return {};
  if (core_->presenter) {
    auto detached = core_->presenter->detach();
    if (!detached) return detached;
  }
  core_->touches.clear();
  if (!core_->handle) {
    core_->state.status = SessionState::Closed;
    core_->pending.push_back({EventType::Closed, {}});
    return {};
  }
  auto s = core_->call("close");
  core_->read_state();
  return s;
}
Status Session::pump() {
  auto keep = shared_from_this();
  if (auto s = core_->check(); !s) return s;
  if (core_->presenter) core_->presenter->refresh();
  Status s;
  if (core_->handle) {
    s = core_->call("pump");
    core_->read_state();
  }
  if (core_->presenter && core_->state.status == SessionState::Ready &&
      core_->state.foreground) {
    auto draw = core_->presenter->present();
    if (!draw && s) s = draw;
  }
  auto events = core_->drain();
  return !s ? s : events;
}
Status Session::snapshot(Frame& out) {
  if (auto s = core_->check(); !s) return s;
  if (!core_->handle)
    return Status::error("ERR_NOT_LOADED", "load a pack first");
  auto next = std::make_shared<Frame::Impl>();
  Buffer b;
  auto s = b.read(weapp_container_sdk_frame_acquire(core_->handle,
                                                    &next->handle, &b.value));
  if (!s) return s;
  Buffer bi;
  s = bi.read(
      weapp_container_sdk_frame_info(next->handle, &next->info, &bi.value));
  if (!s) return s;
  out.impl_ = std::move(next);
  return {};
}
State Session::state() const {
  if (auto s = core_->check(); !s) throw std::logic_error(s.message);
  return core_->state;
}
Status Session::capabilities(Capabilities& c) {
  Json j;
  auto s = core_->call("capabilities", Json::object(), &j);
  if (!s) return s;
  auto b = j.at("build");
  c.quickjs = b.value("quickjs", false);
  c.skia_cpu = b.value("skiaCpu", false);
  c.metal = b.value("metal", false);
  auto r = j.at("runtime");
  c.gpu_presented = r.value("gpuSurface", false);
  c.renderer =
      r["renderer"].is_string() ? r["renderer"].get<std::string>() : "";
  c.native_apis.clear();
  for (auto i = j["nativeApis"].begin(); i != j["nativeApis"].end(); ++i)
    c.native_apis.push_back(i.key());
  return {};
}
void Session::on_event(std::function<void(const Event&)> listener) {
  if (auto s = core_->check(); !s) throw std::logic_error(s.message);
  core_->listener = std::move(listener);
}
View::View(std::shared_ptr<Session> s, std::unique_ptr<detail::Presenter> p)
    : session_(std::move(s)), presenter_(std::move(p)) {}
static bool valid_rect(const Rect& r) {
  return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
         std::isfinite(r.height) && std::abs(r.x) <= 2147483647.0 &&
         std::abs(r.y) <= 2147483647.0 && r.width >= 0 && r.height >= 0 &&
         r.width <= 32767 && r.height <= 32767;
}
std::unique_ptr<View> View::create(std::shared_ptr<Session> s, void* parent,
                                   const Rect& bounds, Status& status) {
  if (!s) {
    status = Status::error("ERR_INVALID_ARGUMENT", "session is required");
    return {};
  }
  if (!valid_rect(bounds)) {
    status = Status::error("ERR_INVALID_ARGUMENT",
                           "native view bounds must be finite, nonnegative and "
                           "within native coordinate limits");
    return {};
  }
  auto& c = *s->core_;
  status = c.check();
  if (!status) return {};
  if (c.presenter) {
    status = Status::error("ERR_SURFACE_ATTACHED",
                           "session already owns a native view");
    return {};
  }
  if (c.state.status == SessionState::Closed ||
      c.state.status == SessionState::Closing) {
    status = Status::error("ERR_CLOSED", "session is closing or closed");
    return {};
  }
  auto p = detail::create_presenter(c);
  c.presenter = p.get();
  status = p->initialize(parent, bounds);
  if (status && c.state.status == SessionState::Ready) status = p->attach();
  if (!status) {
    c.report(p->detach());
    c.presenter = nullptr;
    return {};
  }
  return std::unique_ptr<View>(new View(std::move(s), std::move(p)));
}
View::~View() {
  if (auto s = session_->core_->check(); !s) std::terminate();
  session_->core_->report(presenter_->detach());
  session_->core_->presenter = nullptr;
  presenter_.reset();
  session_->core_->surface_visible = false;
  session_->core_->report(session_->core_->foreground());
}
void* View::native_handle() const { return presenter_->native_handle(); }
Status View::set_bounds(const Rect& r) {
  if (!valid_rect(r))
    return Status::error("ERR_INVALID_ARGUMENT", "invalid native view bounds");
  if (auto s = session_->core_->check(); !s) return s;
  return presenter_->set_bounds(r);
}
Status View::set_visible(bool v) {
  if (auto s = session_->core_->check(); !s) return s;
  return presenter_->set_visible(v);
}
}  // namespace weapp::desktop
