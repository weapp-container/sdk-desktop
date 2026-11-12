import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync, symlinkSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { assetEntry, readRelease, releaseUrl, run, writeRelease, publishRelease, verifyPortableDistribution } from '../tools/public-release.mjs';
function fixture(t) {
  const root = mkdtempSync(join(tmpdir(), 'weapp-public-release-'));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  writeFileSync(join(root, '.gitignore'), 'artifacts/\n');
  writeFileSync(join(root, 'package.json'), JSON.stringify({ version: '0.1.0-preview.7.0' }));
  run('git', ['init', '-q'], root); run('git', ['add', '.'], root);
  run('git', ['-c', 'user.name=Publication Test', '-c', 'user.email=publication@example.invalid', 'commit', '-qm', 'Fixture'], root);
  const out = join(root, 'artifacts'); mkdirSync(out);
  writeFileSync(join(out, 'sdk.tar.gz'), 'inert archive fixture');
  return { root, out, manifest: join(out, 'release.json') };
}
test('the frozen release verifies offline and rejects changed archive bytes before publication', async t => {
  const f = fixture(t);
  const value = writeRelease(f.root, f.out, 'weapp-container/sdk-desktop', [assetEntry(f.out, 'sdk.tar.gz')]);
  assert.equal((await publishRelease(f.root, f.manifest)).published, false);
  assert.equal(releaseUrl(value, value.assets[0]), `https://github.com/weapp-container/sdk-desktop/releases/download/v0.1.0-preview.7.0/sdk.tar.gz`);
  assert.throws(() => readRelease(f.manifest, 'weapp-container/sdk-ios', value.sdkVersion), /identity/);
  assert.throws(() => readRelease(f.manifest, value.repository, '0.1.0-preview.6.0'), /identity/);
  writeFileSync(join(f.out, 'sdk.tar.gz'), 'changed bytes');
  await assert.rejects(() => publishRelease(f.root, f.manifest), /changed/);
});
test('publication refuses a dirty checkout and uncontained archive input', t => {
  const f = fixture(t);
  assert.throws(() => assetEntry(f.out, '../package.json'), /path/);
  symlinkSync(join(f.root, 'package.json'), join(f.out, 'linked.zip'));
  assert.throws(() => assetEntry(f.out, 'linked.zip'), /symlink/);
  writeFileSync(join(f.root, 'unreviewed.txt'), 'new source');
  assert.throws(() => writeRelease(f.root, f.out, 'weapp-container/sdk-desktop', [assetEntry(f.out, 'sdk.tar.gz')]), /clean/);
});
test('release metadata rejects duplicate assets and invalid hashes', t => {
  const f = fixture(t), entry = assetEntry(f.out, 'sdk.tar.gz');
  assert.throws(() => writeRelease(f.root, f.out, 'weapp-container/sdk-desktop', [entry, entry]), /duplicate/);
  assert.throws(() => writeRelease(f.root, f.out, 'weapp-container/sdk-desktop', [{ ...entry, sha256: '0' }]), /unverified/);
});
test('distribution inspection rejects compiler paths inside libraries before archiving', t => {
  const f = fixture(t), library = join(f.out, 'wrapper.lib');
  for (const path of ['D:\\a\\sdk-desktop\\build\\wrapper.obj', '/Users/builder/source/wrapper.cpp', '/home/runner/source/wrapper.cpp']) {
    writeFileSync(library, Buffer.concat([Buffer.from([0, 255]), Buffer.from(path)]));
    assert.throws(() => verifyPortableDistribution(f.out), /private\/local path/);
  }
  writeFileSync(library, 'weapp-desktop-build/wrapper.obj\0weapp-container/sdk-desktop/src/windows.cpp\0/pages/home/index');
  assert.doesNotThrow(() => verifyPortableDistribution(f.out));
});
