# Public preview release 0.1.0-preview.7.0

Status: source configuration merged; registry publication and platform acceptance remain pending. ABI stays 1. Do not claim completion until every registry version is anonymously downloadable and its real consumer passes.

Run from clean committed source. Historical tags, locks, ORIGIN and evidence are immutable. New builds use one clean core production commit. Never overwrite an existing tag, version, asset or checksum. A failed published version requires a new version.

## Order

1. Core `public-native.yml` builds nine native targets from one commit, collects licenses, validates hashes and packages the runtime. Download `public-runtime` and run the existing runtime publisher with `--execute` only after its offline verification passes.
2. Android, iOS and Desktop `build-public.yml` select that successful core run ID. Their source inventory and producer receipts bind the native input. Retain the original verified archive bytes.
3. Publish Android to Central, iOS/desktop prereleases to GitHub, then import their HTTP/hash-pinned Podspecs into `weapp-container/Specs`. Submit to Trunk while it accepts submissions and record its actual outcome.
4. Update current default runtime locks using the actual verified core manifest; preserve old locks under historical filenames. Pin iOS URLs/checksums with `--pin`, commit reviewed changes, and `--finalize` the original archives.
5. Publish Contract, React Native, then Flutter platform interface, five platform packages, and main package. Validate public consumers from fresh directories without GitHub authentication. Record build, simulator and real device results independently.

## Authentication

First npm publication requires an npm user with publish access to `@weapp-container`, browser login/2FA and `--access public --tag preview`. Configure npm trusted publishing for `publish-npm.yml` after the first publication.

First pub.dev publication requires human login for each new package; configure package-specific GitHub OIDC after creation. Maven Central needs verified `io.github.weapp-container`, Portal user-token credentials and a publicly discoverable PGP signing key. Set `MAVEN_CENTRAL_USERNAME`, `MAVEN_CENTRAL_PASSWORD`, `WEAPP_SIGNING_KEY`, and optional `WEAPP_SIGNING_PASSWORD` in the local process or repository secrets; never commit values. CocoaPods Trunk registration/email verification belongs to the account owner.

## Acceptance

Verify every archive SHA-256/size, architecture, dynamic dependency and license. Exercise interrupted downloads, mismatched hash/version and offline cache without fallback. Run npm pack and installed exports/types, Flutter analyze/publish dry-run, Pod lint, SwiftPM consumer and Central validation. Real consumers must load, display, interact, exchange host messages, navigate and close with the authored test pack. CI software rendering does not prove hardware or device acceptance.

## Desktop commands

Package each verified installation using `node tools/package-release.mjs --sdk <installation> --out <fresh-directory>`. On the same clean SDK commit, merge all three `release.json` paths with `--merge <comma-separated-paths> --out <fresh-directory>`. This generates `WeappContainerDesktop.podspec` from the actual macOS URL and hash. `--verify` checks local bytes; `--publish` verifies remote digests before publishing a prerelease.

The publish workflow accepts only a successful finalized artifact from its exact checkout commit. For iOS, run `finalize-public.yml` after the archive pins have been reviewed and committed, using the original successful `build-public.yml` run ID. Desktop `build-public.yml` merges its three successful platform jobs into `finalized-public-release` automatically, including the hash-pinned macOS Podspec. The finalizer and publisher reuse the original archive bytes.
