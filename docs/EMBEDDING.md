<a id="weapp-container-desktop-sdk-preview"></a>
# Weapp 容器桌面 SDK 预览

**简体中文** · [English](EMBEDDING.en.md)


安装的 CMake 包在每个会话中嵌入一个真实的 QuickJS/Skia 运行时，该运行时位于应用程序拥有的 NSView、子 HWND 或 GTK3 绘制区域中。应用程序拥有其主线程和顶层窗口。没有 WebView 或 JavaScript 桥接传输像素。此预览可在本地安装，且不会发布到注册表。

| 目标 | 原生呈现 | 基线 |
| --- | --- | --- |
| macOS | NSView + CAMetalLayer，GPU完成操作在分离前 | macOS 12+，arm64/x86_64 通用归档 |
| Windows | child HWND + 预乘 BGRA DIB/AlphaBlend | Windows 10 x64, MSVC, 宿主每显示器 DPI 意识 |
| Linux | GtkDrawingArea + Cairo, Skia CPU 帧捕获 | Ubuntu 22.04 x64, glibc 2.35+, GTK 3.24, X11/Wayland |

源可用性并不能保证目标的可用性。包回执和消费者日志记录了实际构建和运行的平台。Rosetta 执行明确不是 Intel 硬件的结果。原生输入包括鼠标/触摸、滚轮和键盘事件；本预览不提供原生文本输入控件或 IME 组合功能。

<a id="build-and-consume"></a>
## 构建和消费

使用 [根目录 README](../README.md) 中的独立命令。运行时存档在私有二进制条款下单独提供；无需父源代码检出或 Rust 编译器。

<a id="c-embedding"></a>
## C++ 插入

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

`View::native_handle()` 返回原生子对象。将其放入宿主的布局中，并调用 `set_bounds` 进行手动布局；边界以逻辑像素为单位。GTK 非固定容器控制实际分配，因此其布局优先于大小请求。SDK 会观察显示缩放变化。零大小的视图会隐藏呈现，但保留最后一个正的视口。GTK 宿主必须在加载前允许首次实际大小分配；否则加载将返回 `ERR_NOT_LAID_OUT`。一个会话一次只能有一个视图，这对 GPU 和位图呈现器有强制要求。销毁视图会保留其会话；附加新视图将恢复该会话。

会话构建是惰性的：首次加载需要实际的视图/显示指标。原生布局提供这些指标，或者无头宿主提供 `Config.metrics`。因此，模块作用域的窗口 API 在首次执行时会看到宿主的观察结果。
除非同时提供了 `storage_key` 和 `storage_root`，否则存储仅存在于内存中。
使用应用程序/账户特定的键和应用私有目录。

每个会话/视图方法、回调和销毁操作都属于创建 UI 线程。macOS 要求使用 AppKit 主线程；GTK 需要初始化的宿主 GTK 显示。SDK 在宿主循环中为每个会话安装一个定时器。当视图被隐藏或关闭时，定时器仍会继续处理 HTTP 请求、宿主调用和存储操作；而 JavaScript 定时器、输入和呈现操作则在隐藏时暂停。可显式调用 `pump()` 以实现确定性的宿主集成，但不能替代宿主循环。在非主线程执行命令将返回错误。在异线程上销毁 C++ 对象属于合同违规，将直接终止，而不是触碰线程绑定的运行时状态。不可变的 `Frame` 复制可以跨线程传递，并且可存活于会话之外。

`navigate`、`back`、`send_message`、`register_host_method`、`complete_host_call`、`fail_host_call`、`pointer`、`wheel`、`key`、`flush_storage` 和 `close` 是类型化方法。`Value` 是一种类型化的业务数据树；协议信封保留在内部。HostCall 事件在 `fields` 中包含 `callId`、`method` 和 `payload`。根据需要将业务工作转发到另一个队列，然后在拥有者 UI 线程上交付完成。`fail_host_call` 将主机失败映射为运行时的 `invalid_request` 错误，并附带提供的消息。就绪、导航、帧、消息、取消、错误、存储和关闭事件在运行时调用之外被传递。

