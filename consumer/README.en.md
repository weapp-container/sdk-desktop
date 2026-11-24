# Desktop SDK Consumer

[简体中文](README.md) · **English**

This consumer runs two native Views with an installed Desktop SDK header and library. It demonstrates window layout, dual-session isolation, host messages, business-pack navigation, pointer input, resize, persistence, and close.

## Run

Create the fixture from the core repository, then build a matching Desktop installation on the target system:

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

Use `--target linux-x64` on Linux and provide a display with `xvfb-run` when needed. Use `--target windows-x64` on Windows. The target needs CMake 3.21+, a C++17 compiler, and its native window toolchain. `consumer-smoke.mjs` accepts only a fresh directory and a verified installation, then removes its temporary storage on exit.

## Example Behavior

`showcase_smoke.hpp` creates two independent sessions, waits for real layout and first frames, sends a restore message to the first session, performs native pointer input and resize, and closes both sessions. `smoke.hpp` covers the general C ABI consumption path. The receipt records the runtime version, target architecture, input pack, and cleanup result.
