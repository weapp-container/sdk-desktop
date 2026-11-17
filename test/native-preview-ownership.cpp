// Host-independent ownership test. The presenter and native C ABI are injected;
// this does not create a window or claim GPU/platform execution.
#include "internal.hpp"
#include "weapp/native_preview.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
using namespace weapp::desktop;
struct weapp_container_sdk_session { bool gpu = true; };
static int creates = 0, destroys = 0, pumps = 0, drains = 0, presents = 0, attaches = 0;
static int result(weapp_container_sdk_buffer* output, detail::Json value) {
  auto bytes = detail::Json{{"ok", true}, {"value", value}}.dump();
  output->len = bytes.size();
  output->data = static_cast<uint8_t*>(std::malloc(output->len));
  std::memcpy(output->data, bytes.data(), output->len);
  return 0;
}
extern "C" {
void weapp_container_sdk_buffer_free(weapp_container_sdk_buffer* value) {
  std::free(value->data); value->data = nullptr; value->len = 0;
}
int32_t weapp_container_sdk_session_new(const uint8_t*, size_t,
    weapp_container_sdk_session**, weapp_container_sdk_buffer* output) {
  ++creates; return result(output, nullptr);
}
int32_t weapp_container_sdk_session_destroy(weapp_container_sdk_session** session,
    weapp_container_sdk_buffer* output) {
  ++destroys; *session = nullptr; return result(output, nullptr);
}
int32_t weapp_container_sdk_session_command(weapp_container_sdk_session* session,
    const uint8_t* command, size_t size, const uint8_t*, size_t,
    weapp_container_sdk_buffer* output) {
  std::string name(reinterpret_cast<const char*>(command), size);
  if (name == "pump") ++pumps;
  if (name == "takeEvents") { ++drains; return result(output, detail::Json::array()); }
  return result(output, {{"status", "ready"}, {"generation", 7}, {"frameRevision", 9},
      {"presentationRequired", session->gpu}, {"inputEnabled", true}, {"foreground", true}});
}
int32_t weapp_container_sdk_frame_acquire(weapp_container_sdk_session*,
    const weapp_container_sdk_frame**, weapp_container_sdk_buffer* output) { return result(output, nullptr); }
int32_t weapp_container_sdk_frame_info(const weapp_container_sdk_frame*,
    weapp_container_sdk_frame_info_data*, weapp_container_sdk_buffer* output) { return result(output, nullptr); }
int32_t weapp_container_sdk_frame_release(const weapp_container_sdk_frame**,
    weapp_container_sdk_buffer* output) { return result(output, nullptr); }
}
namespace weapp::desktop::detail {
const char* platform_name() { return "test-injected"; }
std::unique_ptr<Loop> create_loop(std::function<void()>, Status&) { return {}; }
class ProbePresenter final : public Presenter {
 public:
  explicit ProbePresenter(Core& core) : Presenter(core) {}
  Status initialize(void*, const Rect&) override { return {}; }
  void* native_handle() const override { return nullptr; }
  Status set_bounds(const Rect&) override { return {}; }
  Status set_visible(bool) override { return {}; }
  Status attach() override { ++attaches; return {}; }
  Status detach() override { return {}; }
  Status present() override { ++presents; return {}; }
  void refresh() override {}
};
std::unique_ptr<Presenter> create_presenter(Core& core) { return std::make_unique<ProbePresenter>(core); }
}
int main() {
  weapp_container_sdk_session runtime;
  int parent = 1;
  Metrics metrics{375, 667, 2, 1920, 1080, 0};
  Rect bounds{320, 48, 375, 667};
  Status status;
  auto preview = NativePreview::create(&runtime, &parent, metrics, bounds, {}, status);
  assert(status && preview && attaches == 1);
  assert(preview->present());
  assert(presents == 1 && creates == 0 && pumps == 0 && drains == 0 && destroys == 0);
  auto invalid = bounds; invalid.x = std::numeric_limits<double>::infinity();
  assert(!preview->set_bounds(invalid));
  assert(preview->detach());
  assert(!preview->present());
  preview.reset();
  assert(destroys == 0 && pumps == 0 && drains == 0);
  runtime.gpu = false;
  preview = NativePreview::create(&runtime, &parent, metrics, bounds, {}, status);
  assert(!status && status.code == "ERR_UNSUPPORTED_CAPABILITY" && !preview);
  assert(attaches == 1 && destroys == 0 && creates == 0);
  std::cout << "PASS borrowed native preview keeps one runtime, never pumps/drains/destroys it, rejects raster and invalid bounds\n";
}