加载替换在候选者启动前停止呈现并回收/断开GPU。如果候选者失败，则保留并重新连接之前的运行时。
`close()` 也会安全断开并启动一个异步的持久关闭。在收到 `EventType::Closed` 之前持续运行宿主循环，检查其错误字段，然后销毁视图和会话。C++析构函数是同步的最后清理手段；显式关闭可使宿主可观测到持久化完成。回调可能更改监听器或启动其他操作；嵌套事件循环不会递归回收回调。回调中不得抛出异常。

C 消费者使用 `weapp/desktop.h`，包括无类型指针的 Value 构建器/访问器。
没有 C 函数接受原始命令名或 JSON 字符串。回调字段的借用用于此目的；使用 `weapp_desktop_value_clone` 保留它们。C 会话的 `destroy` 要求处于关闭状态，并在成功时清除调用者的句柄。

<a id="installed-consumer-verification"></a>
## 已安装的消费者验证

`consumer` 是一个独立的 CMake 项目；它仅包含已安装的头文件。它测试了 C 和 C++ 链接、两个会话/视图、原生指针/键适配器、状态隔离、失败候选回滚、DPR 缩放、隐藏服务泵送、跨线程拒绝、持久关闭、保留帧生命周期和存储锁重新打开。在 Linux 上，使用 `xvfb-run` 和无头的 Weston 合成器（通过 `GDK_BACKEND=wayland`）分别执行相同的 consumer。consumer 会创建和销毁自己的宿主窗口和临时存储。

内部编码器为 nlohmann/json 3.11.3（MIT），来自其上游标记的单个头文件。其许可证随安装包一同提供。如果存在，打包工具会复制原生分发的许可证清单；发布打包必须保留 `the private runtime distribution` 生成的完整第三方清单。


<a id="native-device-services"></a>
## 原生设备服务

C++ 在会话创建前通过 `Config.native_device_apis =
std::make_shared<DeviceApiOptions>()`（来自 `weapp/device_apis.hpp`）启用选项。使用精确的 HTTPS 原点配置 `allowed_network_origins`；空列表将拒绝原生网络访问。`private_root` 选择 SDK 的持久化管理文件存储。核心部分提供应用程序/账户租约和真实的原生目录；来宾模块只能看到受保护的 `wx.env.USER_DATA_PATH` 值 `wxfile://user`。不要从来宾选项中提供文件系统根目录。

SDK 在模块求值之前安装平台处理程序，并在创建 UI 线程上处理它们的完成。可重用的 `DeviceApis` 接口也支持借用原生会话（例如 Flutter）：其命令回调必须使用该会话，其平台视图必须提供 `set_native_parent`，且其拥有者循环必须调用 `pump`、`update_components`、`set_foreground` 和 `retire`。它不得为设备操作创建第二个会话。

管理同步文件系统调用在当前 JavaScript 执行周期内执行实际的磁盘操作。异步文件系统操作和原生媒体发布使用相同的范围锁、配额和原子替换。单个二进制/读/写数据块限制为 1 MiB，管理范围限制为 32 MiB，目录枚举限制为 4096 个条目。保存的设置和权限同意存储在核心的私有辅助目录中。账户变更会在核心确认清理之前释放原生资源，并在核心确认清理后释放其文件租约。原生回调在该确认之后无法发布文件。

在 macOS 上，驱动程序使用 NSURLSession 的 HTTPS/WSS、AppKit 对话框/剪贴板、CoreLocation、CoreWLAN、CoreBluetooth 中心端/GATT、AVFoundation 音频/摄像头、Contacts 和 EventKit。包含的应用程序必须声明摄像头、麦克风、位置、蓝牙、联系人和日历的使用描述，以启用其服务，同时在沙盒环境中还需声明正常的沙盒/网络/设备权限。录制需要明确的 AAC 或 WAV 格式；当前无法获取编码帧回调。原生的视频/音频源必须是管理文件，并且首先需通过受信任的网络策略下载。摄像头/视频像素将按照正常的 Skia 绘制顺序进入，包含裁剪和变换。没有子层叠加视图可以绕过合成。应用权限拒绝将持续存在，直到用户在应用设置中重置这些权限；重置将停止敏感的活动资源。生物识别支持/注册检测是原生的，而 SOTER 签名生成仍不支持，因为 LocalAuthentication 无法生成微信 SOTER 证明。

