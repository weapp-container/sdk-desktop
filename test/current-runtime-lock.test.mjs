import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { SDK_VERSION, selectArtifact } from '../tools/runtime-artifacts.mjs';
import { SDK_VERSION as CANDIDATE_VERSION } from '../tools/native-input.mjs';

test('C header version agrees with package and runtime identities', () => {
  const header = readFileSync(new URL('../include/weapp/desktop.h', import.meta.url), 'utf8');
  const pkg = JSON.parse(readFileSync(new URL('../package.json', import.meta.url)));
  assert.equal(header.match(/^#define WEAPP_DESKTOP_VERSION "([^"]+)"$/m)?.[1], pkg.version);
  assert.equal(pkg.version, CANDIDATE_VERSION);
});

test('current public pin supplies every native target and wrapper without historical fallback', () => {
  const pin = JSON.parse(readFileSync(new URL(`../runtime-artifacts.${SDK_VERSION}.lock.json`, import.meta.url)));
  assert.deepEqual(pin.source, { repository: 'weapp-container/weapp-container', releaseTag: `runtime-v${SDK_VERSION}` });
  assert.equal(pin.runtimeVersion, SDK_VERSION);
  assert.equal(pin.abiVersion, 1);
  const required = [
    ['runtime','android','arm64-v8a'], ['runtime','android','x86_64'],
    ['runtime','ios','arm64-device'], ['runtime','ios','arm64-simulator'], ['runtime','ios','x86_64-simulator'],
    ['runtime','macos','arm64'], ['runtime','macos','x86_64'],
    ['runtime','linux','x86_64'], ['runtime','windows','x86_64'],
    ['android-sdk','android','universal'], ['ios-sdk','ios','universal'],
    ['desktop-sdk','macos','universal'], ['desktop-sdk','linux','x86_64'], ['desktop-sdk','windows','x86_64'],
  ];
  assert.equal(pin.artifacts.length, required.length);
  for (const [component, platform, arch] of required) {
    const selected = selectArtifact(pin, { version: SDK_VERSION, abiVersion: 1, component, platform, arch });
    assert.ok(selected.size > 0); assert.match(selected.sha256, /^[a-f0-9]{64}$/);
  }
  assert.throws(() => selectArtifact(pin, { version: '0.1.0-preview.5.3', abiVersion: 1, component: 'runtime', platform: 'android', arch: 'arm64-v8a' }), { code: 'ERR_RUNTIME_MANIFEST' });
});
