# 桌面账号切换的宿主清理边界

当前 C++ `Session` 和 C session 复用 runtime 的共享 `HostAccountManager`。相同
`hostAppId` / `tenantId` 的所有 controller 共用账号 epoch；`switchAccount` 立即撤销旧
身份，原生设备和 GPU 清理从 `accountSwitch` 的 `revoking` 事件开始。公共 controller
保持稳定，内部 JS session、页面栈及查询参数由 runtime 重建。

## 异步清理与身份快照

在加载前调用 `set_account_cleanup_handler`。SDK 先排空自身设备并解绑 GPU，然后在
创建该 session 的 UI 线程调用宿主 hook 一次。hook 返回
`std::shared_future<AccountCleanupResult>`，宿主在自己的执行器完成剩余资源清理和
新身份预取；SDK 的 `pump()` 只作非阻塞检查，并在所属线程把 `sync_values` 通过
`prepareAccountHostServiceSnapshots` 提交给同一个 operation，随后才确认 cleanup。

```cpp
session->set_account_cleanup_handler([backend](const AccountCleanupContext& context) {
  // backend owns its credentials, cancellation and executor. It returns a
  // shared_future and never calls Session from a worker thread.
  return backend->drainAndPrepare(context.operation_id, context.application, context.account);
});
```

同步 identity snapshot 仍由宿主真实后端提供。凭据保留在宿主；`sync_values` 不应包含
宿主 token。异步结果不能在 UI 线程以 `wait()` / `get()` 阻塞；deferred future 明确
失败。旧 operation 的 task 必须先到终态，随后才能启动最新 operation 的 hook；
旧快照不会提交给新 owner。错误保持封锁，宿主修复条件后调用
`retry_account_cleanup()`。重试不恢复旧身份。

每次 `revoking` 都用 runtime 提供的完整快照替换 SDK replay cache，缺失该元数据时
清空缓存。runtime 仅保留小程序 metadata 与设备快照：`getAccountInfoSync`、
`getSystemSetting`、`getDeviceInfo`、`getWindowInfo`、`getAppBaseInfo`、
`getSystemInfoSync`、`getBatteryInfoSync`；身份状态重置为该宿主账号的未授权应用
状态，其他自定义快照置空。hook 的新值只能合入这个 operation 的新基线，省略值
不会继承旧用户授权。OS 保留名单由 runtime 维护，SDK 不另建一份可分歧的名单。

`close()` 在已启动的 hook 仍未结束时返回 `ERR_RESOURCE_CLEANUP_PENDING`；持续
`pump()` 会在 task 结束后推进关闭。清理失败时继续保持封锁，错误通过事件报告并可显式
重试。关闭请求不会因清理成功而启动新账号 JS。宿主应始终完成自己签发的 task，
然后等待 `Closed` 再销毁 controller。

C API 提供对应的 `weapp_desktop_session_set_account_cleanup_handler`。callback 借用
application/account 字段，并获得一个自有 completion。调用
`weapp_desktop_account_cleanup_complete(&completion, sync_values, error_code,
error_message, &error)` 只完成 promise，可以从其他线程调用；它不进入 runtime。
每个 completion 必须且只能结算一次，成功消费后指针置空。取消也必须以错误结算。

## C ABI 兼容

原有 C state 与 event 的数字不变，`IDENTITY_SWITCHING` 与 `EVENT_ACCOUNT_SWITCH`
追加到各自枚举末尾。C++ 到 C 使用显式映射，不依赖两个枚举的内部排列。
`weapp_desktop_config` 布局不变；新增配置、注册和命令函数分别为
`weapp_desktop_session_configure_host_services`、
`weapp_desktop_session_register_api_handler` 和
`weapp_desktop_session_host_services_command`。

## 验证范围与剩余接入

`WEAPP_DESKTOP_BUILD_TESTS=ON` 会生成独立 authored fixture，运行公开 C++/C SDK →
实际 native FFI → QuickJS 的账号生命周期回归，包括迟到 login、共享 epoch、连续
换号、清理失败重试、异步快照、关闭、身份快照省略后的 owner 重建/重载和旧 ABI 数值。此测试不打开窗口，不请求权限，
不作为 release 归档新装、GPU 呈现、物理设备或商户交易验收。

桌面类型化身份/支付服务客户端、完整共享账号公共入口、参考后端 HTTPS 传输和
原生资料/手机号授权 UI 尚需交付。原始 command 与 cleanup hook 只是接入边界，
不能登记为完整身份或支付闭环。桌面官方支付渠道与商户条件也须分别实现并验收。