Windows 使用 WinHTTP、系统剪贴板/对话框/电源信息、原生 WLAN 扫描/连接通知以及系统设置/地图处理程序。Wi-Fi 连接支持开放或 WPA2 个人网络，使用临时配置文件；已接受的操作系统连接尝试在被取消后可能仍会完成，而被取消的访客连接则会保持终止状态。Linux 使用 GTK/GIO、可选的 libcurl 7.85+、NetworkManager 的 Wi-Fi 发现/当前接入点、freedesktop 屏幕抑制器以及 sysfs 背光控制。Linux 服务可能因硬件不可用、服务缺失或操作系统权限不足而拒绝连接。NetworkManager 在请求扫描后会列出其观察到的接入点缓存。

前景定位在 macOS 上使用 CoreLocation，在 Windows 上使用 C++/WinRT Geolocator（当 SDK 头文件和运行时激活可用时），在 Linux 上使用 GeoClue（当系统服务被拥有或可激活时）。Linux 宿主必须将可信的 `DeviceApiOptions::linux_desktop_id` 设置为其已安装的桌面文件 ID（带或不带 `.desktop` 后缀）；空值将禁用定位路由。来宾参数无法提供此身份。GeoClue 服务策略和操作系统许可仍然适用。Windows 权限获取需要附加的可见宿主 HWND、其 UI 线程和前景根窗口。缺少这些能力将导致路由未注册。

所有三个驱动程序仅接受 WGS84 坐标系；不支持的坐标系将被拒绝。
`wx.getFuzzyLocation` 请求 `scope.userFuzzyLocation`，将坐标四舍五入到 0.01 度，报告精度至少为  1500 米，并移除海拔和速度元数据。 精确位置请求使用 `scope.userLocation`。 位置超时时间为 100..120000 毫秒（默认值为 30000）。 启动监听会等待其初始的原生启动/定位；一个活跃的监听器可接受相同选项的重复启动，但若精度或超时时间发生变更，则必须先停止监听器。 Windows 每一秒查询一次定位器的定位结果；Linux 每 500 毫秒观察一次 GeoClue 客户端的定位信息。 两种实现均不保证提供操作系统事件流。

停止会取消排队的监视器交付和待处理的应用程序/原生授权。
Windows/Linux 停止回执会等待其原生源被销毁，包括在取消后延迟获取；已取消的工作程序会保留其槽位，直到清理完成，且不超过八个拥有者限制。Windows 会保留相同的异步操作，直到完成回调或终端状态允许 `Close`，在重试不确定的关闭失败时不会释放操作或会话。GeoClue 会尝试 Stop/DeleteClient，然后确认其私有 D-Bus 连接的关闭；GeoClue 将私有断开记录为客户端清理。连接关闭失败会保留连接以供重试。原生清理失败持续存在会保持停止/退休待处理，而不是确认已释放资源。`DeviceApis::retire` 在原生工作程序耗尽时返回 `ERR_RESOURCE_CLEANUP_PENDING`，并将其控制权交还给拥有者循环。宿主会保留拥有者并重试退休；它们必须在成功之前销毁它。桌面账户切换循环会自动重试，并且只有在成功后才会发送核心清理确认。没有成功明确退休的销毁会保留同步安全回退，并可能等待原生清理。
macOS 会分离旧的管理器/代理，并在获取新版本前捕获新版本；已退休管理器的排队回调无法进入该监视器。入口饱和会停止管理器在其拥有者上运行，并发出有界终端错误，而不是静默继续收集被丢弃的位置更新。重置、后台和退休操作都会取消排队交付和待处理授权；返回前台需要重新启动。其他桌面设备家族在宿主未添加其自身注册处理器之前均不支持。

Windows BLE 中心在可用的蓝牙 SDK 头文件和运行时激活时使用 C++/WinRT。Windows 原生实现和 SDK 头文件探针使用 C++20 以支持投影的标准协程功能；公共包装头文件仍与 C++17 消费者兼容。原生发现提供设备身份和地址类型；客体参数无法替代该元数据。连接权限准备在附加的前台宿主 UI 线程上运行。异步操作在完成时仍保持所有权，包括取消后的延迟连接结果，清理操作会释放精确的观察器、事件注册、GATT 服务/会话和设备引用。`wx.getBLEMTU` 报告原生 GATT 会话的最大 PDU 大小；配对、外围/服务器模式以及 MTU 变更不会由这些桌面后端注册。

