<a id="weapp-容器桌面-sdk"></a>
# Weapp Container Desktop SDK

[简体中文](README.md) · **English**

<a id="公开发行候选preview7"></a>
## Public distribution candidate: preview.7

The installation examples below describe the preview.7 distribution candidate. Check the named registry or release before using them; source publication does not establish package publication or device acceptance. The source-build examples later in this document refer to the reviewed preview.6 checkout below. The core builder currently emits preview.6, so do not combine that runtime with preview.7 wrappers.

First public publication is being prepared. Use these commands once this version is visible in its registry. Historical private archives and locks retain their original identities.

Version `0.1.0-preview.7.0` requires CMake 3.21+ and C++17. The public [GitHub prerelease](https://github.com/weapp-container/sdk-desktop/releases/tag/v0.1.0-preview.7.0) provides:

- `WeappContainerDesktop-0.1.0-preview.7.0-macos-universal.tar.gz`: macOS 12+, arm64/x86_64.
- `WeappContainerDesktop-0.1.0-preview.7.0-linux-x86_64.tar.gz`: x86_64, GTK 3.24+, GIO 2.66+, fontconfig and epoxy.
- `WeappContainerDesktop-0.1.0-preview.7.0-windows-x86_64.tar.gz`: Windows x86_64, MSVC.

Verify `SHA256SUMS` before extracting to your installation directory. Archives include relocatable CMake configuration, headers, runtime libraries and licenses. Consumers need no Rust or sibling source checkout.

<a id="已复核的源码构建版本"></a>
### Reviewed source-build checkout

```sh
git clone https://github.com/weapp-container/sdk-desktop.git sdk-desktop-source
cd sdk-desktop-source
git checkout c27a715b06367957a24af0a91c969a1abec5627e
```



[Documentation](https://container.weapp.dev/en/docs/) · [Build from source](https://github.com/weapp-container/weapp-container/blob/main/docs/sdk-source-build.en.md) · [APIs and contracts](https://github.com/weapp-container/sdk-contract/blob/main/README.en.md)

Embed WeChat Mini Programs into existing macOS, Windows, or Linux applications. The SDK provides C/C++ session, native view, message, and lifecycle interfaces, with the host managing the window and main event loop.

Reviewed source example version: `0.1.0-preview.6.0`, C ABI: `1`. This repository uses Apache-2.0 for the host wrapper, build tools, and consumption examples; the newly built runtime library is licensed under Apache-2.0 and requires retention of its `RUNTIME-LICENSE` and third-party attribution statements.

<a id="从当前源码开始"></a>
## Start from Source Code

For initial integration, use the public core repository and its fixed SDK commit—no download token from historical archives is required:

```sh
git clone --recurse-submodules https://github.com/weapp-container/weapp-container.git
cd weapp-container
corepack enable
pnpm install --frozen-lockfile
node sdk/contract/fixtures/make-acceptance-pack.mjs --out artifacts/hello-pack
```

Then prepare platform-specific runtimes and notices according to the [source build instructions](https://github.com/weapp-container/weapp-container/blob/main/docs/sdk-source-build.en.md), and use the build tool from this repository to generate the integration package. Platform toolchains remain essential dependencies; the availability of source code does not imply that pre-built binaries are available in public package repositories. Existing native mini-program or framework build artifacts can be packaged independently using a portable pack tool; the weapp-vite tool is not required.

The following sections retain detailed installation, view, and lifecycle instructions. Commands involving `prepare-runtime` / `prepare-native` belong to explicit archival paths and must use verified accompanying manifests. Historical default locks may not be downloadable; see the end of the document for details.

<a id="准备环境"></a>
## Prepare Environment

- Node.js 24+, Python 3, CMake 3.21+, C++17 compiler.
- macOS: macOS 12+ with Xcode Command Line Tools.
- Windows: Windows 10 x64, MSVC, and Windows SDK; C++/WinRT adapter requires C++20.
- Linux: Ubuntu 22.04 x64 / glibc 2.35+, GTK 3.24, GIO 2.66+, fontconfig, epoxy development packages.

The host application consumes a built installation without Rust or Cargo. Producing the runtime requires Rust and the public core repository. The SDK is not published to public package managers; a standalone SDK checkout contains the wrapper, examples, and historical archive installation tools.

```sh
git clone https://github.com/weapp-container/sdk-desktop.git
cd sdk-desktop
```


<a id="获取运行库并构建"></a>
## Obtain Runtime and Build

Operate within the clean checkout root directory of the public core repository. Apple Silicon macOS example:

```sh
pnpm sdk:build:native -- --target aarch64-apple-darwin --profile release
pnpm sdk:licenses -- --target aarch64-apple-darwin --features quickjs-native,skia-gpu-metal
pnpm sdk:build:desktop -- --target macos-arm64 --pack artifacts/hello-pack --verify
```

Output is located at `artifacts/sdk/desktop/macos-arm64/`. Intel, universal macOS, Windows, and Linux triples, platform toolchains, and packaging workflows are detailed in [Source Build](https://github.com/weapp-container/weapp-container/blob/main/docs/sdk-source-build.en.md). Build and window validation records are generated for the target system.

<a id="接入已有-cmake-工程"></a>
## Integrate with Existing CMake Project

Pass the installation directory to `CMAKE_PREFIX_PATH`:

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

On Windows, the runtime is distributed as a DLL and import library; copy the DLL to the program's directory using `weapp_container_copy_runtime`. When distributing the application, retain the license files from the installation directory.

<a id="创建视图并加载-pack"></a>
## Create a View and Load a Pack

The following code should be placed within the host window or controller. Retain `MiniAppHost` to ensure it lives as long as the window. Call `native_parent` with `NSView*`, `HWND`, or `GtkContainer*` as appropriate.

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
      // 在这里处理 Message、HostCall、Error 等事件；回调中不要抛异常。
    });
  }

  // 在视图获得实际正尺寸之后调用；GTK 必须等待首次布局分配。
  void load(const std::string& pack_directory) {
    check(session->load(pack_directory));
  }

  void begin_close() { check(session->close()); }
};
```

Call `view->set_bounds(...)` when the view size changes; the host's existing main event loop continues to run. The SDK installs a session timer, and periodic polling via `pump()` is generally unnecessary. GTK will return `ERR_NOT_LAID_OUT` upon loading before layout.

Common business operations are as follows, all requiring validation of the returned `Status`:

```cpp
MiniAppHost::check(host.session->navigate("pages/detail/index", {{"id", "coffee"}}));
MiniAppHost::check(host.session->send_message(Value::Object{
    {"type", "host.greeting"}, {"text", "你好"}
}));
MiniAppHost::check(host.session->back());
```

`host` refers to the previously created and loaded pack instance `MiniAppHost`. When a mini-program needs to invoke a host method, call `register_host_method` before the first load. After receiving `HostCall`, use either `complete_host_call` or `fail_host_call` to complete the request. See [Embedding Guide](docs/EMBEDDING.en.md) for details.

<a id="生命周期与存储"></a>
## Lifecycle and Storage

- All Session/View calls, event handling, and destruction occur on the UI thread where they were created; macOS uses the AppKit main thread.
- Persistent storage only occurs when both `storage_root` and `storage_key` are set. The directory uses the application's private directory, with keys stable across applications and accounts; without these settings, data is stored in memory.
- When the window is closed, call `begin_close()` and continue the main loop until `closed` is `true`. Inspect the error information from `Closed` events before releasing the view and session; do not terminate the process before persistent storage is complete.
- When a view is hidden, the main loop must still process asynchronous completion and storage. Only one session can be associated with a single view at a time.

<a id="运行消费示例与检查"></a>
## Run Consumption Examples and Verification

```sh
node tools/consumer-smoke.mjs \
  --sdk /absolute/installed-sdk \
  --pack /absolute/portable-pack \
  --arch arm64
