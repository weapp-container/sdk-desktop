#pragma once

#if defined(__linux__)
#include <gtk/gtk.h>
#endif

namespace weapp::desktop::test {
// Session::create requires the actual platform event loop on the host thread.
// Linux test runners provide an owned X11 or Wayland display; a missing display
// must fail rather than replacing the production event loop with a stub.
inline bool initialize_native_host() {
#if defined(__linux__)
  int argc = 0;
  char** argv = nullptr;
  return gtk_init_check(&argc, &argv);
#else
  return true;
#endif
}
}  // namespace weapp::desktop::test
