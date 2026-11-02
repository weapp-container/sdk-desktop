# 桌面宿主账号、授权与支付

`include/weapp/host_services.hpp` 提供独立的 C++17 业务适配器。它复用 runtime 的
共享账号 owner；`HostAccountManager` 只包装换号、查询、取消、重试及本地数据删除，
不维护第二套 epoch。`HostApiContext` 保留完整的 runtime wire context，账号 epoch
与 deadline 使用十进制字符串，不能经 JSON double 转换。

## 接入顺序

1. 以宿主可信安装记录构造 `Config.host_services` 的 application/account。
   `packRoot` 必须对应已验证包；哈希不代替发行者认证。
2. 创建 `HostServicesClient`，配置 HTTPS origin 与 native-only host token provider。
   provider 按收到的可信应用/账号选择凭据，不能把 token 交给 JS。
3. 等待 `prepare_configuration` 的异步结果，再配置并创建 session。SDK 不通过阻塞
   UI 回调伪造同步 `identity.getState`；JS 启动前的状态来自实际后端。
4. 创建 `HostServices`，配置 `HostConsentPresenter::native(parent, status)` 或自己的
   原生授权 presenter，最后在 `load()` 前 `install(session)`。业务 `on_event` 监听继续
   收到其原有事件。无 consent presenter 不注册交互授权能力；无支付渠道不注册支付能力。
5. 兑换登录 code、恢复前台、撤销授权或刷新 token 后，先取
   `current_context(context)`，再 `refresh(context)`。Future 由 owner `pump()` 结算。
   旧 owner 的 context 在获取凭据或发请求前拒绝，迟到结果也不能更新新 owner 的同步快照。

```cpp
HostBackendOptions options;
options.endpoint = "https://host-backend.example";
options.host_token = [accounts](const HostApiContext& context) {
  return accounts->bearer_for(context.application, context.account);
};
Status status;
auto backend = HostServicesClient::create(std::move(options), status);
// Handle status and await backend->prepare_configuration(trusted_configuration)
// on the host's async executor before creating/loading the Session.
```

SDK 仅调用参考服务的宿主入口：登录 code、应用会话检查、身份状态、资料、授权策略、
consent、手机号码、应用退出及订单 launch。独立 app client secret、issuer secret、
业务 app bearer 和下单金额留在后端；SDK 不提供这些凭据的 guest 入口。
选择请求完整携带 `miniAppId/packRoot/expectedPrincipalId/expectedPrincipalEpoch`。

## 网络与授权 UI

macOS 使用 ephemeral `NSURLSession`，Windows 使用 `WinHTTP`，Linux 使用 7.85+
`libcurl`；没有 libcurl 的 Linux 构建明确拒绝创建后端客户端。请求由两个 worker
执行，队列最多 128 项，活动工作最多 256 项；重定向、cookie 和自动宿主认证关闭，
系统证书校验保持启用，响应上限 256 KiB，单次请求最长 15 秒。开发 HTTP 仅允许显式
开启的 `127.0.0.1/localhost/[::1]` origin。取消中断原生请求，结果只在 owner pump
处理；宿主凭据不自动附加到任意小程序网络请求。

原生授权 UI 为 AppKit sheet、属于宿主 HWND 的 Win32 窗口与 GTK dialog，必须绑定
可见宿主 parent。展示后端注册的应用、用途和权限版本；资料与手机号分别确认，
拒绝仍写回后端，窗口关闭/换号取消不等于同意。自定义 presenter 的 completion
必须且只能结算一次，收到 cancellation 时关闭自己的 UI 并结算取消。
测试中的 controlled presenter 只验证协议及生命周期，不能作为原生 UI 验收。

`wx.getUserProfile` 仍须通过 facade 的非空 `desc` 检查；原生 UI 展示的用途来自后端
policy，不以 guest desc 替换。`wx.getUserInfo` 只读取已有授权。手机号返回应用作用域
一次性 code，通过真实 `getPhoneNumber` 按钮路由进入内部宿主 API，不向 guest 暴露
`wx.getPhoneNumber`。`revoke(context, kind)` 撤销指定应用/用户的单类授权并刷新快照。

