#include <gtk/gtk.h>

#include <atomic>
#include <cassert>
#include <functional>
#include <thread>

namespace weapp::desktop::detail {
bool host_services_post_to_context(GMainContext*, std::function<void()>);
}

int main() {
  auto* context = g_main_context_new();
  assert(context);
  const auto owner = std::this_thread::get_id();
  std::thread::id callback_thread;
  std::atomic<bool> callback_ran{false};
  std::thread worker([&] {
    const bool queued =
        weapp::desktop::detail::host_services_post_to_context(context, [&] {
          callback_ran = true;
          callback_thread = std::this_thread::get_id();
        });
    assert(queued);
    // Explicit source attachment must not execute UI work on the cancelling
    // worker, even when the owner context is not being iterated yet.
    assert(!callback_ran);
  });
  worker.join();
  assert(!callback_ran);
  while (g_main_context_pending(context))
    g_main_context_iteration(context, false);
  assert(callback_ran);
  assert(callback_thread == owner);
  g_main_context_unref(context);
  return 0;
}
