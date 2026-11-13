# 公开预览发行 0.1.0-preview.7.1

**简体中文** · [English](PUBLIC-RELEASE.en.md)

状态：源码配置已合入，注册源发布与平台验收仍待完成。ABI 保持为 1。只有每个注册源版本均可匿名下载、真实消费程序也通过后，才能报告发行完成。

从干净且已提交的源码运行。历史 tag、锁文件、ORIGIN 和证据保持不可变。新构建使用同一个干净的核心生产提交，不覆盖已有 tag、版本、资产或校验和；已发布版本失败时使用新版本。

## 顺序

1. 核心 `public-native.yml` 从同一提交构建九个原生目标，收集许可、验证哈希并打包运行库。下载 `public-runtime`，仅在离线验证通过后，使用已有运行库发布器的 `--execute`。
2. Android、iOS、Desktop 的 `build-public.yml` 选择该成功核心运行的 ID。源码清单与 producer 回执绑定原生输入，保留已验证归档的原始字节。
3. 将 Android 发布到 Central，将 iOS/desktop prerelease 发布到 GitHub，再把 HTTP/哈希固定的 Podspec 导入 `weapp-container/Specs`。Trunk 仍接受投稿时提交，并记录实际结果。
4. 使用实际已验证的核心 manifest 更新当前默认运行库锁，旧锁以历史文件名保留。用 `--pin` 固定 iOS URL/checksum，提交复核后的变化，再对原始归档执行 `--finalize`。
5. 依次发布 Contract、React Native，再发布 Flutter platform interface、五个平台包和主包。从无 GitHub 认证的新目录验证公开消费程序，分别记录构建、模拟器与真机结果。

## 认证

首次 npm 发布需要对 `@weapp-container` 有发布权限的 npm 用户、浏览器登录/2FA，以及 `--access public --tag preview`。首次发布后，为 `publish-npm.yml` 配置 npm trusted publishing。

首次 pub.dev 发布需要每个新包的人工登录，创建后配置包专属 GitHub OIDC。Maven Central 需要验证 `io.github.weapp-container`、Portal 用户 token 凭据及可公开发现的 PGP 签名公钥。在本地进程或仓库 secrets 中设置 `MAVEN_CENTRAL_USERNAME`、`MAVEN_CENTRAL_PASSWORD`、`WEAPP_SIGNING_KEY` 和可选的 `WEAPP_SIGNING_PASSWORD`，不提交其值。CocoaPods Trunk 注册和邮件验证由账号所有者完成。

## 验收

验证每个归档的 SHA-256/大小、架构、动态依赖和许可。覆盖下载中断、哈希/版本不匹配及无回退的离线缓存。执行 npm pack、安装后 exports/types、Flutter analyze/publish dry-run、Pod lint、SwiftPM consumer 和 Central validation。真实消费程序必须加载、显示、交互、交换宿主消息、导航并关闭自有测试 pack。CI 软件渲染不证明硬件或设备验收。

## Desktop 命令

使用 `node tools/package-release.mjs --sdk <installation> --out <fresh-directory>` 打包每个已验证安装。在同一干净 SDK 提交上，通过 `--merge <comma-separated-paths> --out <fresh-directory>` 合并三个 `release.json` 路径，根据实际 macOS URL/哈希生成 `WeappContainerDesktop.podspec`。`--verify` 检查本地字节；`--publish` 在发布 prerelease 前验证远程 digest。


发布工作流只接受与当前 checkout 提交完全一致且成功完成 finalize 的产物。iOS 在归档 pin 已复核并提交后，使用原成功 `build-public.yml` 的 run ID 执行 `finalize-public.yml`。Desktop 的 `build-public.yml` 自动将三个平台成功作业合并为 `finalized-public-release`，包含固定哈希的 macOS Podspec。finalize 和发布均复用原始归档字节。
