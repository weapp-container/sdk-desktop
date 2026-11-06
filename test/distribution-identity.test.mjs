import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { distributionIdentity, distributionOptions, assertDistributionIdentity } from '../tools/distribution-identity.mjs';
import { selectArtifact, downloadRuntimeArtifact, SDK_VERSION } from '../tools/runtime-artifacts.mjs';
const identity = { distributionProfile: 'restricted-showcase', policySha256: 'a'.repeat(64) };
const expected = { version: SDK_VERSION, abiVersion: 1, component: 'runtime', platform: 'android', arch: 'arm64-v8a' };
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
function manifest(artifact) { return { schemaVersion: 1, runtimeVersion: SDK_VERSION, abiVersion: 1, ...identity,
  source: { repository: 'weapp-container/runtime-artifacts', releaseTag: `runtime-v${SDK_VERSION}` }, artifacts: [artifact] }; }
test('distribution defaults to full and restricted policy selection is explicit', () => {
  assert.deepEqual(distributionIdentity(), { distributionProfile: 'full', policySha256: null });
  assert.deepEqual(distributionOptions(['--distribution-profile', identity.distributionProfile, '--showcase-policy-sha256', identity.policySha256]), identity);
  assert.throws(() => assertDistributionIdentity(identity), /mismatch/);
  for (const args of [['--distribution-profile','restricted-showcase'], ['--showcase-policy-sha256', identity.policySha256],
    ['--distribution-profile','other'], ['--distribution-profile','full','--distribution-profile','full'],
    ['--distribution-profile','restricted-showcase','--showcase-policy-sha256','x']]) assert.throws(() => distributionOptions(args));
});
test('a reviewed lock cannot be downgraded, rebound or silently selected as full', () => {
  const value = manifest({ component: 'runtime', platform: 'android', arch: 'arm64-v8a', fileName: 'runtime.tar.gz', size: 1, sha256: 'b'.repeat(64) });
  assert.throws(() => selectArtifact(value, expected), { code: 'ERR_RUNTIME_MANIFEST' });
  assert.throws(() => selectArtifact(value, { ...expected, ...identity, policySha256: 'b'.repeat(64) }), { code: 'ERR_RUNTIME_MANIFEST' });
  assert.equal(selectArtifact(value, { ...expected, ...identity }).policySha256, identity.policySha256);
  assert.throws(() => selectArtifact({ ...value, distributionProfile: 'full', policySha256: null }, { ...expected, ...identity }), { code: 'ERR_RUNTIME_MANIFEST' });
});
test('offline installation preserves the selected policy in exact archive receipts', async t => {
  const root = mkdtempSync(join(tmpdir(), 'weapp-restricted-sdk-')); t.after(() => rmSync(root, { recursive: true, force: true }));
  const archives = join(root, 'archives'); mkdirSync(archives); const archive = join(archives, 'runtime.tar.gz');
  const result = spawnSync('python3', ['-c', 'import io,tarfile,sys; t=tarfile.open(sys.argv[1],"w:gz"); b=b"pinned runtime"; i=tarfile.TarInfo("runtime.txt"); i.size=len(b); i.mode=420; t.addfile(i,io.BytesIO(b)); t.close()', archive], { encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr); const bytes = readFileSync(archive), path = join(root, 'lock.json');
  writeFileSync(path, JSON.stringify(manifest({ component: 'runtime', platform: 'android', arch: 'arm64-v8a', fileName: 'runtime.tar.gz', size: bytes.length, sha256: sha(bytes) })));
  const options = { manifestPath: path, ...expected, ...identity, artifactDirectory: archives, outDirectory: join(root, 'downloads'), installDirectory: join(root, 'install') };
  const installed = await downloadRuntimeArtifact(options);
  assertDistributionIdentity(installed.receipt, identity);
  assertDistributionIdentity(JSON.parse(readFileSync(join(root, 'install/.runtime-install.json'))).identity, identity);
  await assert.rejects(() => downloadRuntimeArtifact({ ...options, policySha256: 'c'.repeat(64) }), { code: 'ERR_RUNTIME_MANIFEST' });
});

import { readNative, nativeIdentity, SDK_VERSION as NATIVE_RUNTIME_VERSION } from '../tools/native-input.mjs';
import { fileURLToPath } from 'node:url';
test('native packaging verifies actual restricted marker bytes and preserves policy identity', t => {
  const root = mkdtempSync(join(tmpdir(), 'weapp-restricted-native-')); t.after(() => rmSync(root, { recursive: true, force: true }));
  const target = 'aarch64-linux-android', directory = join(root, target); mkdirSync(directory);
  const header = readFileSync(fileURLToPath(new URL('../include/weapp_container_sdk.h', import.meta.url)));
  writeFileSync(join(directory, 'weapp_container_sdk.h'), header);
  const library = 'libweapp_container_sdk.so';
  const bytes = Buffer.from(`fixture\0weapp-container-distribution:restricted-showcase:${identity.policySha256}\0`);
  writeFileSync(join(directory, library), bytes);
  const receipt = { schemaVersion: 1, sdkVersion: NATIVE_RUNTIME_VERSION, abiVersion: 1, target, status: 'built',
    profile: 'release', viewDiagnostics: 'off', features: ['restricted-showcase'], ...identity,
    artifacts: [{ file: library, bytes: bytes.length, sha256: sha(bytes) }, { file: 'weapp_container_sdk.h', bytes: header.length, sha256: sha(header) }] };
  const save = () => writeFileSync(join(directory, 'build.json'), JSON.stringify(receipt)); save();
  assert.throws(() => readNative(root, target, [library]), /mismatch/);
  const input = readNative(root, target, [library], identity); assertDistributionIdentity(nativeIdentity(input.receipt), identity);
  Object.assign(receipt, { distributionProfile: 'full', policySha256: null, features: [] }); save();
  assert.throws(() => readNative(root, target, [library]), /exact distribution/);
  Object.assign(receipt, identity, { features: ['restricted-showcase'] });
  const rebound = Buffer.from('wrong binary identity'); writeFileSync(join(directory, library), rebound);
  receipt.artifacts[0] = { file: library, bytes: rebound.length, sha256: sha(rebound) }; save();
  assert.throws(() => readNative(root, target, [library], identity), /exact distribution/);
});
