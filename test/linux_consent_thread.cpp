#include <gtk/gtk.h>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <thread>

namespace weapp::desktop::detail {
bool host_services_post_to_context(GMainContext*, std::function<void()>);
}

int main() {
  auto require = [](bool condition, const char* message) {
    if (!condition) {
      std::cerr << message << '\n';
      std::exit(1);
    }
  };
  auto* context = g_main_context_new();
  require(context != nullptr, "failed to create GLib owner context");
  const auto owner = std::this_thread::get_id();
  std::thread::id callback_thread;
  std::atomic<bool> callback_ran{false};
  std::thread worker([&] {
    const bool queued =
        weapp::desktop::detail::host_services_post_to_context(context, [&] {
          callback_ran = true;
          callback_thread = std::this_thread::get_id();
        });
    require(queued, "failed to attach cancellation source");
    // Explicit source attachment must not execute UI work on the cancelling
    // worker, even when the owner context is not being iterated yet.
    require(!callback_ran.load(), "cancel source ran on worker thread");
  });
  worker.join();
  require(!callback_ran.load(), "cancel source ran before owner iteration");
  while (g_main_context_pending(context))
    g_main_context_iteration(context, false);
  require(callback_ran.load(), "owner context did not dispatch cancellation");
  require(callback_thread == owner,
          "cancellation callback escaped owner thread");
  g_main_context_unref(context);
  return 0;
}
