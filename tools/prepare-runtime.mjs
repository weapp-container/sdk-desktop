#!/usr/bin/env node
import { distributionOptions } from './distribution-identity.mjs';
import { cpSync, existsSync, mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { downloadRuntimeArtifact } from './runtime-artifacts.mjs';
import { copyVerifiedTree, option, readNative, readNotices, SDK_VERSION, sha256 } from './native-input.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const expectedDistribution = distributionOptions(args);
const platform = JSON.parse(readFileSync(resolve(root, 'package.json'), 'utf8')).weappContainerPlatform;
const matrix = {
  android: [['android', 'arm64-v8a', 'aarch64-linux-android'], ['android', 'x86_64', 'x86_64-linux-android']],
  ios: [['ios', 'arm64-device', 'aarch64-apple-ios'], ['ios', 'arm64-simulator', 'aarch64-apple-ios-sim'], ['ios', 'x86_64-simulator', 'x86_64-apple-ios']],
  'macos-universal': [['macos', 'arm64', 'aarch64-apple-darwin'], ['macos', 'x86_64', 'x86_64-apple-darwin']],
  'macos-arm64': [['macos', 'arm64', 'aarch64-apple-darwin']],
  'macos-x64': [['macos', 'x86_64', 'x86_64-apple-darwin']],
  'windows-x64': [['windows', 'x86_64', 'x86_64-pc-windows-msvc']],
  'linux-x64': [['linux', 'x86_64', 'x86_64-unknown-linux-gnu']],
};
const target = platform === 'desktop' ? option(args, '--target', process.platform === 'darwin' ? 'macos-universal' : process.platform === 'win32' ? 'windows-x64' : 'linux-x64') : platform;
const selected = matrix[target];
if (!selected) throw new Error(`Unsupported SDK target: ${target}`);
const manifestPath = resolve(option(args, '--manifest', resolve(root, 'runtime-artifacts.lock.json')));
if (!existsSync(manifestPath)) throw new Error('The reviewed runtime-artifacts.lock.json is required; unpinned downloads are forbidden');
const installationRoot = resolve(option(args, '--install-root', resolve(root, '.runtime/install')));
const stage = `${installationRoot}.preparing-${process.pid}`;
const backup = `${installationRoot}.previous-${process.pid}`;
if (existsSync(stage) || existsSync(backup)) throw new Error('Runtime staging or backup already exists');
mkdirSync(stage, { recursive: true });
try {
  let licenseHash;
  const receipts = [];
  for (const [runtimePlatform, arch, triple] of selected) {
    const native = resolve(stage, 'native', triple);
    const result = await downloadRuntimeArtifact({
      manifestPath, ...expectedDistribution, version: SDK_VERSION, platform: runtimePlatform, arch,
      component: 'runtime', abiVersion: 1,
      outDirectory: resolve(root, '.runtime/downloads'), installDirectory: native,
      artifactDirectory: option(args, '--artifact-dir', process.env.WEAPP_CONTAINER_RUNTIME_ARTIFACT_DIR),
    });
    const library = runtimePlatform === 'android' ? 'libweapp_container_sdk.so' : runtimePlatform === 'windows' ? 'weapp_container_sdk.dll.lib' : 'libweapp_container_sdk.a';
    readNative(resolve(stage, 'native'), triple, runtimePlatform === 'android' ? [library, 'libc++_shared.so'] : runtimePlatform === 'windows' ? [library, 'weapp_container_sdk.dll'] : [library], expectedDistribution);
    const current = sha256(resolve(native, 'RUNTIME-LICENSE'));
    if (licenseHash && current !== licenseHash) throw new Error('Runtime slice license texts disagree');
    licenseHash = current;
    cpSync(resolve(native, 'RUNTIME-LICENSE'), resolve(stage, 'native/RUNTIME-LICENSE'));
    mkdirSync(resolve(stage, 'licenses'), { recursive: true });
    copyVerifiedTree(resolve(native, 'licenses'), resolve(stage, 'licenses', triple));
    readNotices(resolve(stage, 'native'), resolve(stage, 'licenses'), triple);
    receipts.push({ target: triple, artifact: result.receipt });
  }
  writeFileSync(resolve(stage, 'installation.json'), `${JSON.stringify({ schemaVersion: 1, sdkVersion: SDK_VERSION, ...expectedDistribution, abiVersion: 1, target, manifestSha256: sha256(manifestPath), artifacts: receipts }, null, 2)}\n`);
  if (existsSync(installationRoot)) renameSync(installationRoot, backup);
  try { renameSync(stage, installationRoot); }
  catch (error) { if (existsSync(backup)) renameSync(backup, installationRoot); throw error; }
  rmSync(backup, { recursive: true, force: true });
  console.log(JSON.stringify({ status: 'prepared', nativeRoot: resolve(installationRoot, 'native'), licensesRoot: resolve(installationRoot, 'licenses') }, null, 2));
} finally { rmSync(stage, { recursive: true, force: true }); }
