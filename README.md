# Weapp Container Desktop SDK

核心运行时与 SDK 自有源码采用 Apache-2.0，可按[主仓库源码构建说明](https://github.com/weapp-container/weapp-container/blob/main/docs/sdk-repositories.md)构建当前运行库。现有预编译归档保留原许可、访问要求及精确版本和哈希锁；本次源码更新不发布替代二进制。

**简体中文** | [English](README.en.md)

在现有 macOS、Windows 或 Linux 应用中嵌入小程序。SDK 提供 C/C++ 的会话、原生视图、消息和生命周期接口，由宿主管理窗口及主事件循环。

当前源码版本：`0.1.0-preview.6.0`，C ABI：`1`。本仓库以 Apache-2.0 开源宿主封装、构建工具和消费示例；新构建的运行库采用 Apache-2.0，使用时需要保留其 `RUNTIME-LICENSE` 和第三方声明。

## 准备环境

- Node.js 24+、Python 3、CMake 3.21+、C++17 编译器。
- macOS：macOS 12+ 与 Xcode Command Line Tools。
- Windows：Windows 10 x64、MSVC 与 Windows SDK；C++/WinRT 适配器使用 C++20。
- Linux：Ubuntu 22.04 x64 / glibc 2.35+，GTK 3.24、GIO 2.66+、fontconfig、epoxy 开发包。

无需 Rust、Cargo 或核心源码仓库。SDK 尚未发布到公共包管理器，按以下步骤从本仓库构建安装目录。

```sh
git clone https://github.com/weapp-container/sdk-desktop.git
cd sdk-desktop
```

## 获取运行库并构建

准备脚本按 [runtime-artifacts.lock.json](runtime-artifacts.lock.json) 校验版本、ABI、大小和 SHA-256。当前锁文件包含 preview.6.0 的 macOS 与 Linux 运行库，尚无 Windows 运行库条目；Windows 源码可供接入，默认准备流程会因缺少对应条目而失败。

获得运行库下载授权后，通过密钥管理器或 CI secret 注入 `WEAPP_CONTAINER_RUNTIME_TOKEN`，再执行：

```sh
node tools/prepare-runtime.mjs --target macos-universal
node tools/build.mjs \
  --target macos-universal \
  --native-root .runtime/install/native \
  --licenses-root .runtime/install/licenses \
  --pack /absolute/portable-pack \
  --verify
```

`/absolute/portable-pack` 是已经解包、包含 `manifest.json` 和 `blobs/sha256/` 的 portable pack 目录。不要传入源码目录或压缩包。

如果已取得精确版本的离线归档，可替换准备命令：

```sh
node tools/prepare-runtime.mjs \
  --target macos-universal \
  --artifact-dir /absolute/authorized-archives
```

离线输入仍执行相同校验。可用 `--manifest /absolute/reviewed-lock.json` 指定另一份已审核的锁文件；SDK 源码与运行库版本必须匹配，缺少条目或校验失败不会回退到旧版本。

目标可选 `macos-universal`、`macos-arm64`、`macos-x64`、`windows-x64`、`linux-x64`，须在对应系统构建。产物默认位于 `artifacts/desktop/TARGET`。`--verify` 会运行安装后的独立消费程序，需要可用的桌面显示环境；不传该选项只构建、链接，不代表界面运行通过。

## 接入已有 CMake 工程

将安装目录交给 `CMAKE_PREFIX_PATH`：

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

Windows 的运行库使用 DLL 和 import library；`weapp_container_copy_runtime` 将 DLL 复制到程序旁边。分发应用时还需保留安装目录中的许可文件。

## 创建视图并加载 pack

以下代码放在宿主窗口或控制器中。保留 `MiniAppHost`，使其与窗口同寿命。`native_parent` 分别为 `NSView*`、`HWND` 或 `GtkContainer*`。

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

视图尺寸变化时调用 `view->set_bounds(...)`；宿主已有的主事件循环继续运行。SDK 会安装会话定时器，通常不需要自行轮询 `pump()`。GTK 在布局前加载会返回 `ERR_NOT_LAID_OUT`。

常用业务操作如下，均需检查返回的 `Status`：

```cpp
MiniAppHost::check(host.session->navigate("pages/detail/index", {{"id", "coffee"}}));
MiniAppHost::check(host.session->send_message(Value::Object{
    {"type", "host.greeting"}, {"text", "你好"}
}));
MiniAppHost::check(host.session->back());
```

`host` 是上面创建并已加载 pack 的 `MiniAppHost` 实例。需要小程序调用宿主方法时，在首次加载前调用 `register_host_method`，接收 `HostCall` 后使用 `complete_host_call` 或 `fail_host_call` 完成请求，详见 [嵌入指南](docs/EMBEDDING.md)。

## 生命周期与存储

- 所有 Session/View 调用、事件处理及析构都在创建它们的 UI 线程；macOS 使用 AppKit 主线程。
- 只有同时设置 `storage_root` 和 `storage_key` 才会持久化。目录使用应用私有目录，key 按应用和账号保持稳定；不设置时为内存存储。
- 窗口关闭时调用 `begin_close()`，继续运行主循环直到 `closed` 为 `true`。检查 `Closed` 事件的错误信息，再释放 view 和 session；持久化完成前不要结束进程。
- 视图隐藏时仍需让主循环处理异步完成与存储。一个 session 同时只能关联一个 view。

## 运行消费示例与检查

```sh
node tools/consumer-smoke.mjs \
  --sdk /absolute/installed-sdk \
  --pack /absolute/portable-pack \
  --arch arm64
npm test
```

消费示例使用安装后的头文件和库，并检查重定位后的链接与原生运行。`--arch` 选择应与目标匹配；Apple Silicon 上的 x86_64 运行属于 Rosetta 结果。`npm test` 检查封装和工具，不能替代平台、GPU 或真机验证。

## 进一步阅读

- [嵌入、线程、事件、原生设备接口](docs/EMBEDDING.md)
- [账号、身份与支付宿主服务](ACCOUNT-SERVICES.md)
- [Windows 传感器](WINDOWS-SENSORS.md) · [Windows 音频](WINDOWS-AUDIO.md)
- [共享适配协议](CONTRACT.md) · [SDK 合约仓库](https://github.com/weapp-container/sdk-contract)
- [源码许可证](LICENSE) · [原始提取记录](ORIGIN.json) · [当前源码清单](SOURCE-INVENTORY.json)

构建工具会校验源码清单。修改 SDK 源码后须按 `tools/source-inventory.mjs` 的审核流程更新清单，历史 `ORIGIN.json` 保留原样。各平台实际构建与运行范围以对应产物的验证记录为准。
