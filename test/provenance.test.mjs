import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, cp, writeFile, rm, symlink } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
const hash = value => createHash('sha256').update(value).digest('hex');
test('provenance accepts root worktree metadata but rejects nested metadata and links', async t => {
  const root = await mkdtemp(join(tmpdir(), 'weapp-sdk-origin-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  await mkdir(join(root, 'tools'));
  const entries = [];
  for (const name of ['check-origin.mjs', 'native-input.mjs']) {
    const destination = join(root, 'tools', name);
    await cp(new URL(`../tools/${name}`, import.meta.url), destination);
    const { readFile } = await import('node:fs/promises');
    entries.push({ destinationPath: `tools/${name}`, destinationSha256: hash(await readFile(destination)) });
  }
  await writeFile(join(root, 'header.h'), 'public ABI');
  entries.push({ destinationPath: 'header.h', destinationSha256: hash('public ABI') });
  await writeFile(join(root, 'ORIGIN.json'), JSON.stringify({ sdkVersion: 'test', sourceCommit: 'fixture', sources: entries, generated: [], contract: { headerPath: 'header.h', headerSha256: hash('public ABI') } }));
  const check = () => spawnSync(process.execPath, [join(root, 'tools/check-origin.mjs')], { encoding: 'utf8' });
  await writeFile(join(root, '.git'), 'gitdir: /external/git-metadata\n');
  const accepted = check(); assert.equal(accepted.status, 0, accepted.stderr);
  await mkdir(join(root, 'nested')); await writeFile(join(root, 'nested/.git'), 'unreviewed');
  const nested = check(); assert.notEqual(nested.status, 0); assert.match(nested.stderr, /Unreviewed source path: nested\/\.git/);
  await rm(join(root, 'nested'), { recursive: true });
  await rm(join(root, '.git')); await symlink('header.h', join(root, '.git'));
  const linked = check(); assert.notEqual(linked.status, 0); assert.match(linked.stderr, /Source symlink is forbidden/);
});
