# Weapp Container desktop SDK preview

The installed CMake package embeds one real QuickJS/Skia runtime per session in
an application-owned NSView, child HWND, or GTK3 drawing area. The application
owns its main loop and top-level windows. There is no WebView or JavaScript
bridge carrying pixels. This preview is locally installable and is not published
to a registry.

| Target | Native presentation | Baseline |
| --- | --- | --- |
| macOS | NSView + CAMetalLayer, GPU completion before detach | macOS 12+, arm64/x86_64 universal archive |
| Windows | child HWND + premultiplied BGRA DIB/AlphaBlend | Windows 10 x64, MSVC, host per-monitor DPI awareness |
| Linux | GtkDrawingArea + Cairo, Skia CPU snapshots | Ubuntu 22.04 x64, glibc 2.35+, GTK 3.24, X11/Wayland |

Source availability does not certify a target. The package receipt and consumer
logs record the platforms actually built and run. Rosetta execution is explicitly
not an Intel hardware result. Native input includes mouse/touch, wheel and key
events; this preview does not provide native text-entry controls or IME composition.

## Build and consume

Use the standalone commands in [the root README](../README.md). Runtime archives are supplied independently under private binary terms; no parent source checkout or Rust compiler is required.

## C++ embedding

```cpp
#include <weapp/desktop.hpp>
using namespace weapp::desktop;
Status status;
Config config;
config.storage_key = "my-app/account-42";
config.storage_root = application_private_directory;
auto session = Session::create(config, status);
// Check status after every operation. native_parent is NSView*, HWND, or GtkContainer*.
auto view = View::create(session, native_parent, {0, 0, 360, 640}, status);
session->on_event([](const Event& event) { /* inspect event.type and fields */ });
status = session->register_host_method("account.current");
status = session->load(unpacked_portable_pack_directory);
```

`View::native_handle()` returns the native child object. Put it in the host's
layout and call `set_bounds` for manual layout; bounds are logical pixels. GTK
non-fixed containers govern actual allocation, so their layout wins over a size
request. The SDK observes display scale changes. A zero-sized view hides
presentation while preserving the last positive viewport. GTK hosts must allow the first real size allocation before load; otherwise load
returns `ERR_NOT_LAID_OUT`. A session may have
one view at a time, enforced for GPU and raster presenters. Destroying a view
preserves its session; attaching a new view resumes it.

Session construction is lazy: first load requires actual positive view/display
metrics. Native layout supplies these, or a headless host supplies `Config.metrics`.
Module-scope window APIs therefore see host observations on their first execution.
Storage is memory-only unless both `storage_key` and `storage_root` are supplied.
Use application/account-specific keys and app-private directories.

Every Session/View method, callback and destruction belongs to the creating UI
thread. macOS requires the AppKit main thread; GTK requires an initialized host
GTK display. The SDK installs a per-session timer on the host loop. Timers continue
servicing HTTP, host calls and storage while the view is hidden or closing;
JavaScript timers, input and presentation pause when hidden. Explicit `pump()` is
available for deterministic host integration, but does not replace the host loop.
A wrong-thread command returns an error. Destroying C++ objects on a foreign
thread is a contract violation and terminates rather than touching thread-bound
runtime state. Immutable `Frame` copies can cross threads and outlive the session.

`navigate`, `back`, `send_message`, `register_host_method`, `complete_host_call`,
`fail_host_call`, `pointer`, `wheel`, `key`, `flush_storage` and `close` are typed
methods. `Value` is a typed business data tree; protocol envelopes stay internal.
HostCall events include `callId`, `method` and `payload` in `fields`. Forward
business work to another queue as needed, then deliver completion on the owner UI
thread. `fail_host_call` maps a host failure to the runtime's `invalid_request`
error with the supplied message. Ready, navigation, frame, message, cancellation,
error, storage and closed events are delivered outside runtime invocations.

Load replacement stops presentation and drains/detaches the GPU before candidate
startup. A failed candidate preserves and reattaches the previous runtime.
`close()` also detaches safely and starts an asynchronous durable close. Keep the
host loop running until `EventType::Closed`, inspect its error fields, then destroy
the view and session. The C++ destructor is a synchronous last-resort cleanup;
explicit close gives the host observable persistence completion. Callbacks may
change the listener or initiate another operation; nested event loops do not
recursively drain callbacks. Do not throw from callbacks.

C consumers use `weapp/desktop.h`, including opaque typed Value builders/accessors.
No C function accepts a raw command name or JSON string. Borrowed callback fields
last for that callback; use `weapp_desktop_value_clone` to retain them. C session
`destroy` requires the Closed state and clears the caller's handle on success.

## Installed consumer verification

`consumer` is an independent CMake project; it includes installed
headers only. It tests C and C++ linkage, two sessions/views, native pointer/key
adapters, state isolation, a failed candidate rollback, DPR resize, hidden
service pumping, cross-thread rejection, durable close, retained frame lifetime
and storage-lock reopen. On Linux execute the same consumer with both a display
from `xvfb-run` and a headless Weston compositor using `GDK_BACKEND=wayland`.
The consumer creates and destroys its own host window and temporary storage.

The internal codec is nlohmann/json 3.11.3 (MIT), vendored from its upstream tagged
single header. Its license ships with the installation. Native distribution
license inventory is copied by the packager when present; release packaging must
retain the full third-party inventory produced by `the private runtime distribution`.
