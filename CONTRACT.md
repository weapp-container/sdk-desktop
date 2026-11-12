<a id="sdk-contract-v1"></a>
# SDK 协议 v1

**简体中文** · [English](CONTRACT.en.md)


SDK 版本：`0.1.0-preview.5.3`；C ABI 版本：`1`。

平台适配器通过 `weapp_container_sdk.h` 调用独立授权的运行时。应用程序使用平台的带类型会话和视图 API。以下 JSON 为内部适配器协议；应用程序不会构造运行时桥接信封。

<a id="session-and-scheduling"></a>
## 会话与调度

在单个线程上创建和操作会话。Android 使用专用的 HandlerThread；Apple 适配器使用主线程。**绝不要**在另一个线程上的帧/纹理回调中调用会话。用户事件应在原生命令返回后交付。独立调度完成泵的执行，与显示回调分离：隐藏视图将停止播放和应用计时器，而 HTTP、宿主完成、取消、超时和持久化操作在操作系统允许执行时仍会继续。

每个视图挂载是独占的。解挂保留会话。在加载替换内容或关闭前，必须停止渲染并解挂原生 GPU 上下文。在替换成功或失败后都必须重新挂载：失败的候选者将保留之前的运行时。候选者存储、请求和消息在启动和帧准备成功前保持私有。

宿主拥有稳定的 `storageKey`，并提供应用数据 `storageRoot`。**故意省略两者**将选择仅内存存储。持久键有一个写入者，包括在关闭期间，直到工作线程释放锁。重新加载将保持租约。**不要**从会话标识或包摘要中派生密钥。

<a id="internal-commands"></a>
## 内部命令

每个结果均为 `{ok:true,value:...}` 或 `{ok:false,error:{code,message}}`。
每个 C 结果缓冲区必须**精确一次**释放。即使操作无应用值返回，也必须检查结果。

| 命令 | 请求/响应 |
| --- | --- |
| `load` | `{packPath, route?, options?, initialHostChrome?}`；一个已验证的本地包目录 |
| `classifyDevPack` | `{packPath}`；完整验证且不更改会话，返回 `{reloadKind:"inPlace"|"restart"}` |
| `applyDevPack` | `{packPath,route?,options?,initialHostChrome?}`；在候选者接受后返回 `{reloadKind,state}`，**不涉及 GPU 播放** |
| `devState` | 仅读取的 `{state,pages:[{id,route,data}]}` 检查 |
| `clearDevStorage` | 清除当前 wx 存储；适配器必须等待 `flushStorage` 以确保持久完成 |
| `registerHostMethod` | 首次加载前返回 `{name}` |
| `completeHostCall` | 返回 `{callId,result}` 或 `{callId,error:{code,message,retryable}}` |
| `sendMessage` | 返回包含 JSON 数据的 `{message}` |
| `navigate` | 返回 `{route,query?,replace?}` |
| `back` | 返回 `{}` |
| `switchTab` | 返回 `{route}`；分发运行时的 `wx.switchTab` 语义 |
| `inspectPackChrome` | 返回 `{packPath,route?}`；验证所有 blob 并解析静态宿主 Chrome，**无需创建 guest 运行时** |
| `hostChrome` | `HostChromeGeometry` schema 1；测量胶囊/内容矩形和单调布局修订 |
| `setPreviewOccluded` | `{occluded:boolean}`；可独立于应用生命周期暂停预览输入/呈现 |
| `lifecycle` | `{event:"show"}` 或 `{event:"hide"}` |
| `input` | `{events:[...]}` 在逻辑视口坐标中 |
| `resize` | `{width,height}` 在逻辑像素中 |
| `windowMetrics` | `HostWindowMetrics`，包括实际像素比和安全区域 |
| `presented` | `{generation,revision}` 在绘制前捕获；确认在成功宿主呈现后的确切原生GPU帧 |
| `pump` | `{}` 使用SDK时钟；适配器不得用运行时间替代Unix纳秒 |
| `state` | 当前版本，路由、视口、前景、表面、`frameRevision`、`presentationRequired` 和 `inputEnabled` |
| `capabilities` | 构建特性、实际GPU呈现状态、API和不支持的操作 |
| `takeEvents` | 排空有界事件队列；在命令执行和泵操作后调用 |
| `flushStorage` | 返回 `{pending:true,ticket}` 或 `{pending:false}` |
| `close` | 开始异步关闭；返回 `{pending:true,ticket}` 或 `{pending:false,status:"closed"}` |

观察公共API中的 `ready`、`navigation`、`chrome`、`message`、`error` 和 `closed`。
适配器也消费 `frame`、`hostCall`、`hostCallCancelled` 和 `storageFlushed`。
仅在匹配的 `storageFlushed` 事件上解析刷新票据；仅在 `closed` 上解析关闭，并传播其持久性错误。关闭期间持续泵取。完成之后，在其所属线程上销毁C句柄。同步C关闭函数是最终清理屏障，而非面向UI的异步关闭API。

