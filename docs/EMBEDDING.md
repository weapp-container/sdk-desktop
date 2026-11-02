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
its observed access-point cache after requesting a scan.

Foreground location uses CoreLocation on macOS, C++/WinRT Geolocator on Windows
when its SDK headers and runtime activation are available, and GeoClue on Linux
when a system service is owned or activatable. Linux hosts must set the trusted
`DeviceApiOptions::linux_desktop_id` to their installed desktop-file ID (with or
without `.desktop`); empty disables location routes. Guest parameters cannot
supply this identity. GeoClue service policy and OS consent still apply. Windows
permission acquisition requires the attached visible host HWND, its UI thread
and foreground root window. Missing capabilities leave the routes unregistered.

All three drivers accept WGS84 only; unsupported coordinate systems are rejected.
`wx.getFuzzyLocation` requests `scope.userFuzzyLocation`, rounds coordinates to
0.01 degrees, reports accuracy of at least 1500 metres and removes altitude and
speed metadata. Precise calls request `scope.userLocation`. Location timeout is
100..120000 milliseconds (default 30000). Starting a watch waits for its initial
native startup/fix; an active watch accepts repeated starts with identical
options, while a changed accuracy or timeout requires stopping it first. Windows
polls Geolocator fixes at one-second intervals; Linux observes GeoClue's client
location at 500ms intervals. Neither implementation promises an OS event stream.

Stop invalidates queued watch delivery and pending application/native grants.
Windows/Linux stop receipts wait until their native source is destroyed, including
late acquisition after cancellation; cancelled workers retain their slots within
the eight-owner limit until cleanup completes. Windows retains the same async
operation until a completion callback or terminal status permits `Close`, and
retries uncertain close failures without releasing the operation or apartment.
GeoClue tries Stop/DeleteClient, then confirms closure of its private D-Bus
connection; GeoClue documents private disconnect as client cleanup. Failed
connection closure retains the connection for retry. A persistent native cleanup
failure keeps stop/retirement pending rather than acknowledging released resources.
macOS detaches the old manager/delegate and captures a new generation before a
fresh watch; queued callbacks from a retired manager cannot enter that watch.
Reset, background and retirement revoke both queued delivery and pending grants;
returning to foreground requires a fresh start. Other desktop device families
remain unsupported unless a host adds its own registered handlers.

The macOS source has a local CMake archive-build check. This is not camera,
Bluetooth, microphone, contact, calendar or privacy-dialog acceptance evidence.
Windows target compilation, a complete Linux GTK target build and physical
device verification remain separate requirements; available source routes do
not establish OS parity. The GeoClue source has an isolated compile check against
real GIO headers on the local macOS machine; this does not execute GeoClue.

The optional CMake `WEAPP_DESKTOP_BUILD_TESTS=ON` target builds
`weapp_device_permissions_test`; run it with `ctest --test-dir <build-dir>
--output-on-failure`. It exercises the production dispatcher, persisted consent,
owner queue and retained-object cleanup with fake OS consent callbacks. Pending
application grants, delayed OS grants and queued successful completions cannot
escape a permission reset; fresh authorization can recover afterward. This is
an executable lifecycle contract check, not hardware or privacy-dialog acceptance.
The `weapp_device_location_test` executable additionally uses fake native sources
to verify fuzzy projection/scopes, queued-event revocation, delayed acquisition,
stop/drain ordering, cleanup failure recovery, background behavior and bounded
cancelled workers. It submits the adapter's complete configuration through the
actual linked core C ABI; this validates catalog registration, not OS location.

Native request cancellation and managed-file publication share a completion
barrier on macOS, Windows and Linux. Cancellation before publication prevents
the download file and result enqueue. A write already accepted by that barrier
finishes its bounded file publication and result enqueue before cancellation
returns; cancellation does not roll back an accepted file. The optional
`weapp_device_publication_test` executable checks both orderings using real
managed files and controlled worker timing, plus rejection after owner retirement.
It does not establish WinHTTP, libcurl, NSURLSession or hardware acceptance.

Windows advertises `wx.connectSocket` and the `SocketTask` send, close and
Open/Message/Error/Close event family through asynchronous WinHTTP. Linux
advertises that same family only when built against libcurl **8.16+**, both
WebSocket symbols link, and the loaded library reports the `wss` protocol.
The installed CMake package requires 8.16+ if its archive contains this route;
an HTTP-only archive retains its 7.85+ dependency. A missing gate leaves the
entire SocketTask family absent from the native service configuration.

| Source route | Platform transport | Activation requirement | Acceptance still required |
| --- | --- | --- | --- |
| HTTPS request/download/upload | WinHTTP / optional libcurl | Windows 10 SDK / libcurl 7.85+ | Target build and real transfer/cancellation |
| WSS SocketTask | Async WinHTTP WebSocket | Windows 10 SDK | Windows build and real TLS/close/cancellation |
| WSS SocketTask | libcurl WebSocket | Headers/library 8.16+, linked APIs, runtime `wss` protocol | Linux build and real TLS/close/cancellation |

These WSS routes accept only ASCII credential-free URLs without fragments and
match their canonical **HTTPS origin** against `allowed_network_origins`.
They retain system certificate/hostname verification, reject redirects,
validate offered and selected subprotocols, and reject reserved or injected
request headers. Text/binary messages are limited to one MiB; each socket has
32 queued commands and four MiB of queued outgoing bytes. Each owner has at
most eight native socket workers, including cancelled workers still draining.
Inbound messages share the bounded owner queue; saturation closes the transport
and reserves space for Error/Close notifications. All guest events and results
are delivered by the owner thread. Close completion follows native handle
cleanup; retirement cancels and joins the workers. WinHTTP callback state
survives the final `HANDLE_CLOSING` notifications.

With `WEAPP_DESKTOP_BUILD_TESTS=ON`, `weapp_device_socket_protocol_test`
checks strict inputs, fragments, binary bytes, quotas, terminal queue capacity
and UTF-8 handling without linking a runtime or contacting a server.
`weapp_device_socket_test` links the SDK and exercises its actual dispatcher,
owner delivery, cancellation bounds and worker drain with a fake OS transport.
It also submits the adapter's complete configuration to the actual runtime
through `configureHostServices`, checks rejection of an API absent from the
locked catalog, and checks that a rejected update preserves valid configuration.
SocketTask and RecorderManager expose only catalogued `on` events; the runtime
releases subscriptions through their lifecycle without invented `off` APIs.
On UNIX with libcurl 8.16+, `weapp_curl_socket_compile` compiles the actual
optional transport against real headers, and `weapp_curl_socket_headers_test`
executes the bounded C callback with malformed and duplicate response headers.
These checks do not establish a Linux
target build, a Windows build or real network/device acceptance. The older
macOS NSURLSession SocketTask implementation is a separate source route.