应用退出成功后，runtime 撤销该应用的全部关联 controller，并重建其 JS owner；
撤销先于旧 JS continuation，旧 logout callback/Promise 不进入替换 owner。宿主从
账号切换事件和查询终态观察完成，新入口的同步快照反映 app 未登录。其他小程序
与宿主账号保留，后端既有独立资料/手机号 consent 也不被应用退出自动删除。

## 桌面支付边界

`HostPaymentChannel` 是宿主正式渠道与 SDK 的类型化接入边界。guest 仅传 `orderId`；
SDK 从后端取得与可信应用/账号/epoch 绑定的 launch，拒绝 guest 支付签名。
桌面只接受显式渠道对应的 `wechat-qr/alipay-qr`（native）或
`wechat-h5/alipay-h5`（h5）。APP 与 JSAPI 参数不能传给桌面渠道，也不自动转换。
同一时刻只允许一个支付 UI；渠道支持重叠时拒绝选择。

渠道 `launch` 的 completion 使用 `HostApiError`，失败时可在 `details` 中保留渠道原始
`errCode`（数字或字符串）、`errMsg` 及公开扩展字段；容器仍负责外层错误分类和调用
上下文。completion 必须在其自有 UI/任务终结并释放后结算；成功仅表示渠道
交互完成，不代表付款。QR 收银台可以提供明确的“已完成”按钮，但该按钮不证明付款。
SDK 再按同一订单轮询
后端，只有服务端 `paid` 才向 JS 返回 `serverConfirmed:true`。等待上限可配置为
1 秒至 5 分钟，默认 15 秒；等待超时、关闭与换号只终止客户端等待，不撤销、退款或
重新发起扣款。渠道必须响应 cancellation，停止自己的 UI/等待并完成 callback。
微信/支付宝的产品开通、QR/H5 宿主展示器、回跳及正式凭据由宿主显式配置。
当前交付渠道接口与严密的服务端确认流程；内置三端正式 QR/H5 展示器仍待实现验收。

本地 reference 通道必须同时显式安装对应 channel 并开启
`allow_reference_payments`，只接受 `reference/app/reference`，绝不用于正式失败回退。
参考服务的验签与订单归属仍照常执行，不登记为真实商户交易。

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

安装身份适配器后，换号先取消并排空该适配器的请求、订阅与授权/支付 UI，然后
运行已有宿主 cleanup hook。hook 应在自己的任务中安装新宿主凭据；hook 成功后，SDK
自动请求新账号的真实后端状态，并绑定同一 operation 提交，再 ACK。后端预取失败
同样保持封锁并要求显式 retry；退出宿主账号使用 null principal，准备确定的未登录
快照，不调用旧 bearer，也不生成沙箱账号。若需要后端撤销宿主会话或删除账号，
宿主仍负责执行实际后端动作；本地 data deletion 回执不是后端撤销证明。

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

启用实际参考服务回归：

```sh
cmake -S . -B build -DWEAPP_CORE_DIR=/absolute/verified/runtime \
  -DWEAPP_DESKTOP_BUILD_TESTS=ON \
  -DWEAPP_HOST_SERVICES_REFERENCE_ROOT=/absolute/tools/host-services-reference
cmake --build build
ctest --test-dir build --output-on-failure
```

Node.js 24 测试服务保留 app/issuer/callback secret，仅将临时 native bearer 写入
0600 的任务私有输入文件，退出后删除。公开 C++ SDK → 真实 native transport → 实际
HTTP/SQLite 后端 → FFI/QuickJS 链路验证 callback/Promise、独立授权及撤销、账号切换、
迟到结果、应用退出、reference 签名通知与订单确认，以及传输取消/重定向/大小限制。
原生授权 UI、正式 QR/H5 渠道、物理设备、归档新装、GPU 呈现和商户交易仍分别待验收；
编译、有限测试、模拟器和真实设备证据互不替代。