宿主调用标识符中已包含版本标识。它们是不透明的。最多仅执行一次；在取消时释放宿主任务。延迟或重复的原生完成无法在替换运行时中执行JavaScript。切勿在其自身事件分发中调用FFI；应将完成结果回传至会话队列。

<a id="development-reload-preview4"></a>
## 开发重载（预览.4）

在发送开发命令前，必须要求 `capabilities.pack.devReload.protocolVersion == "1.0"` 和选定的重载类型。旧版本运行时对未知命令返回 `ERR_UNSUPPORTED_CAPABILITY`；不得模拟成功。开发播放器创建一个会话，使用 `gpuRequired:true`：缺少编译的原生GPU提供程序的运行时将拒绝创建。省略 `gpuRequired` 保持现有SDK行为。此设置从未能证明帧已被呈现。

`devDiagnostics:true` 显式启用有界开发控制台诊断。它默认为false，不会改变普通生产会话的追踪。适配器通过其现有的所属线程事件流转发诊断事件。
分类信息与模拟器共享。一个经过完全验证的 WXML/WXSS/静态资源编辑可以保留当前会话、页面数据、页面栈、存储和定时器。候选视图/布局准备在接受前运行。JS/模块、配置、依赖、组件图或不安全组件/WXS 的更改将重启会话。重启会保留持久化存储，并尝试恢复当前路由（如果仍可用）。重启时需分离 GPU 表面；在原地编辑时不得分离 GPU 表面。适配器将在运行时拥有线程上序列化 classify/apply 操作，并在重启成功或失败后重新附加。

包验证和预接受启动失败将保留已接受的包。一旦新运行时/帧被接受，后续的 GPU 失败被视为呈现失败：暂停输入并重试，或明确重新加载之前的包。重新加载之前的包无法恢复其旧的 JS 堆。

`clearDevStorage` 会清除 wx 存储，不会影响已下载的包或当前页面数据。

可选的 `weapp_container_sdk_gl.h` 扩展提供一个由宿主拥有的 GL 函数解析器，用于桌面 OpenGL 3.3。原始的 GLES ABI 头文件保持不变。在当前拥有上下文中解析函数；切勿共享该上下文，也绝不在 fence/完成回调中调用 FFI。

<a id="presentation"></a>
## 呈现

使用 SDK 中的 `frameRevision` 而非应用帧 ID 来检测重绘：滚动重绘可能有意保留应用帧 ID。一个提交会准备经过验证的原生绘制资源，而无需 CPU 页面回读。GLES 和 Metal 会直接绘制到宿主目标。CPU 呈现器将获取一个不可变的引用计数 RGBA8 帧；像素为预乘，物理尺寸为逻辑视口乘以像素比例。

当 `state.presentationRequired` 为 true 时，准备帧或成功调用 GLES/Metal 绘制入口点不会触发呈现。在表面附加后、绘制前，需读取 `generation` 和 `frameRevision`；仅在成功完成 EGL 交换、完成 Metal 可绘制对象呈现，或等效的可信宿主呈现收据后，才调用 `presented` 以该捕获身份进行确认。该命令在会话拥有线程上运行，绝不能在 GPU 回调中运行。该命令返回当前状态，并可释放页面 `onReady` 效果，这些效果接受更新的帧。应单独调度该新版本；绝不能用最新状态重新标记旧的收据。在调整大小、重新加载、分离或重新附加之后的延迟收据将返回 `ERR_CANCELLED` 错误，并且不会污染当前会话。每次附加都会准备一个新版本，包括在附加后重新附加的情况。
替换失败，因此旧表面的收据无法启用其后继者。

`state.inputEnabled` 用于判断当前已接受的帧是否已越过运行时呈现边界。对于未呈现的原生 GPU 帧，输入将被拒绝。SDK 会报告 `ready`，表示已提交打包；但这并不能证明宿主表面已将其呈现。当 `presentationRequired` 为 false 时，运行时使用其同步光栅边界；CPU 平台的呈现器仍需单独追踪成功的原生绘制，并且不得声称 GPU 附件或收据。

GLES 的 attach/present/detach 操作需要拥有者当前的上下文。上下文丢失时使用 abandon。SDK 清理操作不得终止与宿主渲染共享的显示。Metal 目标必须为同设备、BGRA8Unorm、2D、采样数为一、渲染目标使用方式、非仅限帧缓冲区，且尺寸必须与声明的完全一致。在 GPU 处理完成前必须保留纹理。Flutter 通过在相同队列上使用完成屏障发布完成的 IOSurface/CVPixelBuffer；纹理回调仅保留已完成的帧。

帧句柄可存活于会话之外，并可跨线程，具有独立保留的引用。帧所有权从不会跨越 Dart 或 React Native 的 JSON 通道。

<a id="mini-program-api"></a>
## 小程序 API

包含 `types/weapp-container.d.ts`。`weappContainer.invoke()` 返回一个 Promise；`postMessage`、`onMessage` 和 `offMessage` 提供双向 JSON 消息。`wx.request` 执行真正的异步 HTTP(S)，支持 JSON/text/ArrayBuffer、超时和中断。上传、下载和 WebSocket 不属于预览功能。