Linux BLE 通过私有系统 D-Bus 连接使用 BlueZ。默认情况下，它暴露适配器状态、低功耗发现和已观察到的设备。发现过滤器和会话属于该私有客户端；清理操作不会开启或关闭全局适配器，也不会移除配对设备。中心连接/GATT API 要求可信主机设置 `DeviceApiOptions::linux_bluetooth_exclusive=true`（Flutter 主机选项 `linuxBluetoothExclusive`）。这表示部署将阻止其他客户端访问目标设备：BlueZ 设备1 连接/断开操作是全局的，断开连接可终止所有配置文件。因此，此选项是一种部署协议，而非操作系统强制的隔离。SDK 还会拒绝已连接的设备以及同一 SDK 实例中存在冲突的租约。它会保留每个租约，直到断开连接且虚假的 Connected 属性确认清理完成；丢失私有 D-Bus 对等端不会被用作该设备确认的替代。

BlueZ GATT 发现拒绝模糊的重复 UUID，将每个设备绑定到 64 个服务，将每个宿主绑定到 256 个特性，使用实际的 ReadValue/WriteValue 操作，并观察通知 Value 信号。BlueZ 写入操作限制为 512 字节。BlueZ 和 Windows 均不注册合成的 `wx.getBLEDeviceRSSI`；广告的 RSSI 仍保留在观察到的设备记录中，由原生发现提供。BlueZ 也未提供 `wx.getBLEMTU`，因为该后端没有通用的客户端 MTU 查询功能。不支持的路由保持未注册。

共享的 Windows/Linux BLE 主体允许八个原生主体，包括已取消但仍在回收的主体，32 个待处理命令，256 个观察到的设备和 256 个通知传递令牌。关闭适配器会等待所有先前的原生主体。停止发现会作废已排队的启动请求；禁用通知和设备关闭会在原生释放完成前撤销已排队的特征数据传递。读取事件携带其请求取消令牌。重置/后台/退休操作会撤销适配器和待处理授权；前台返回需要重新打开。原生断开连接会撤销已排队的通知传递；已作废的 BlueZ 服务数据库会在缓存的属性路径可被重用之前退场。入口饱和会终止通过有界主体队列的收集；被拒绝的命令回执会保留错误完成状态，直到主体可以回收它。可执行文件 `weapp_device_bluetooth_test` 使用虚拟的 OS 后端和实际的链接核心目录配置检查这些边界。将真实 BlueZ 源码与本地 GIO 头文件进行隔离编译，以验证 C++/GIO 兼容性；它不会执行 BlueZ。Windows 目标编译、完整的 Linux GTK 构建、物理射频/GATT 行为和操作系统许可均属于独立的验收工作。

macOS 源代码有一个本地的 CMake 构建检查。这并非摄像头、蓝牙、麦克风、联系人、日历或隐私对话框的接受证据。Windows 目标编译、完整的 Linux GTK 目标构建以及物理设备验证仍为独立要求；现有的源码路径无法证明操作系统的一致性。GeoClue 源代码在本地 macOS 机器上针对真实的 GIO 头文件进行独立编译检查；此过程不会执行 GeoClue。

可选的 CMake `WEAPP_DESKTOP_BUILD_TESTS=ON` 目标会构建 `weapp_device_permissions_test`；使用命令 `ctest --test-dir <build-dir>
--output-on-failure` 运行该测试。该测试使用伪造的 OS 同意回调来验证生产调度器、持久化同意、所有者队列和保留对象清理功能。待处理的应用程序授权、延迟的 OS 授权以及已排队的完成操作在权限重置后都无法逃脱；新的授权可以在之后恢复。这是一项可执行生命周期契约的验证，而非硬件或隐私对话的接受。`weapp_device_location_test` 可执行文件还使用伪造的原生源码来验证模糊投影/作用域、已排队事件撤销、延迟获取、停止/清空顺序、清理失败恢复、后台行为以及有界取消的工作者。它通过实际链接的核心 C ABI 提交适配器的完整配置；这验证了目录注册，而非操作系统位置。

