# Desktop SDK Consumer

**简体中文** · [English](README.en.md)

这个消费者使用已安装的 Desktop SDK 头文件和库运行两个原生 View。它演示窗口布局、双会话隔离、宿主消息、业务 pack 导航、指针输入、重设尺寸、持久化和关闭。

## 运行

从核心仓库生成 fixture，再在目标系统构建匹配的 Desktop 安装目录：

```sh
node sdk/contract/fixtures/make-acceptance-pack.mjs --out artifacts/hello-pack
node scripts/sdk-native-build.mjs --target aarch64-apple-darwin --out artifacts/sdk/native
node scripts/sdk-licenses.mjs --target aarch64-apple-darwin --features quickjs-native,skia-gpu-metal
node sdk/desktop/tools/build.mjs --target macos-arm64 \
  --native-root artifacts/sdk/native --licenses-root artifacts/sdk/licenses \
  --pack artifacts/hello-pack --out artifacts/sdk/desktop --verify
node sdk/desktop/tools/consumer-smoke.mjs \
  --sdk artifacts/sdk/desktop/macos-arm64 \
  --pack artifacts/hello-pack \
  --arch arm64
```

Linux 使用 `--target linux-x64` 并在需要时通过 `xvfb-run` 提供显示，Windows 使用 `--target windows-x64`。目标平台需要 CMake 3.21+、C++17 和对应的窗口工具链。`consumer-smoke.mjs` 只接受新目录和已验证安装目录，并在退出时清理临时存储。

## 示例行为

`showcase_smoke.hpp` 创建两个独立会话，等待真实布局和首帧，向第一个会话发送恢复消息，执行原生点击和重设尺寸，再关闭两个会话。`smoke.hpp` 覆盖更通用的 C ABI 消费流程。报告包含运行库版本、目标架构、输入 pack 和清理结果。
