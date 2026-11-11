# Open-source runtime integration

The current core runtime and SDK source are Apache-2.0, with separately identified
components retaining their licenses. See the [core source build guide](https://github.com/weapp-container/weapp-container/blob/main/docs/sdk-repositories.md).

This repository's existing runtime lock describes previously built archives. Its
versions, sizes, hashes, distribution profiles and download authorization remain
unchanged. A current source checkout is not evidence that those binaries were
rebuilt. A new open-source binary release requires independent build, packaging
and platform validation. Use the SDK's explicit local runtime/SDK input options
with a matching source build; no private artifact is needed for source tests.

Keep C ABI 1, existing public package names and negotiated capabilities. Keep
ORIGIN.json as historical provenance and refresh SOURCE-INVENTORY.json after
reviewing source changes where this repository supplies an inventory tool.