原生请求取消和管理文件发布在 macOS、Windows 和 Linux 上共享一个完成屏障。在发布之前取消会阻止下载文件和结果入队。一个已由该屏障接受的写入操作会在取消返回前完成其有界文件发布和结果入队；取消不会回滚已接受的文件。可选的 `weapp_device_publication_test` 可执行文件使用真实的管理文件和受控的工作器时序，以及所有者退休后的拒绝来检查两种顺序。它不建立 WinHTTP、libcurl、NSURLSession 或硬件验收。

Windows 提供 `wx.connectSocket`，且 `SocketTask` 通过异步 WinHTTP 发送、关闭以及 Open/Message/Error/Close 事件族。Linux 仅在链接了 libcurl **8.16+** 时才会提供该事件族，同时 WebSocket 符号也存在，且加载的库报告了 `wss` 协议。如果其归档文件包含此路由，则安装的 CMake 包要求版本为 8.16+；仅包含 HTTP 的归档文件仍保留对 7.85+ 的依赖。缺少该开关将导致整个 SocketTask 事件族在原生服务配置中缺失。

| 源路由 | 平台传输 | 激活要求 | 验收仍需 |
| --- | --- | --- | --- |
| HTTPS 请求/下载/上传 | WinHTTP / 可选 libcurl | Windows 10 SDK / libcurl 7.85+ | 目标构建和实际传输/取消 |
| WSS SocketTask | Async WinHTTP WebSocket | Windows 10 SDK | Windows 构建和真实的 TLS/关闭/取消 |
| WSS SocketTask | libcurl WebSocket | Headers/library 8.16+，链接的 API，运行时 `wss` 协议 | Linux 构建和真实的 TLS/关闭/取消 |
| BLE 中心端/GATT | C++/WinRT | Bluetooth SDK 头文件、激活、连接同意的宿主前端 UI | Windows 构建和实际无线电/GATT/延迟结果清理 |
| LE 发现 | BlueZ 私有 D-Bus 客户端 | 运行或可激活的 BlueZ 服务，供电适配器 | Linux 构建和客户端范围的发现清理 |
| BLE 中心/GATT | BlueZ 私有 D-Bus 客户端 + 排他性目标租用 | 显式可信宿主排他性，无预先存在的连接 | Linux 构建和真实的 GATT/断开/恢复 |

这些 WSS 路由仅接受无片段的 ASCII 无凭据 URL，并将它们的规范 **HTTPS 源** 与 `allowed_network_origins` 进行匹配。
它们保留系统证书/主机名验证，拒绝重定向，验证提供和选定的子协议，并拒绝保留或注入的请求头。文本/二进制消息限制为 1 MiB；每个套接字有 32 个排队命令和 4 MiB 的排队输出字节。每个所有者最多有八个原生套接字工作线程，包括已取消但仍正在处理的线程。入站消息共享有界的所有者队列；饱和时会关闭传输并为错误/关闭通知保留空间。所有客体事件和结果均由所有者线程传递。原生句柄清理完成后，关闭操作才算完成；退休会取消并等待工作线程结束。WinHTTP 回调状态在最终的 `HANDLE_CLOSING` 通知后仍保持有效。

With `WEAPP_DESKTOP_BUILD_TESTS=ON`, `weapp_device_socket_protocol_test`
检查严格的输入、片段、二进制字节、配额、终端队列容量和 UTF-8 处理，无需链接运行时或联系服务器。
`weapp_device_socket_test` 链接 SDK 并测试其实际调度器、所有者交付、取消边界和工作线程回收，使用虚拟的 OS 传输。它还通过 `configureHostServices` 将适配器的完整配置提交给实际运行时，检查未在锁定目录中列出的 API 被拒绝的情况，并检查被拒绝的更新是否保留有效配置。
SocketTask 和 RecorderManager 仅暴露已登记的 `on` 事件；运行时通过生命周期管理释放订阅，没有虚构的 `off` API。
在 UNIX 系统中，使用 libcurl 8.16+，`weapp_curl_socket_compile` 使用真实头文件编译实际的可选传输，`weapp_curl_socket_headers_test` 执行有界 C 回调，处理格式错误和重复的响应头。
这些检查并不能建立 Linux 目标构建、Windows 构建或真实的网络/设备验收。较旧的 macOS NSURLSession SocketTask 实现是独立的源代码路径。
