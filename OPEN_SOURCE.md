<a id="open-source-runtime-integration"></a>
# 开源运行时集成

**简体中文** · [English](OPEN_SOURCE.en.md)


当前核心运行时和 SDK 源码采用 Apache-2.0 许可，各独立组件保留其原始许可证。详情请参见 [核心源码构建指南](https://github.com/weapp-container/weapp-container/blob/main/docs/sdk-repositories.md)。

本仓库现有的运行时锁定文件记录了先前构建的归档文件。其版本、大小、哈希值、分发配置及下载授权保持不变。仅凭当前源码检出并不能证明这些二进制文件已被重新构建。新的开源二进制版本发布需独立完成构建、打包及平台验证。在进行源码测试时，应使用 SDK 提供的明确本地运行时/SDK 输入选项，并确保与匹配的源码构建版本一致；无需使用私有构件即可完成源码测试。

保持 C ABI 1、现有的公共包名以及协商的特性支持。保留 ORIGIN.json 作为历史溯源记录，并在审查本仓库提供的源码变更后更新 SOURCE-INVENTORY.json。
