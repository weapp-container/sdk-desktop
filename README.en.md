# Weapp Container Desktop SDK

Runtime packaging and publication now live in the [core repository](https://github.com/weapp-container/weapp-container/tree/main/tools/runtime-artifacts). New manifests select exact core releases through `--manifest`. The default lock retains its historical `runtime-artifacts` source, version, hashes and bundled licenses. The retired repository remains a read-only archive; downloads never fall back between repositories. Private repositories still require read access; source licensing does not imply public binary availability.

The core runtime and SDK source are available under Apache-2.0. Build the current runtime from the [core source guide](https://github.com/weapp-container/weapp-container/blob/main/docs/sdk-repositories.md). Existing prebuilt archives retain their original licenses, access requirements and exact version/hash pins; this source update does not publish replacement binaries.

[简体中文](README.md) | **English**

Embed mini apps in an existing macOS, Windows, or Linux application. The SDK provides C/C++ sessions, native views, messaging, and lifecycle APIs. Your application owns its windows and main event loop.

Source version: `0.1.0-preview.6.0`. C ABI: `1`. Host wrappers, build tools, and consumer examples in this repository are open source under Apache-2.0. New open-source runtime builds carry Apache-2.0; retain their `RUNTIME-LICENSE` and third-party notices.

## Prerequisites

- Node.js 24+, Python 3, CMake 3.21+, and a C++17 compiler.
- macOS: macOS 12+ and Xcode Command Line Tools.
- Windows: Windows 10 x64, MSVC, and the Windows SDK; C++/WinRT adapters use C++20.
- Linux: Ubuntu 22.04 x64 / glibc 2.35+, with GTK 3.24, GIO 2.66+, fontconfig, and epoxy development packages.

Rust, Cargo, and the core source repository are unnecessary. The SDK is not published to a public package registry; build an installation from this checkout:

```sh
git clone https://github.com/weapp-container/sdk-desktop.git
cd sdk-desktop
```

## Obtain the runtime and build

The preparation script verifies version, ABI, size, and SHA-256 against [runtime-artifacts.lock.json](runtime-artifacts.lock.json). The current lock contains preview.6.0 runtime archives for macOS and Linux, with no Windows runtime entry. Windows integration source is available, but default preparation fails when its required pin is absent.

After obtaining runtime download access, inject `WEAPP_CONTAINER_RUNTIME_TOKEN` through your secret manager or CI secrets, then run:

```sh
node tools/prepare-runtime.mjs --target macos-universal
node tools/build.mjs \
  --target macos-universal \
  --native-root .runtime/install/native \
  --licenses-root .runtime/install/licenses \
  --pack /absolute/portable-pack \
  --verify
```

`/absolute/portable-pack` must be an unpacked portable pack containing `manifest.json` and `blobs/sha256/`. Do not pass a source project or archive.

If you already have the exact authorized archives, replace the preparation command with:

```sh
node tools/prepare-runtime.mjs \
  --target macos-universal \
  --artifact-dir /absolute/authorized-archives
```

Offline input undergoes the same checks. Use `--manifest /absolute/reviewed-lock.json` to select another reviewed lock. Source and runtime versions must match; a missing pin or failed check never falls back to an older version.

Targets are `macos-universal`, `macos-arm64`, `macos-x64`, `windows-x64`, and `linux-x64`; build on the matching OS. Output defaults to `artifacts/desktop/TARGET`. `--verify` runs the installed independent consumer and needs a working desktop display. Without it, compilation and linking do not establish successful UI execution.

## Integrate with CMake

Pass the installation directory through `CMAKE_PREFIX_PATH`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(MyHost LANGUAGES C CXX)

add_executable(my_app main.cpp)
find_package(WeappContainerDesktop CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE WeappContainer::Desktop)
weapp_container_copy_runtime(my_app)
```

```sh
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/absolute/installed-sdk \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Windows uses a runtime DLL and import library. `weapp_container_copy_runtime` copies the DLL beside your executable. Include the installed license files when distributing the application.

## Create a view and load a pack

Place this code in your host window or controller. Keep `MiniAppHost` alive for the window's lifetime. `native_parent` is an `NSView*`, `HWND`, or `GtkContainer*`, respectively.

```cpp
#include <weapp/desktop.hpp>
#include <stdexcept>

using namespace weapp::desktop;

struct MiniAppHost {
  std::shared_ptr<Session> session;
  std::unique_ptr<View> view;
  bool closed = false;

  static void check(const Status& status) {
    if (!status) throw std::runtime_error(status.code + ": " + status.message);
  }

  void attach(void* native_parent, const std::string& private_directory) {
    Config config;
    config.storage_root = private_directory;
    config.storage_key = "my-app/account-42";
    Status status;
    session = Session::create(config, status);
    check(status);
    view = View::create(session, native_parent, {0, 0, 360, 640}, status);
    check(status);
    session->on_event([this](const Event& event) {
      if (event.type == EventType::Closed) closed = true;
      // Handle Message, HostCall, Error, etc. Never throw from callbacks.
    });
  }

  // Call after a positive native layout; GTK needs its first allocation.
  void load(const std::string& pack_directory) {
    check(session->load(pack_directory));
  }

  void begin_close() { check(session->close()); }
};
```

Call `view->set_bounds(...)` when its bounds change, and keep your existing main event loop running. The SDK installs a session timer, so manual `pump()` calls are normally unnecessary. Loading before GTK layout returns `ERR_NOT_LAID_OUT`.

Common business operations return a `Status` that must be checked:

```cpp
MiniAppHost::check(host.session->navigate("pages/detail/index", {{"id", "coffee"}}));
MiniAppHost::check(host.session->send_message(Value::Object{
    {"type", "host.greeting"}, {"text", "Hello"}
}));
MiniAppHost::check(host.session->back());
```

Here, `host` is a loaded `MiniAppHost` instance. To expose host methods to a mini app, call `register_host_method` before the first load, then complete each `HostCall` using `complete_host_call` or `fail_host_call`. See the [embedding guide](docs/EMBEDDING.md).

## Lifecycle and storage

- Run all Session/View methods, event handlers, and destruction on their creating UI thread. macOS uses the AppKit main thread.
- Persistence requires both `storage_root` and `storage_key`. Use an application-private directory and a stable application/account key. Omitting them selects memory-only storage.
- Call `begin_close()` when closing the window, and keep the main loop alive until `closed` is `true`. Inspect the `Closed` event for errors, then release the view and session. Do not exit before persistence completes.
- Keep the main loop servicing asynchronous completion and storage while hidden. A session can have only one view at a time.

## Run the consumer and checks

```sh
node tools/consumer-smoke.mjs \
  --sdk /absolute/installed-sdk \
  --pack /absolute/portable-pack \
  --arch arm64
npm test
```

The consumer uses installed headers and libraries and checks relocation, linking, and native execution. Match `--arch` to your target; x86_64 execution on Apple Silicon is a Rosetta result. `npm test` covers wrappers and tooling, not platform, GPU, or physical-device acceptance.

## Further reading

- [Embedding, threads, events, and native device APIs](docs/EMBEDDING.md)
- [Account, identity, and payment host services](ACCOUNT-SERVICES.md)
- [Windows sensors](WINDOWS-SENSORS.md) · [Windows audio](WINDOWS-AUDIO.md)
- [Shared adapter protocol](CONTRACT.md) · [SDK contracts](https://github.com/weapp-container/sdk-contract)
- [Source license](LICENSE) · [Original extraction receipt](ORIGIN.json) · [Current source inventory](SOURCE-INVENTORY.json)

Build tools verify the source inventory. After changing SDK source, refresh it through the reviewed workflow in `tools/source-inventory.mjs`; preserve historical `ORIGIN.json` bytes. Refer to each artifact's validation records for the platforms actually built and run.