npm test
```

The consumption example uses the installed headers and libraries and verifies relocation-based linking and native runtime behavior. `--arch` selection must match the target; x86_64 execution on Apple Silicon is a Rosetta result. `npm test` verifies the packaging and tools—this does not replace platform, GPU, or actual device validation.

<a id="进一步阅读"></a>
## Further Reading

- [Embedding, Threads, Events, Native Device Interfaces](docs/EMBEDDING.en.md)
- [Account, Identity, and Payment Host Services](ACCOUNT-SERVICES.en.md)
- [Windows Sensors](WINDOWS-SENSORS.en.md) · [Windows Audio](WINDOWS-AUDIO.en.md)
- [Shared Adapter Protocol](CONTRACT.en.md) · [SDK Contract Repository](https://github.com/weapp-container/sdk-contract/blob/main/README.en.md)
- [Source License](LICENSE) · [Original Extraction Records](ORIGIN.json) · [Current Source List](SOURCE-INVENTORY.json)

The build tool validates the source list. After modifying SDK source code, update the list according to the `tools/source-inventory.mjs` review process. Historical `ORIGIN.json` entries remain unchanged. The actual build and runtime scope for each platform is defined by the verification records of the corresponding build product.

<a id="历史归档与迁移"></a>
## Historical Archives and Migration
Historical issuance audit (2026-11-12): The SDK archive part in the default lock is missing, the runtime pin does not match the current asset with the same name, and the default online preparation will be rejected by the integrity check. Please build from matching clean source, or explicitly select an independently audited companion manifest via `--manifest`; don't just modify the old hash. [Difference and Migration Record](https://github.com/weapp-container/weapp-container/blob/main/docs/runtime-artifacts-migration.en.md).

The runtime distribution tool has been moved into [core warehouse](https://github.com/weapp-container/weapp-container/tree/main/tools/runtime-artifacts). The new manifest uses the exact Release version of the core repository; choose this explicitly with `--manifest`. The updated downloader directs the `runtime-artifacts` identifier in the old lock to the private `weapp-container/runtime-artifacts-private-archive`, allows only nine frozen versions, retains the original version, hash and accompanying license, and the receipt records the actual repository. The old version of the downloader needs to be upgraded; the new repository with the same name only retains the source code of the update tool. There is no cross-repository rollback, private files still require read permission, and the source code license does not mean that historical releases are publicly available.

The core runtime and SDK own source code use Apache-2.0. You can build the current runtime library according to [Main Warehouse Source Code Construction Instructions](https://github.com/weapp-container/weapp-container/blob/main/docs/sdk-source-build.en.md). Existing precompiled archives retain the original license, access requirements, precise versions and hash locks; no replacement binaries will be released in this source code update.

<a id="可选已审核的归档输入"></a>
### Optional: Audited archive input

Historical installation commands (not used as the default path for first access):

Prepare script to verify version, ABI, size and SHA-256 by [runtime-artifacts.lock.json](runtime-artifacts.lock.json). The current lock file contains preview.6.0's macOS and the Linux runtime. There is no Windows runtime entry yet; the Windows source code is available for access, and the default preparation process will fail due to the lack of corresponding entries.

After obtaining the authorization to download the runtime library, inject `WEAPP_CONTAINER_RUNTIME_TOKEN` through the key manager or CI secret, and then execute:

```sh
node tools/prepare-runtime.mjs --target macos-universal
node tools/build.mjs \
  --target macos-universal \
  --native-root .runtime/install/native \
  --licenses-root .runtime/install/licenses \
  --pack /absolute/portable-pack \
  --verify
```

`/absolute/portable-pack` is the unpacked portable pack directory containing `manifest.json` and `blobs/sha256/`. Do not pass in the source code directory or compressed package.

If you have obtained an accurate version of the offline archive, you can replace the prepare command:

```sh
node tools/prepare-runtime.mjs \
  --target macos-universal \
  --artifact-dir /absolute/authorized-archives
```

The same verification is still performed for offline input. You can use `--manifest /absolute/reviewed-lock.json` to specify another audited lock file; SDK source code and runtime version must match. Missing entries or verification failures will not fall back to the old version.

The target options are `macos-universal`, `macos-arm64`, `macos-x64`, `windows-x64`, and `linux-x64`, which must be built on the corresponding system. Products are located at `artifacts/desktop/TARGET` by default. `--verify` will run the independent consumer program after installation and requires an available desktop display environment; if this option is not passed, it will only build and link, which does not mean that the interface has passed.
