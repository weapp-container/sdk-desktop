# Weapp Container Desktop SDK

Version **0.1.0-preview.4** · [weapp-container organization](https://github.com/weapp-container) · [SDK contract](https://github.com/weapp-container/sdk-contract)

This independent repository contains the **Apache-2.0 host wrapper and consumer source**. The runtime implementation remains closed source. Its separately licensed binaries are obtained only from the private [`weapp-container/runtime-artifacts`](https://github.com/weapp-container/runtime-artifacts) repository. SDK source licensing does not grant a runtime binary license. Distributions retain `RUNTIME-LICENSE` and all third-party notices alongside the wrapper's `LICENSE`.

A fresh clone needs Node.js 24+, Python 3 and the platform toolchain below. It does **not** need a parent monorepo, Rust, Cargo, a git submodule or private implementation source.

This checkout targets **0.1.0-preview.4** for Dev Player. The default runtime
lock pins the real private `runtime-v0.1.0-preview.4` release by exact size and
SHA-256. Historical preview.2 releases, reports and source identities are retained
in history; they are not attributed to this version. Only the listed target pins
are available. Windows/Linux runtime distributions remain blocked. Binary
download/installation does not establish Player, GPU or device acceptance.

## Obtain the native runtime

Use the reviewed `runtime-artifacts.lock.json` shipped with the matching SDK release. It locks the exact runtime version, platform, architecture, ABI **1**, artifact size and SHA-256. An absent pin, missing architecture, hash/version mismatch or unavailable engine is an error; there is no download of `latest`, software substitution or silent fallback.

Set `WEAPP_CONTAINER_RUNTIME_TOKEN` in your credential environment with read access to the private artifact repository. Do not place credentials in source files or CLI arguments. Then run:

Network artifact transfers have a single 15-minute deadline covering headers,
redirects and the complete body; release metadata has a separate 30-second
deadline. For slower connections, set `WEAPP_CONTAINER_RUNTIME_TIMEOUT_MS` to a
positive base-10 integer in milliseconds, up to `3600000` (one hour). Invalid
values return `ERR_RUNTIME_OPTIONS`; transfer failures and timeouts return
sanitized `ERR_RUNTIME_NETWORK`. Explicit API/CLI overrides are documented in the
[shared SDK contract](https://github.com/weapp-container/sdk-contract).

```sh
node tools/prepare-runtime.mjs
```

For an authorized offline copy use `--artifact-dir /absolute/runtime-archives`, or `WEAPP_CONTAINER_RUNTIME_ARTIFACT_DIR`. Files still undergo the same committed pin, hash, version and ABI checks. `--manifest /absolute/reviewed-lock.json` explicitly selects another reviewed lock; `--version` overrides are intentionally unavailable in this wrapper.

Preparation atomically installs verified inputs under `.runtime/install/native` and `.runtime/install/licenses`. It preserves the prior installation on failure. Generated archives, binaries, caches and credentials are excluded from Git. `--native-root` below also accepts an explicitly supplied local distribution with matching per-target `build.json`, SHA-256, the reviewed C header and runtime license; local input does not imply private-release authentication.

Exact runtime distributions retain the producer's original `build.json` and bind its bytes with `runtime-distribution.json`. The selection accounts for every producer artifact and can omit only the unused iOS dynamic library or Android static archive. Required libraries, the reviewed header and platform notices remain mandatory. Inputs without this selection receipt continue to verify the full producer inventory.

## Build and embed

Requirements: CMake 3.21+, a C++17 compiler and the target UI SDK. macOS 12+ uses NSView/Metal. Dev Player uses a child HWND/WGL OpenGL 3.3 preview on Windows 10 x64 and GtkGLArea/OpenGL 3.3 on Linux (Ubuntu 22.04/glibc 2.35/GTK 3.24 baseline). These GPU integrations require platform execution evidence. Existing software embedding paths remain separate; they cannot satisfy the Player GPU requirement.

```sh
node tools/prepare-runtime.mjs --target macos-universal
node tools/build.mjs --target macos-universal --native-root .runtime/install/native --licenses-root .runtime/install/licenses --pack /absolute/verified-portable-pack --verify
```

Other targets are `macos-arm64`, `macos-x64`, `windows-x64` and `linux-x64`; build on the corresponding OS. `--skip-consumer` only packages. `--verify` requires a running display and exercises the installed native consumer. No successful build is counted as a GUI test.

Installations under `artifacts/desktop/TARGET` include public C/C++ headers, native and wrapper static archives, relative CMake exports, notices and an independent consumer. Consumers use:

```cmake
find_package(WeappContainerDesktop CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE WeappContainer::Desktop)
```

Pass the install prefix as `CMAKE_PREFIX_PATH`. The host owns its existing event loop and window. [C/C++ embedding, threads and lifecycle](docs/EMBEDDING.md).

```sh
node tools/consumer-smoke.mjs --sdk /absolute/installed-sdk --pack /absolute/verified-portable-pack --arch arm64
```

The relocation check copies the installation before compiling/linking without Rust. Rosetta x64 is explicitly an emulated result. Linux X11/Wayland and Windows GUI execution require their own environments; source availability is not an acceptance claim.

## Validation and provenance

```sh
npm test
```

These are wrapper input-contract tests, not runtime, GPU, device, performance or WeChat parity results. This split starts with a new version: historical preview.1 results are not attributed to preview.2. Platform build/UI reports must be regenerated against the exact pinned runtime before claiming that scope passed. Native IME, complete accessibility and advanced gestures remain separate limitations.

The 2026-10-27 preview.2 report for wrapper source `360e789` records a universal macOS package, independent C/C++ build/link and arm64 native execution with actual Metal presentation. The consumer uses synthetic Cocoa event dispatch to exercise two views, input/scrolling, navigation, resize, hidden completion pumping, window visibility, owner-thread enforcement and durable reopening. It does not establish physical OS input. macOS x86_64 execution, Windows/Linux execution, performance and WeChat parity remain unmeasured; their unavailable runtime pins fail explicitly.

`ORIGIN.json` lists the allowlisted source paths, original source SHA-256, transformed destination SHA-256 and reviewed C-header contract hash. This repository includes no core implementation, Git history from the original repository, binary runtime, cache or credentials.

### Reviewing later SDK source changes

`ORIGIN.json` stays as the historical extraction receipt. `SOURCE-INVENTORY.json` records the current reviewed wrapper source paths, SHA-256, sizes and executable modes, including the historical receipt's unchanged hash. The origin check and package build verify this exact inventory in the standalone SDK checkout and reject untracked or unreviewed paths, modified bytes, symlinks, core sources, binaries and caches. This source review is separate from build, artifact and device acceptance evidence.

After reviewing an update, stage all intended new source files, refresh explicitly, then stage the inventory:

```sh
git add <reviewed-files>
node tools/source-inventory.mjs --write --version 0.1.0-preview.4 --reason "Describe the reviewed changes"
git add SOURCE-INVENTORY.json
node tools/check-origin.mjs
npm test
```

Commit source and inventory together before building distributable packages. `sourceBaseCommit` identifies the checkout used to prepare the source review; it does not bind subsequent build evidence to that earlier commit. Never rewrite historical receipts or fabricate runtime archive lock hashes to make this check pass.
