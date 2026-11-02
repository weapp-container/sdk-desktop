#pragma once
#include <chrono>
#include <set>
#include <thread>

#include "nlohmann/json.hpp"
#include "weapp/desktop.hpp"
#include "weapp/device_apis.hpp"
#include "weapp_container_sdk.h"

namespace weapp::desktop::detail {
using Json = nlohmann::json;
Json encode(const Value&);
Value decode(const Json&);
struct Buffer {
  weapp_container_sdk_buffer value{};
  ~Buffer() { weapp_container_sdk_buffer_free(&value); }
  Status read(int32_t, Json* = nullptr);
};
class Loop {
 public:
  virtual ~Loop() = default;
};
std::unique_ptr<Loop> create_loop(std::function<void()>, Status&);
const char* platform_name();
class Presenter;
struct Core {
  Config config;
  std::thread::id owner = std::this_thread::get_id();
  weapp_container_sdk_session* handle = nullptr;
  bool owns_handle = true;
  std::function<void(const Status&)> error_sink;
  std::unique_ptr<Loop> loop;
  Presenter* presenter = nullptr;
  std::unique_ptr<DeviceApis> devices;
  State state;
  bool requested_foreground = true, surface_visible = true, delivering = false;
  bool presentation_required = false;
  bool presentation_enabled = false;
  bool input_enabled = false;
  std::set<std::string> methods;
  std::function<void(const Event&)> listener;
  std::vector<Event> pending;
  struct Touch {
    double x, y, start_x, start_y;
    std::chrono::steady_clock::time_point start;
    bool moved = false;
  };
  std::map<uint32_t, Touch> touches;
  ~Core();
  Status check() const;
  Status create_native();
  Status prepare_devices();
  Status update_device_components();
  Status call(const char*, const Json& = Json::object(), Json* = nullptr);
  Status read_state();
  Status acknowledge_presentation(uint64_t generation, uint64_t revision);
  Status drain();
  Status foreground();
  void report(const Status&);
  Status set_metrics(const Metrics&);
  Status pointer(const PointerInput&);
  Status wheel(const WheelInput&);
  Status key(const KeyInput&);
  void cancel_touches();
};
class Presenter {
 public:
  explicit Presenter(Core& c) : core_(c) {}
  virtual ~Presenter() = default;
  virtual Status initialize(void*, const Rect&) = 0;
  virtual void* native_handle() const = 0;
  virtual Status set_bounds(const Rect&) = 0;
  virtual Status set_visible(bool) = 0;
  virtual Status attach() = 0;
  virtual Status
  detach() = 0;  // drain GPU work before releasing any texture/context
  virtual Status present() = 0;
  virtual void refresh() = 0;

 protected:
  Core& core_;
  void visibility(bool visible) {
    core_.surface_visible = visible;
    core_.report(core_.foreground());
  }
  Status metrics(const Metrics& m) { return core_.set_metrics(m); }
};
std::unique_ptr<Presenter> create_presenter(Core&);
struct Raster {
  const weapp_container_sdk_frame* frame = nullptr;
  weapp_container_sdk_frame_info_data info{};
  std::vector<uint8_t> bgra;
  uint64_t generation = 0;
  ~Raster() { clear(); }
  void clear();
  Status update(Core&, bool& changed);
};
}  // namespace weapp::desktop::detail
