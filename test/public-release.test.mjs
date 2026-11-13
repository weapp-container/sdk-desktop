import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync, symlinkSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { assetEntry, readRelease, releaseUrl, run, writeRelease, publishRelease } from '../tools/public-release.mjs';
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
test('publication verifies an unpublished draft by ID when tag lookup cannot see it', async t => {
  const f = fixture(t);
  const value = writeRelease(f.root, f.out, 'weapp-container/sdk-ios', [assetEntry(f.out, 'sdk.tar.gz')]);
  const files = readdirSync(f.out).map(name => assetEntry(f.out, name));
  function github(overrides = {}) {
    const calls = [];
    const invoke = (_command, args) => {
      calls.push(args);
      assert.ok(!args.some(arg => arg.includes('/releases/tags/')), 'Unpublished drafts must not use tag lookup');
      if (args.includes('--slurp')) return JSON.stringify([[], overrides.exists ? [{ tag_name: value.tag, draft: true }] : []]);
      if (args[1].includes('/git/ref/')) throw Error('Not Found (HTTP 404)');
      if (args.includes('POST')) return JSON.stringify({ id: 123 });
      if (args.includes('PATCH') || args[0] === 'release') return '{}';
      assert.equal(args[1], 'repos/weapp-container/sdk-ios/releases/123');
      return JSON.stringify({ id: 123, tag_name: value.tag, draft: true, prerelease: true, target_commitish: value.sourceCommit,
        assets: files.map(file => ({ name: file.fileName, size: file.size, digest: `sha256:${file.sha256}`, state: 'uploaded', ...overrides.asset })), ...overrides.release });
    };
    return { calls, invoke };
  }
  const success = github();
  assert.equal((await publishRelease(f.root, f.manifest, true, success.invoke)).status, 'published');
  const patch = success.calls.find(args => args.includes('PATCH'));
  assert.equal(patch[1], 'repos/weapp-container/sdk-ios/releases/123');
  assert.ok(patch.includes('make_latest=false'));
  for (const overrides of [{ exists: true }, { release: { id: 124 } }, { release: { assets: [] } }, { asset: { digest: 'sha256:wrong' } }, { release: { target_commitish: 'b'.repeat(40) } }]) {
    const rejected = github(overrides);
    await assert.rejects(publishRelease(f.root, f.manifest, true, rejected.invoke));
    assert.ok(!rejected.calls.some(args => args.includes('PATCH')));
    if (overrides.exists) assert.ok(!rejected.calls.some(args => args.includes('POST')));
  }
});
