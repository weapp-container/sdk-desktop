import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtemp, readFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { downloadRuntimeArtifact, selectArtifact, RUNTIME_REPOSITORY,
  LEGACY_RUNTIME_REPOSITORY, LEGACY_RUNTIME_ARCHIVE_REPOSITORY, LEGACY_RUNTIME_VERSIONS } from '../tools/runtime-artifacts.mjs';

const bytes = Buffer.from('isolated migration fixture, not a runtime');
const sha256 = createHash('sha256').update(bytes).digest('hex');
const target = { platform: 'linux', arch: 'x86_64', component: 'runtime', abiVersion: 1 };
function manifest(repository, version) {
  return { schemaVersion: 1, runtimeVersion: version, abiVersion: 1,
    source: { repository, releaseTag: `runtime-v${version}` },
    artifacts: [{ ...target, fileName: 'runtime-fixture.tar.gz', size: bytes.length, sha256 }].map(({ abiVersion, ...entry }) => entry) };
}
async function context(t, repository = RUNTIME_REPOSITORY, version = '0.1.0-preview.6.0') {
  const directory = await mkdtemp(join(tmpdir(), 'wc-migration-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const pin = manifest(repository, version), manifestPath = join(directory, 'manifest.json');
  await writeFile(manifestPath, JSON.stringify(pin));
  return { pin, options: { ...target, version, manifestPath, outDirectory: join(directory, 'cache'), artifactDirectory: undefined } };
}

test('core releases and frozen historical versions are explicit with no future legacy releases', () => {
  assert.equal(RUNTIME_REPOSITORY, 'weapp-container/weapp-container');
  assert.equal(LEGACY_RUNTIME_REPOSITORY, 'weapp-container/runtime-artifacts');
  for (const version of LEGACY_RUNTIME_VERSIONS) {
    assert.equal(selectArtifact(manifest(LEGACY_RUNTIME_REPOSITORY, version), { ...target, version }).repository, LEGACY_RUNTIME_REPOSITORY);
  }
  for (const version of ['0.1.0-preview.5.7', '0.1.0-preview.6.1', '1.0.0']) {
    assert.throws(() => selectArtifact(manifest(LEGACY_RUNTIME_REPOSITORY, version), { ...target, version }), { code: 'ERR_RUNTIME_MANIFEST' });
    assert.equal(selectArtifact(manifest(RUNTIME_REPOSITORY, version), { ...target, version }).repository, RUNTIME_REPOSITORY);
  }
});

test('both repositories use exact metadata and asset endpoints and retain the actual receipt source', async t => {
  for (const repository of [RUNTIME_REPOSITORY, LEGACY_RUNTIME_REPOSITORY]) {
    const c = await context(t, repository), calls = [];
    const result = await downloadRuntimeArtifact({ ...c.options, token: 'test-token', fetchImpl: async (url, options) => {
      calls.push(String(url)); assert.equal(options.headers.Authorization, 'Bearer test-token');
      return calls.length === 1 ? Response.json({ tag_name: c.pin.source.releaseTag, draft: false,
        assets: [{ id: 17, state: 'uploaded', name: c.pin.artifacts[0].fileName, size: bytes.length, digest: `sha256:${sha256}` }] }) : new Response(bytes);
    } });
    const storageRepository = repository === LEGACY_RUNTIME_REPOSITORY ? LEGACY_RUNTIME_ARCHIVE_REPOSITORY : repository;
    assert.deepEqual(calls, [`https://api.github.com/repos/${storageRepository}/releases/tags/${c.pin.source.releaseTag}`,
      `https://api.github.com/repos/${storageRepository}/releases/assets/17`]);
    assert.equal(result.receipt.repository, repository);
    assert.equal(result.receipt.resolvedRepository, repository === LEGACY_RUNTIME_REPOSITORY ? storageRepository : undefined);
    assert.deepEqual(await readFile(result.path), bytes);
  }
});

test('core supports anonymous access and omits the Authorization header', async t => {
  const c = await context(t); let calls = 0;
  await downloadRuntimeArtifact({ ...c.options, token: '', fetchImpl: async (_url, options) => {
    assert.equal(options.headers.Authorization, undefined);
    return ++calls === 1 ? Response.json({ tag_name: c.pin.source.releaseTag, draft: false,
      assets: [{ id: 17, state: 'uploaded', name: c.pin.artifacts[0].fileName, size: bytes.length }] }) : new Response(bytes);
  } });
  assert.equal(calls, 2);
});

test('repository failures do not fall back and redirects cannot cross between the two repositories', async t => {
  for (const repository of [RUNTIME_REPOSITORY, LEGACY_RUNTIME_REPOSITORY]) {
    for (const redirect of [false, true]) {
      const c = await context(t, repository); let calls = 0;
      const other = repository === RUNTIME_REPOSITORY ? LEGACY_RUNTIME_REPOSITORY : RUNTIME_REPOSITORY;
      await assert.rejects(downloadRuntimeArtifact({ ...c.options, token: 'secret', fetchImpl: async () => {
        calls++;
        return redirect ? new Response(null, { status: 302, headers: { location: `https://api.github.com/repos/${other}/releases/1` } })
          : new Response('private response must not appear', { status: 404 });
      } }), { code: redirect ? 'ERR_RUNTIME_REDIRECT' : 'ERR_RUNTIME_AUTH' });
      assert.equal(calls, 1);
    }
  }
});

test('malformed tokens fail before networking for either source', async t => {
  for (const repository of [RUNTIME_REPOSITORY, LEGACY_RUNTIME_REPOSITORY]) {
    const c = await context(t, repository);
    for (const token of [' ', 'x\ny', 123, null]) {
      await assert.rejects(downloadRuntimeArtifact({ ...c.options, token, fetchImpl() { assert.fail('Invalid credentials cannot request'); } }), { code: 'ERR_RUNTIME_CREDENTIALS' });
    }
  }
});
