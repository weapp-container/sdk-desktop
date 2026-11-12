<a id="windows-sensors"></a>
# Windows 传感器

**简体中文** · [English](WINDOWS-SENSORS.en.md)


当桌面包装器使用官方 C++/WinRT 传感器头文件构建时，`DeviceApis` 会安装以下 12 个 API。每次启动均使用相应的真实 `Windows.Devices.Sensors` 类；若 `GetDefault()` 返回 null，则操作失败且不会生成传感器读数。

| 传感器 | 启动/停止 | 订阅/取消订阅 | 原生值 |
| --- | --- | --- | --- |
| 加速度计 | `wx.startAccelerometer` / `wx.stopAccelerometer` | `wx.onAccelerometerChange` / `wx.offAccelerometerChange` | 景观轴，单位为 g 的加速度 |
| 指南针 | `wx.startCompass` / `wx.stopCompass` | `wx.onCompassChange` / `wx.offCompassChange` | 磁北角度（度）；已知时的原生精度 |
| 陀螺仪 | `wx.startGyroscope` / `wx.stopGyroscope` | `wx.onGyroscopeChange` / `wx.offGyroscopeChange` | 景观轴，角速度从度/秒转换为弧度/秒 |

`normal`、`ui` 和 `game` 请求 200、60 和 20 毫秒的间隔，但受原生传感器的 `MinimumReportInterval` 限制。这些是请求的间隔，并非对设备采样频率的保证。指南针在锁定 API 合约中不提供采样选项。实现中不会将真北标记为磁北，且不提供未知精度的说明。

原生回调仅入队有界值。运行时所有者线程通过现有事件通道将其消耗。当队列满时，将终止该源。停止、权限重置、后台化、在结果交付前取消调用、以及账户所有者退出均会立即撤销已排队的值。停止操作会等待该传感器类型所有先前来源的完成，包括仍在消耗失败清理的拥有者。在旧的清理尚未完成时启动新实例将失败。

该源在 `remove_ReadingChanged` 返回检查成功的前提下，保留其 MTA、原生传感器、注册令牌和进程租约。预计的移除负载为 `noexcept`，因此实现调用相同的官方 SDK ABI 并检查其 HRESULT。移除失败或配置恢复将重试相同的剩余步骤。成功步骤不会重复执行。在释放源前，将恢复原始报告间隔和读数转换。租约防止两个 SDK 会话同时更改同一应用的传感器；直接使用这些原生类的宿主必须与 SDK 协调此类使用。

这三个 WinRT 类不暴露 `RequestAccessAsync`。若实际存在 `E_ACCESSDENIED`，将返回权限拒绝，原生 HRESULT 存在于 `errCode` 中。适配器不会伪造微信授权范围或操作系统权限提示。

有限生命周期目标可以在没有私有运行时存档的情况下运行：

```sh
cmake -S test/native-sources -B /tmp/weapp-native-sources
cmake --build /tmp/weapp-native-sources --config Release
ctest --test-dir /tmp/weapp-native-sources -C Release --output-on-failure
```

专用的 Windows 工作流还编译了完整的 Windows 适配器和
所有三个传感器的注册/移除特化版本均针对已安装的官方 Windows SDK。其 OBJECT 目标在运行时无链接。编译、受控源码生命周期测试、物理传感器接受度、以及打包运行时接受度为独立证据。传感器硬件、真实操作系统撤销行为、恢复行为，以及完整的私有运行时传输仍待物理 Windows 验证。现有发布归档文件和历史收据保留其原始版本和源码身份。