<a id="native-host-chrome-preview4"></a>
## 原生宿主 Chrome（预览版 4）

在启用 Player shell 前，需协商 `capabilities.chrome.protocolVersion == "1.0"` 加上 `geometry`、`inspectPack` 和 `occlusion`。这些新增功能使用 ABI 1；旧版运行时必须在能力协商中失败，绝不能猜测坐标。

`inspectPackChrome` 从经过 SHA-256 验证的 JSON 模块（或旧版资源）中解析配置。它不执行 App/Page 代码、不获取存储、也不附加 GPU。解析初始导航布局，测量实际内容表面，提供原生的 `windowMetrics` 和 `hostChrome`，然后是 `load`。在加载前可接受 `windowMetrics`；它始终是视口/显示更新的唯一边界。内容矩形的尺寸必须与初始窗口指标一致。

`state.chrome` 和 `{type:"chrome",generation,chrome}` 事件包含声明中定义的 `PageChrome` 观察值。应用窗口默认值、页面覆盖值以及每页动态的标题/颜色由单个运行时解析器统一管理。快照包含实际运行时页面栈和配置的系统 TabBar；`custom:true` 的 tabBars 仍为应用组件，绝不会是虚假的原生栏。
一个 Chrome 版本与一个内容帧版本是独立的：更改标题或颜色不会重新评估视图脚本，也不会证明新的 GPU 帧已被呈现。标签图标是打包资源引用，而不是对任意宿主文件的访问。

`hostChrome` 使用实际的当前屏幕逻辑像素矩形，而不是设备像素。`contentRect` 不会重新解释 `screenTop` 或 `safeArea`，其原点从不被用作第二个输入坐标变换。原生输入始终相对于测量的内容表面。运行时通过 `wx.getMenuButtonBoundingClientRect` 返回测量的胶囊。重复使用完全相同的观察是幂等的；观察变更需要更大的版本号。可选的会话/页面绑定会拒绝已退役所有者的更新。无效更新保持之前的宿主几何不变。在实时缩放期间，输入和呈现收据保持禁用，而内容尺寸与最新原生视口观察不同。

遮挡会退化当前的输入/呈现权限并取消活动联系。它不会调用应用的隐藏/显示钩子，不会停止业务定时器，不会替换会话或清除存储。解除遮挡会提交一个全新的渲染版本；在 GPU 模式下，需要再次提供有效的呈现收据。在被遮挡期间持续泵送。下载不可变更新可能继续，但播放器会推迟候选激活，直到其预览可见。

`capabilities.chrome.transactionalRestart:true` 协商替换布局的暂存。重启可能携带顶层的 `initialHostChrome`，该对象在调用前会被测量并转换为实际屏幕逻辑坐标。其可选的所有者绑定引用旧的已接受会话/页面。只有新的隔离候选者在其调用 `App.onLaunch` / `Page.onLoad` 前会接收到其视口和胶囊几何；启动失败将保留之前的配置、几何和实例。`initialHostChrome` 对于原地更新会被拒绝。

成功后，`state.hostChrome` 会被重新绑定到新所有者。原生表面可能仍保留其之前的物理尺寸：`state.chromeGeometryReady` 会保持为假，直到原生 `windowMetrics` 观察与新内容尺寸匹配。绘制/呈现/输入必须等待此标志和正常的呈现收据。在暂存期间的旧尺寸观察无法将新会话回退到旧尺寸。原生包装器的仅变换 `prepareHostChrome` 必须使用 Flutter-根到屏幕变换，而不是将前景矩形对齐到旧内容视图原点；准备过程永远不会改变已接受的运行时。

<a id="account-cleanup-and-c-abi"></a>
## 账户清理和 C ABI
账户撤销在 `revoking` 事件中启动原生清理。可选的预加载账户清理处理器返回一个共享任务；仅 UI 线程泵应用与操作绑定的快照并确认清理。待处理或失败的清理会阻断身份输入；显式重试永远不会恢复旧所有者。详情请参见 [ACCOUNT-SERVICES.md](ACCOUNT-SERVICES.md)。C API 会附加身份切换/账户切换值，并显式映射 C++ 枚举，保留每个原始数值和原始配置布局。

C++ `host_services.hpp` 适配器保留了精确的 API 上下文，并使用现有的 Rust 共享账户管理器。原生传输回调永远不会进入 JS。所有者的泵会处理有界请求/订阅，应用身份快照，并独立于宿主事件监听器发出事件。账户替换会在宿主清理钩子之前释放相关资源，然后获取并绑定新账户状态，再发送清理确认。预检失败将导致旧所有者被撤销。原生个人资料/手机同意使用服务器注册的目的/版本；缺少的呈现者/通道不会安装相应能力。支付启动成功并不意味着已付费：只有后端确认的付费订单才被视为成功完成。官方桌面 QR/H5 展示和商户接受仍为独立、未满足的条件。
