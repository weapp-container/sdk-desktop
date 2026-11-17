<a id="windows-bluetooth-le-host-backend"></a>
# Windows Bluetooth LE 宿主后端

**简体中文** · [English](windows-bluetooth.en.md)


状态：**待接受**。`src/device_bluetooth_winrt.hpp` 包含真实的 Windows Bluetooth LE 中心端实现。`windows-bluetooth-sdk-review.json` 中的 SDK 签名审查使用了来自官方 `Microsoft.Windows.SDK.CPP@10.0.26100.9457` 包（Windows SDK 10.0.26100.0，C++/WinRT 2.0.250303.5）提取的 82 个头文件。该审查是源码证据，而非 Windows 构建或硬件运行。两者仍需同时满足。

后端注册 15 个异步 API 和 5 对启停监听器：适配器打开/关闭/状态、发现开始/停止/列表、已连接设备列表、连接创建/关闭、服务/特征值枚举、特征值读取/写入、通知/指示配置，以及实际协商的 MTU。它不注册配对、外围设备/广播、MTU 设置或已连接 RSSI 方法。这些方法仍不被本安装的后端支持。

创建时在附加的前台 HWND 的 UI 线程上启动 `FromBluetoothAddressAsync`。仅接受由该原生所有者实际发现的设备；重用原生的公共/随机地址类型。工作线程获取真实的 GattSession，启用 MaintainConnection，并在确认连接状态前执行未缓存的主服务发现。由于公共 API 无法区分其属性句柄，因此会拒绝重复的服务或特征值 UUID。不会返回合成设备或成功结果。

每个原生异步操作在终端状态、GetResults 以及成功 IAsyncInfo.Close 时保留其精确句柄。在取消检查之前，会先存储晚到的设备/会话获取结果，然后关闭。在发生异常时，完整保留原生服务结果，包括尚未访问的服务，且在 UUID 索引之前保留。在原生事件注册前，会预先保留通知映射槽位和交付门控。失败或取消的 CCCD 配置将关闭所拥有的连接，而不是保留订阅状态。

Windows 回调仅将有界原生事件入队。发现和通知生成会拒绝过期的输入，服务变更或断开连接将使已排队的连接和通知事件失效。Stop 会等待确切的 Watcher 的 Stopped/Aborted 状态。它不会停止其他应用程序的无线电发现。Close 会先使入队项失效，并重试精确的失败注销/Close 步骤；清理失败将阻塞所有者。事件移除使用生成的接口 ABI，因为 C++/WinRT 的 noexcept 事件移除投影会丢弃原生 HRESULT。成功的释放步骤仅被消耗一次。

资源限制是明确的：最多 32 个同时拥有的连接，256 个已发现的设备
设备，每连接支持 256 个主要服务和 256 个特性。原生属性写入限制为 512 字节；无响应写入还须遵守 `MaxPduSize - 3`。Windows 管理的协商 MTU 保持不变。`interval`/`powerLevel` 扫描覆盖请求被拒绝。请求的通知或指示类型必须由实际特性所支持。

Windows 版本需要官方的 C++/WinRT 投影（包含协程支持）、前台原生窗口、可用且启用的 BLE 适配器，以及宿主应用/包的蓝牙权限。当前 macOS 工作区尚无 Windows 编译器、操作系统或物理外围设备。Windows 编译链接、系统授权、物理操作、无线电/服务变更、延迟回调、背压、后台及账户变更清理等功能仍待接受批准。
