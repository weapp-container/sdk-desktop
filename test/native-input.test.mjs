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
  const identity = nativeIdentity({ schemaVersion: 1, sdkVersion: SDK_VERSION, abiVersion: 1, target: 'fixture-target', status: 'built',
    toolchain: { androidNdk: '/private/producer/ndk' }, output: '/private/producer/build', commands: ['/private/producer/cargo'],
    artifacts: [{ file: 'libfixture.a', sha256: '0'.repeat(64), bytes: 10, producerPath: '/private/producer/libfixture.a' }],
  });
  assert.equal(JSON.stringify(identity).includes('/private/producer'), false);
  assert.equal(identity.abiVersion, 1);
  assert.deepEqual(identity.artifacts, [{ file: 'libfixture.a', bytes: 10, sha256: '0'.repeat(64) }]);
});
