# Weapp Container Desktop SDK

首次公开发布正在准备；安装命令须在对应注册源出现本版本后使用。历史私有归档及其锁文件保持原样。

版本 `0.1.0-preview.7.0`；CMake 3.21+、C++17。公开 [GitHub prerelease](https://github.com/weapp-container/sdk-desktop/releases/tag/v0.1.0-preview.7.0) 提供：

- `WeappContainerDesktop-0.1.0-preview.7.0-macos-universal.tar.gz`：macOS 12+，arm64/x86_64。
- `WeappContainerDesktop-0.1.0-preview.7.0-linux-x86_64.tar.gz`：x86_64，GTK 3.24+、GIO 2.66+、fontconfig、epoxy。
- `WeappContainerDesktop-0.1.0-preview.7.0-windows-x86_64.tar.gz`：Windows x86_64，MSVC。

先按 `SHA256SUMS` 验证归档，再解压至任意 SDK 安装目录。归档包含可迁移 CMake 配置、头文件、运行库和许可。消费工程无需 Rust 或兄弟源码仓库。

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

## Published preview erratum

The immutable `0.1.0-preview.7.0` archives contain a stale `WEAPP_DESKTOP_VERSION` C header macro (`0.1.0-preview.6.0`). The archive metadata and linked runtime are `0.1.0-preview.7.0`, ABI 1. Current source corrects the macro and checks version coordination. Existing archives, tags and hashes are retained; distributing the corrected header requires a new version. Public archive consumers passed on macOS, Linux and Windows.
