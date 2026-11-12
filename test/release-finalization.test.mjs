import test from 'node:test';
import assert from 'node:assert/strict';
import { cpSync, mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { assetEntry, cleanSource, readRelease, releaseUrl, run, writeRelease, publishRelease } from '../tools/public-release.mjs';
function fixture(t) {
  const root = mkdtempSync(join(tmpdir(), 'weapp-finalize-'));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  writeFileSync(join(root, '.gitignore'), 'artifacts/\n');
  writeFileSync(join(root, 'package.json'), JSON.stringify({ version: '0.1.0-preview.7.0' }));
  mkdirSync(join(root, 'tools'));
  for (const name of ['package-release.mjs', 'public-release.mjs']) cpSync(fileURLToPath(new URL(`../tools/${name}`, import.meta.url)), join(root, 'tools', name));
  run('git', ['init', '-q'], root);
  commit(root);
  return root;
}
function commit(root) {
  run('git', ['add', '.'], root);
  run('git', ['-c', 'user.name=Publication Test', '-c', 'user.email=publication@example.invalid', 'commit', '-qm', 'Reviewed source'], root);
}

test('Desktop finalization hashes its Podspec and retains all three original archives', async t => {
  const root = fixture(t), manifests = [], expected = [];
  for (const platform of ['macos-universal', 'linux-x86_64', 'windows-x86_64']) {
    const out = join(root, 'artifacts', platform); mkdirSync(out, { recursive: true });
    const name = `${platform}.tar.gz`; writeFileSync(join(out, name), `inert ${platform} archive`);
    const asset = assetEntry(out, name, { platform, component: 'desktop-sdk' }); expected.push(asset);
    writeRelease(root, out, 'weapp-container/sdk-desktop', [asset]); manifests.push(join(out, 'release.json'));
  }
  const out = join(root, 'artifacts/final');
  run(process.execPath, ['tools/package-release.mjs', '--out', out, '--merge', manifests.join(',')], root);
  const release = readRelease(join(out, 'release.json'), 'weapp-container/sdk-desktop', '0.1.0-preview.7.0', true);
  assert.deepEqual(release.assets.filter(a => a.component === 'desktop-sdk'), expected);
  assert.equal(release.assets.find(a => a.component === 'cocoapods-spec').fileName, 'WeappContainerDesktop.podspec');
  assert.match(readFileSync(join(out, 'WeappContainerDesktop.podspec'), 'utf8'), new RegExp(expected[0].sha256));
  assert.equal((await publishRelease(root, join(out, 'release.json'))).published, false);
  writeFileSync(join(out, 'WeappContainerDesktop.podspec'), 'changed spec');
  await assert.rejects(() => publishRelease(root, join(out, 'release.json')), /changed/);
});
