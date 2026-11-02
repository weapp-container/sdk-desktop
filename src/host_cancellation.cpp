#include <mutex>

#include "weapp/host_services.hpp"

namespace weapp::desktop {
struct HostCancellation::Impl {
  std::atomic<bool> cancelled{false};
  std::mutex mutex;
  std::vector<std::function<void()>> callbacks;
};

HostCancellation::HostCancellation() : impl_(std::make_shared<Impl>()) {}
bool HostCancellation::cancelled() const { return impl_->cancelled.load(); }
void HostCancellation::cancel() const {
  if (impl_->cancelled.exchange(true)) return;
  std::vector<std::function<void()>> callbacks;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    callbacks.swap(impl_->callbacks);
  }
  for (auto& callback : callbacks) {
    try {
      callback();
    } catch (...) {
    }
  }
}
void HostCancellation::on_cancel(std::function<void()> callback) const {
  if (!callback) return;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->cancelled.load()) {
      impl_->callbacks.push_back(std::move(callback));
      return;
    }
  }
  callback();
}
}  // namespace weapp::desktop
