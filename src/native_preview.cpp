#include "internal.hpp"
#include <cmath>
#include "weapp/native_preview.hpp"

namespace weapp::desktop {
struct NativePreview::Impl {
  detail::Core core;
  std::unique_ptr<detail::Presenter> presenter;
  bool attached = false;
  ~Impl() {
    // The renderer must be detached before the registry may release its raw
    // runtime. Presenter resource deletion stays on this same owner thread.
    if (presenter) core.report(presenter->detach());
    core.presenter = nullptr;
    presenter.reset();
  }
};
static bool valid_bounds(const Rect& rect) {
  return std::isfinite(rect.x) && std::isfinite(rect.y) &&
      std::isfinite(rect.width) && std::isfinite(rect.height) &&
      rect.x >= 0 && rect.y >= 0 && rect.x <= 32767 && rect.y <= 32767 &&
      rect.width >= 0 && rect.height >= 0 && rect.width <= 32767 && rect.height <= 32767;
}
NativePreview::NativePreview(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NativePreview::~NativePreview() = default;
std::unique_ptr<NativePreview> NativePreview::create(
    void* runtime, void* parent, const Metrics& metrics, const Rect& bounds,
    std::function<void(Status)> errors, Status& status) {
  if (!runtime || !parent || !valid_bounds(bounds) || !metrics.width || !metrics.height ||
      !metrics.screen_width || !metrics.screen_height ||
      !std::isfinite(metrics.pixel_ratio) || metrics.pixel_ratio <= 0 ||
      !std::isfinite(metrics.screen_top)) {
    status = Status::error("ERR_INVALID_ARGUMENT", "native preview needs a live runtime and parent");
    return {};
  }
  auto impl = std::make_unique<Impl>();
  impl->core.config.metrics = metrics;
  impl->core.config.require_gpu = true;
  impl->core.handle = static_cast<weapp_container_sdk_session*>(runtime);
  impl->core.owns_handle = false;
  impl->core.error_sink = std::move(errors);
  status = impl->core.read_state();
  if (!status) return {};
  if (impl->core.state.status != SessionState::Ready) {
    status = Status::error("ERR_NOT_LOADED", "load the borrowed runtime before attaching a native preview");
    return {};
  }
  if (!impl->core.presentation_required) {
    status = Status::error("ERR_UNSUPPORTED_CAPABILITY", "borrowed runtime must use native GPU presentation");
    return {};
  }
  impl->presenter = detail::create_presenter(impl->core);
  impl->core.presenter = impl->presenter.get();
  status = impl->presenter->initialize(parent, bounds);
  if (status) status = impl->presenter->attach();
  if (!status) return {};
  impl->attached = true;
  return std::unique_ptr<NativePreview>(new NativePreview(std::move(impl)));
}
Status NativePreview::set_bounds(const Rect& rect) {
  if (auto status = impl_->core.check(); !status) return status;
  if (!valid_bounds(rect)) return Status::error("ERR_INVALID_ARGUMENT", "invalid native preview bounds");
  return impl_->presenter->set_bounds(rect);
}
Status NativePreview::set_visible(bool visible) {
  if (auto status = impl_->core.check(); !status) return status;
  return impl_->presenter->set_visible(visible);
}
Status NativePreview::present() {
  if (auto status = impl_->core.check(); !status) return status;
  if (!impl_->attached) return Status::error("ERR_SURFACE_DETACHED", "native preview is detached");
  if (auto status = impl_->core.read_state(); !status) return status;
  return impl_->presenter->present();
}
Status NativePreview::detach() {
  if (auto status = impl_->core.check(); !status) return status;
  if (!impl_->attached) return {};
  auto status = impl_->presenter->detach();
  if (status) impl_->attached = false;
  return status;
}
}  // namespace weapp::desktop
