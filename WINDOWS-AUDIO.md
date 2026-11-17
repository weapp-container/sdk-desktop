<a id="windows-inneraudiocontext-source-implementation"></a>
# Windows 内部音频上下文源实现

**简体中文** · [English](WINDOWS-AUDIO.en.md)


预览。6 桌面包装器在使用官方 Windows Media Playback C++/WinRT 头文件编译时会安装原生 `InnerAudioContext` 家族。这会增加 36 个目录条目：工厂、六个可写偏好设置（`src`、`autoplay`、`loop`、`startTime`、`volume`、`playbackRate`）、四个只读运行时快照、五个方法，以及十个开/关事件对。`obeyMuteSwitch` 和 `referrerPolicy` 故意不可用：Windows 本身没有对应的实现策略。这仅是参数部分覆盖，而非完整媒体或七平台验收。

<a id="sources-and-format-boundary"></a>
## 源和格式边界

每个非空源都会创建一个全新的 WinRT `MediaPlayer`、`MediaSource` 和所拥有的 `InMemoryRandomAccessStream`。它从不会将主机文件 URL、网络 URL 或播放列表传递给操作系统解析器。输入必须是严格验证的、自包含的 RIFF/WAVE 流，包含一个 16 字节的 PCM/IEEE 浮点格式块和一个帧对齐的非空数据块。仅允许 `JUNK`/`PAD ` 附加块。PCM 支持 8/16/24/32 位；IEEE 浮点支持 32/64 位；两者均支持 1–8 通道和 8–192 kHz。操作系统编解码器/设备错误仍会被返回。MP3、AAC、可扩展 WAVE、播放列表、远程 URL 和其他格式需要未来明确的源策略工作。

- `wxfile://user/...`: 在管理的文件操作锁下读取；大小上限为32 MiB。
- 相对包路径：需要 `capabilities.hostServices.apiResourceChunks` 等于 `1.0`。仅由运行时选择确切待设置源的资源。拥有线程会将完整的原始调用上下文、偏移量以及最多1 MiB的长度传递给 `hostApiResource`；原生代码从不选择路径或哈希值。总长度必须保持一致，且上限为16,711,680字节。没有源在所有分块到达前会传递给解码器。旧版运行时会明确拒绝这种源形式。

<a id="native-ownership-and-observable-behavior"></a>
## 原生所有权和可观察行为

一个上下文拥有一个 MTA 工作线程、有界命令队列（保留 32 个操作）和有界原生通知队列（64 个通知）。每个 SDK 所有者最多拥有八个上下文。平台回调仅将原生事实入队。只有运行时所有者线程进入桥接和 JS。入流溢出故障会回收确切的所有者；在声称播放正常的情况下，它不会丢弃任意事件流。

替换 `src`、权限重置和后台化会立即撤销前一个源的事件和回执交付令牌。之前已排队的成功的结果也变为取消失败。被撤销的排队包负载在队列锁下被释放；活跃的原生写入保留其精确缓冲区，直到终端完成。新的源在旧的事件注册、播放器、源、流和异步写入均被确认关闭前无法获取。稳定的公共上下文在源重置后仍然存在。本实现中后台播放不可用。

`play`/`pause` 等待实际的原生播放状态。`seek` 等待原生 `MediaPlaybackSession.SeekCompleted`；`Seeking` 在接受位置写入后入队，`Seeked` 仅在终端回调之后触发。`stop` 等待原生暂停和倒退操作完成后再调用 `Stop`。失败/取消/超时的命令在结算前会清空解码器，以保留新源的上下文。命令取消并不表示原生操作已经停止。原生清理失败会保留精确的令牌/句柄以便重试。`destroy` 和 SDK 退役仅在工作线程释放其原生对象后才被确认。在清理过程中，退役操作保持非阻塞状态，仍会返回 `ERR_RESOURCE_CLEANUP_PENDING`。

位置和持续时间将原生的 100 ns `TimeSpan` 帧转换为秒。
`buffered` 使用官方定义的 `DownloadProgress × NaturalDuration` 时间端点，从不使用字节进度。只读属性来自实际的原生快照；生命周期转换不会在原生关闭完成前声明暂停/重置。体积范围为 0–1，播放速率范围为 0.5–2，开始/跳转时间必须符合原生非负帧范围。`autoplay` 和 `startTime` 在源获取期间生效。系统媒体命令管理器已禁用，因此这些私有播放器不会接管宿主级的传输控制。

`MediaFailed` 保留其原始的 `ExtendedErrorCode`、`ErrorMessage` 和公共 `nativeMediaError` 枚举；激活/操作的 HRESULT 值保留其原始有符号代码和权限拒绝类别。解码失败会保留公共上下文以供其他源使用。预检/包/ WAVE 源失败也会精确发出一个对象 `Error`；解码器已报告的原生 `MediaFailed` 不会重复报告。容器取消/截止失败不会伪装成解码器错误，且取消已验收的源将撤销其排队事件以及回执。默认凭据或伪造的播放结果永远不会被替换。

<a id="verification-and-remaining-acceptance"></a>
## 验证和剩余验收

`test/device_audio.cpp` 有八个有限的组别用于 WAVE 验证/目录、精确销毁排水、延迟回执栅栏、后台/重置、延迟原生获取/原始失败、有界命令/背压/退化、以及精确包块上下文/协商/进度，加上恰好一次源错误事件和取消源栅栏。其受控后端验证所有权和调度，而不是操作系统解码器或实际的 JS。

`test/native-sources` 还会编译每个 Windows 包装器翻译单元以及明确的 MediaPlayer 实例化代码，针对运行时的官方 Windows SDK。该仅源码目标不会链接私有运行时归档。所有有限音频和传感器组均在 Windows 上运行。工作流会记录编译器和 SDK 的身份，并上传 CMake/CTest 日志。

实时音频输出、支持的硬件、快速的原生事件定时、在物理 Windows 宿主上的拒绝和后台行为、实际的 QuickJS/V8 面板和 SDK/原生传输、完整的运行时链接，以及包资源 FFI 集成仍待验收。没有模拟、源代码清单或绿色源码编译可以替代这些检查。历史 `ORIGIN.json`、构件锁定和设备回执仍绑定到其原始版本。

官方语义引用已绑定到
`MicrosoftDocs/winrt-api@4c1e9164521785f82dc63ab57a5ee4644f9d1888`：
`windows.media.playback/mediaplayer.md`，
`mediaplaybacksession_seekcompleted.md`，和
`mediaplaybacksession_downloadprogress.md`。Windows 运行器提供了实际用于检查事件移除的 C++/WinRT ABI。
