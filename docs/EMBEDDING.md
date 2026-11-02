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


## Native device services

C++ hosts opt in before session creation with `Config.native_device_apis =
std::make_shared<DeviceApiOptions>()` from `weapp/device_apis.hpp`. Configure
`allowed_network_origins` with exact HTTPS origins; an empty list denies native
network access. `private_root` selects the SDK's durable managed file store.
The core supplies the application/account lease and real native directory;
guest modules see only the protected `wx.env.USER_DATA_PATH` value
`wxfile://user`. Do not supply a filesystem root from guest options.

The SDK installs platform handlers before module evaluation and handles their
completions on the creating UI thread. The reusable `DeviceApis` interface also
supports a borrowed native session (for example Flutter): its command callback
must use that session, its platform view must supply `set_native_parent`, and
its owner loop must call `pump`, `update_components`, `set_foreground` and
`retire`. It must not create a second session for device operations.

Managed synchronous filesystem calls perform real disk operations inside the
current JavaScript turn. Asynchronous filesystem operations and native media
publication use the same scope lock, quotas and atomic replacement. Individual
binary/read/write chunks are limited to one MiB, the managed scope to 32 MiB,
and directory enumeration to 4096 entries. Saved settings and permission
consent live in the core's private auxiliary directory. Account changes retire
native resources before the core acknowledges cleanup and releases its file
lease. Native callbacks cannot publish files after that acknowledgement.

On macOS the driver uses NSURLSession HTTPS/WSS, AppKit dialogs/clipboard,
CoreLocation, CoreWLAN, CoreBluetooth central/GATT, AVFoundation audio/camera,
Contacts and EventKit. The containing app must declare camera, microphone,
location, Bluetooth, contacts and calendar usage descriptions for the services
it enables, plus normal sandbox/network/device entitlements when sandboxed.
Recording requires explicit AAC or WAV; encoded frame callbacks are currently
unavailable. Native video/audio sources must be managed files, downloaded
through the trusted networking policy first. Camera/video pixels enter the
normal Skia paint order with clipping and transforms. No child overlay view
bypasses composition. Application permission denials persist until the user
resets them in application settings; reset stops sensitive active resources.
Biometric support/enrollment detection is native, while SOTER signature
production remains unsupported because LocalAuthentication cannot produce a
WeChat SOTER proof.

Windows uses WinHTTP, system clipboard/dialogs/power information, native WLAN
scan/connection notifications and system settings/map handlers. Wi-Fi connection
supports open or WPA2 personal networks with a temporary profile; accepted OS
connection attempts may finish after cancellation, while their canceled guest
continuations remain retired. Linux uses GTK/GIO, optional libcurl 7.85+,
NetworkManager Wi-Fi discovery/current access points, the freedesktop screen
inhibitor and sysfs backlight control. Linux services may reject unavailable
hardware, missing daemons or insufficient OS permissions. NetworkManager lists
its observed access-point cache after requesting a scan. Other desktop device
families remain unsupported unless a host adds its own registered handlers.

The macOS source has a local CMake archive-build check. This is not camera,
Bluetooth, microphone, contact, calendar or privacy-dialog acceptance evidence.
Windows/Linux target builds and physical device verification remain separate
requirements; available source routes do not establish OS parity.

The optional CMake `WEAPP_DESKTOP_BUILD_TESTS=ON` target builds
`weapp_device_permissions_test`; run it with `ctest --test-dir <build-dir>
--output-on-failure`. It exercises the production dispatcher, persisted consent,
owner queue and retained-object cleanup with fake OS consent callbacks. Pending
application grants, delayed OS grants and queued successful completions cannot
escape a permission reset; fresh authorization can recover afterward. This is
an executable lifecycle contract check, not hardware or privacy-dialog acceptance.
