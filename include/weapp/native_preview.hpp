#pragma once
#include "desktop.hpp"

namespace weapp::desktop {
// Native-only adapter for a runtime already owned by another host wrapper.
// The runtime pointer NEVER crosses Dart/JS or changes ownership. The caller
// keeps it alive until detach has completed. All calls use its owner thread.
// This adapter does not pump the runtime, close it, drain its event queue or
// open its storage: it reuses exactly the supplied session and presenters.
class NativePreview final {
 public:
  static std::unique_ptr<NativePreview> create(
      void* borrowed_runtime, void* parent, const Metrics&, const Rect&,
      std::function<void(Status)> errors, Status&);
  ~NativePreview();
  NativePreview(const NativePreview&) = delete;
  NativePreview& operator=(const NativePreview&) = delete;
  Status set_bounds(const Rect&);
  Status set_visible(bool);
  Status present();
  Status detach();
 private:
  struct Impl;
  explicit NativePreview(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
}  // namespace weapp::desktop
