import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { cpSync, mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { copyPack, HEADER_SHA256, inside, nativeIdentity, readNative, readNotices, requireNativeRoot, SDK_VERSION, sha256 } from '../tools/native-input.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const hash = value => createHash('sha256').update(value).digest('hex');
function fixture(t) {
  const directory = mkdtempSync(resolve(tmpdir(), 'weapp-sdk-input-'));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  const native = resolve(directory, 'native'), target = 'fixture-target', input = resolve(native, target);
  mkdirSync(input, { recursive: true });
  writeFileSync(resolve(input, 'libfixture.a'), 'test bytes: not an executable runtime');
  cpSync(resolve(root, 'include/weapp_container_sdk.h'), resolve(input, 'weapp_container_sdk.h'));
  const receipt = { schemaVersion: 1, sdkVersion: SDK_VERSION, abiVersion: 1, target, status: 'built', artifacts: [{ file: 'libfixture.a', sha256: sha256(resolve(input, 'libfixture.a')) }] };
  const save = () => writeFileSync(resolve(input, 'build.json'), JSON.stringify(receipt)); save();
  return { directory, native, target, input, receipt, save };
}

test('a matching receipt accepts only the reviewed ABI header and hashed input', t => {
  const f = fixture(t);
  assert.equal(sha256(resolve(root, 'include/weapp_container_sdk.h')), HEADER_SHA256);
  assert.equal(readNative(f.native, f.target, ['libfixture.a']).receipt.sdkVersion, SDK_VERSION);
});
test('binary tampering cannot reuse an otherwise successful receipt', t => {
  const f = fixture(t); writeFileSync(resolve(f.input, 'libfixture.a'), 'changed bytes');
  assert.throws(() => readNative(f.native, f.target, ['libfixture.a']), /hash mismatch/);
});
test('older versions, incompatible ABIs and mislabeled targets are rejected before packaging', t => {
  const f = fixture(t); f.receipt.sdkVersion = '0.1.0-preview.1'; f.save();
  assert.throws(() => readNative(f.native, f.target, ['libfixture.a']), /runtime receipt/);
  f.receipt.sdkVersion = SDK_VERSION; f.receipt.abiVersion = 2; f.save();
  assert.throws(() => readNative(f.native, f.target, ['libfixture.a']), /runtime receipt/);
  f.receipt.abiVersion = 1; f.receipt.target = 'another-target'; f.save();
  assert.throws(() => readNative(f.native, f.target, ['libfixture.a']), /runtime receipt/);
});
test('replacing the contract header or omitting a required library fails', t => {
  const f = fixture(t);
  assert.throws(() => readNative(f.native, f.target, ['missing.a']), /Missing hashed/);
  writeFileSync(resolve(f.input, 'weapp_container_sdk.h'), '// different ABI');
  assert.throws(() => readNative(f.native, f.target, ['libfixture.a']), /reviewed SDK contract/);
});
test('duplicate, traversing and symlink artifact names cannot escape the input', t => {
  const f = fixture(t);
  assert.throws(() => inside(f.native, '../outside'), /escapes/);
  f.receipt.artifacts.push(f.receipt.artifacts[0]); f.save();
  assert.throws(() => readNative(f.native, f.target, ['libfixture.a']), /duplicate/);
  if (process.platform !== 'win32') {
    symlinkSync(resolve(f.input, 'libfixture.a'), resolve(f.input, 'link.a'));
    assert.throws(() => inside(f.input, 'link.a'), /symlink/);
  }
});
test('native dependency notices require actual matching bytes and a separate runtime license', t => {
  const f = fixture(t), licenses = resolve(f.directory, 'licenses'), folder = resolve(licenses, f.target);
  mkdirSync(folder, { recursive: true });
  writeFileSync(resolve(folder, 'LICENSE'), 'third party notice');
  writeFileSync(resolve(folder, 'licenses.json'), JSON.stringify({ schemaVersion: 1, target: f.target, packages: [{ name: 'fixture', files: [{ path: 'LICENSE', sha256: hash('third party notice') }] }] }));
  writeFileSync(resolve(f.native, 'RUNTIME-LICENSE'), 'Private binary terms for this test fixture');
  assert.equal(readNotices(f.native, licenses, f.target).notices, folder);
  writeFileSync(resolve(folder, 'LICENSE'), 'tampered');
  assert.throws(() => readNotices(f.native, licenses, f.target), /notice integrity/);
});
test('consumer packs verify manifest and blob bytes without a parent source checkout', t => {
  const f = fixture(t), source = resolve(f.directory, 'pack'), destination = resolve(f.directory, 'copied-pack');
  mkdirSync(resolve(source, 'blobs/sha256'), { recursive: true });
  const blob = 'fixture source', blobHash = hash(blob);
  writeFileSync(resolve(source, 'blobs/sha256', blobHash), blob);
  const manifest = { integrity: { algorithm: 'sha256', root: '' }, schemaVersion: 1 };
  manifest.integrity.root = hash(JSON.stringify(manifest));
  writeFileSync(resolve(source, 'manifest.json'), JSON.stringify(manifest));
  assert.equal(copyPack(source, destination), manifest.integrity.root);
  assert.equal(readFileSync(resolve(destination, 'blobs/sha256', blobHash), 'utf8'), blob);
  assert.throws(() => copyPack(source, resolve(source, 'nested')), /must not be inside/);
  writeFileSync(resolve(source, 'blobs/sha256', blobHash), 'changed');
  assert.throws(() => copyPack(source, resolve(f.directory, 'invalid')), /blob integrity/);
});
test('an absent runtime input never invokes a compiler or searches a parent repository', () => {
  const prior = process.env.WEAPP_CONTAINER_RUNTIME_NATIVE_ROOT;
  delete process.env.WEAPP_CONTAINER_RUNTIME_NATIVE_ROOT;
  try { assert.throws(() => requireNativeRoot([]), /--native-root is required/); }
  finally { if (prior !== undefined) process.env.WEAPP_CONTAINER_RUNTIME_NATIVE_ROOT = prior; }
});
test('packaged native identities exclude producer checkout, toolchain and command paths', () => {
  const nativePostProcessing = { schemaVersion: 1, compileExecuted: false,
    originalReceipt: { file: 'original-build.json', sha256: '1'.repeat(64), bytes: 100 } };
  const identity = nativeIdentity({ schemaVersion: 1, sdkVersion: SDK_VERSION, abiVersion: 1, target: 'fixture-target', status: 'built',
    toolchain: { androidNdk: '/private/producer/ndk' }, output: '/private/producer/build', commands: ['/private/producer/cargo'],
    artifacts: [{ file: 'libfixture.a', sha256: '0'.repeat(64), bytes: 10, producerPath: '/private/producer/libfixture.a' }],
    nativePostProcessing, originalNativeProducerReceipt: '{"output":"/private/producer/build"}',
  });
  assert.equal(JSON.stringify(identity).includes('/private/producer'), false);
  assert.equal(identity.abiVersion, 1);
  assert.deepEqual(identity.nativePostProcessing, nativePostProcessing);
  assert.deepEqual(identity.artifacts, [{ file: 'libfixture.a', bytes: 10, sha256: '0'.repeat(64) }]);
});

function distributionFixture(t, platform = 'ios') {
  const f = fixture(t);
  f.target = platform === 'android' ? 'aarch64-linux-android' : platform === 'windows' ? 'x86_64-pc-windows-msvc' : 'aarch64-apple-ios-sim';
  f.input = resolve(f.native, f.target); mkdirSync(f.input, { recursive: true });
  const omitted = platform === 'android' ? 'libweapp_container_sdk.a' : platform === 'windows' ? 'weapp_container_sdk.lib' : 'libweapp_container_sdk.dylib';
  const library = platform === 'android' ? 'libweapp_container_sdk.so' : platform === 'windows' ? 'weapp_container_sdk.dll.lib' : 'libweapp_container_sdk.a';
  const files = [library, omitted, 'weapp_container_sdk.h', ...(platform === 'android' ? ['libc++_shared.so', 'platform-notices/NOTICE'] : platform === 'windows' ? ['weapp_container_sdk.dll'] : [])];
  for (const name of files) {
    mkdirSync(resolve(f.input, name, '..'), { recursive: true });
    if (name === 'weapp_container_sdk.h') cpSync(resolve(root, 'include', name), resolve(f.input, name));
    else writeFileSync(resolve(f.input, name), `test-only ${name}`);
  }
  const metadata = file => ({ file, bytes: readFileSync(resolve(f.input, file)).length, sha256: sha256(resolve(f.input, file)) });
  f.receipt = { schemaVersion: 1, sdkVersion: SDK_VERSION, abiVersion: 1, target: f.target, status: 'built', sourceCommit: 'a'.repeat(40), sourceDirty: false, artifacts: files.map(metadata) };
  f.save = () => writeFileSync(resolve(f.input, 'build.json'), JSON.stringify(f.receipt)); f.save();
  f.selection = { schemaVersion: 1, sdkVersion: SDK_VERSION, abiVersion: 1, target: f.target, sourceCommit: f.receipt.sourceCommit, sourceDirty: false,
    producerReceipt: { file: 'build.json', sha256: sha256(resolve(f.input, 'build.json')) },
    selectedArtifacts: f.receipt.artifacts.filter(item => item.file !== omitted), omittedArtifacts: f.receipt.artifacts.filter(item => item.file === omitted),
  };
  f.saveSelection = () => writeFileSync(resolve(f.input, 'runtime-distribution.json'), JSON.stringify(f.selection)); f.saveSelection();
  rmSync(resolve(f.input, omitted));
  return Object.assign(f, { library, omitted });
}

for (const platform of ['ios', 'android', 'windows']) test(`a ${platform} exact distribution validates selected bytes while preserving its full producer receipt`, t => {
  const f = distributionFixture(t, platform), original = readFileSync(resolve(f.input, 'build.json'));
  const input = readNative(f.native, f.target, [f.library]);
  assert.equal(input.distribution.omittedArtifacts[0].file, f.omitted);
  assert.deepEqual(readFileSync(resolve(f.input, 'build.json')), original);
  rmSync(resolve(f.input, 'runtime-distribution.json'));
  assert.throws(() => readNative(f.native, f.target, [f.library]), /ENOENT/);
});
test('distribution source, version, ABI and target must match the original receipt', t => {
  const f = distributionFixture(t);
  for (const [field, value] of [['sourceCommit', 'b'.repeat(40)], ['sourceDirty', true], ['sdkVersion', '0.1.0-preview.1'], ['abiVersion', 2], ['target', 'aarch64-apple-ios']]) {
    const original = f.selection[field]; f.selection[field] = value; f.saveSelection();
    assert.throws(() => readNative(f.native, f.target, [f.library]), /identity mismatch/);
    f.selection[field] = original;
  }
});
test('a distribution binds the exact original producer receipt bytes', t => {
  const f = distributionFixture(t); writeFileSync(resolve(f.input, 'build.json'), JSON.stringify(f.receipt, null, 2));
  assert.throws(() => readNative(f.native, f.target, [f.library]), /producer receipt mismatch/);
});
test('distribution partitions reject duplicate, missing and foreign inventory entries', t => {
  const f = distributionFixture(t), original = structuredClone(f.selection);
  f.selection.selectedArtifacts.push(f.selection.selectedArtifacts[0]); f.saveSelection();
  assert.throws(() => readNative(f.native, f.target, [f.library]), /duplicate/);
  f.selection = structuredClone(original); f.selection.omittedArtifacts = []; f.saveSelection();
  assert.throws(() => readNative(f.native, f.target, [f.library]), /complete producer inventory/);
  f.selection = structuredClone(original); f.selection.omittedArtifacts[0].file = 'another-library.a'; f.saveSelection();
  assert.throws(() => readNative(f.native, f.target, [f.library]), /differs from producer/);
});
test('distribution metadata and selected content cannot be altered independently', t => {
  const f = distributionFixture(t), original = structuredClone(f.selection);
  f.selection.selectedArtifacts[0].bytes++; f.saveSelection();
  assert.throws(() => readNative(f.native, f.target, [f.library]), /differs from producer/);
  f.selection = structuredClone(original); f.selection.selectedArtifacts[0].sha256 = '0'.repeat(64); f.saveSelection();
  assert.throws(() => readNative(f.native, f.target, [f.library]), /differs from producer/);
  f.selection = structuredClone(original); f.saveSelection(); writeFileSync(resolve(f.input, f.library), 'corrupted');
  assert.throws(() => readNative(f.native, f.target, [f.library]), /hash mismatch/);
});
test('required libraries, ABI headers and platform notices cannot be omitted', t => {
  const f = distributionFixture(t, 'android'), original = structuredClone(f.selection);
  for (const file of [f.library, 'weapp_container_sdk.h', 'platform-notices/NOTICE']) {
    f.selection = structuredClone(original);
    const index = f.selection.selectedArtifacts.findIndex(item => item.file === file);
    f.selection.omittedArtifacts.push(f.selection.selectedArtifacts.splice(index, 1)[0]); f.saveSelection();
    assert.throws(() => readNative(f.native, f.target, [f.library]), /cannot omit required platform artifact/);
  }
  f.selection = structuredClone(original); f.saveSelection();
  assert.throws(() => readNative(f.native, f.target, [f.omitted]), /Missing hashed native library/);
});
test('distribution selection cannot relax native path and symlink rules', t => {
  const f = distributionFixture(t);
  f.receipt.artifacts[1].file = '../libweapp_container_sdk.dylib'; f.save();
  f.selection.omittedArtifacts[0].file = '../libweapp_container_sdk.dylib'; f.selection.producerReceipt.sha256 = sha256(resolve(f.input, 'build.json')); f.saveSelection();
  assert.throws(() => readNative(f.native, f.target, [f.library]), /Invalid or duplicate native artifact/);
  if (process.platform !== 'win32') {
    const g = distributionFixture(t); rmSync(resolve(g.input, g.library)); symlinkSync(resolve(root, 'LICENSE'), resolve(g.input, g.library));
    assert.throws(() => readNative(g.native, g.target, [g.library]), /symlink/);
  }
});


test('modern View schema 3 packs retain full canonical integrity verification', t => {
  const f = fixture(t), source = resolve(f.directory, 'modern-pack');
  mkdirSync(resolve(source, 'blobs/sha256'), { recursive: true });
  const bytes = Buffer.from('modern module fixture'), digest = hash(bytes);
  writeFileSync(resolve(source, 'blobs/sha256', digest), bytes);
  const manifest = { schemaVersion: 3, modules: [{ code: `sha256:${digest}` }], integrity: { algorithm: 'sha256', root: '' } };
  const canonical = value => Array.isArray(value) ? `[${value.map(canonical).join(',')}]` : value && typeof value === 'object' ? `{${Object.keys(value).sort().map(key => `${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}` : JSON.stringify(value);
  manifest.integrity.root = hash(canonical(manifest));
  writeFileSync(resolve(source, 'manifest.json'), JSON.stringify(manifest));
  assert.equal(copyPack(source, resolve(f.directory, 'modern-copy')), manifest.integrity.root);
  writeFileSync(resolve(source, 'blobs/sha256', digest), 'changed');
  assert.throws(() => copyPack(source, resolve(f.directory, 'modern-tampered')), /blob integrity/);
  writeFileSync(resolve(source, 'blobs/sha256', digest), bytes);
  manifest.schemaVersion = 2; manifest.integrity.root = ''; manifest.integrity.root = hash(canonical(manifest));
  writeFileSync(resolve(source, 'manifest.json'), JSON.stringify(manifest));
  assert.throws(() => copyPack(source, resolve(f.directory, 'unsupported')), /Invalid portable manifest/);
});
