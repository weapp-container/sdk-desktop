import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, cp, writeFile, rm, symlink } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { writeSourceInventory } from '../tools/source-inventory.mjs';
const hash = value => createHash('sha256').update(value).digest('hex');
test('provenance accepts actual root worktree metadata but rejects nested metadata and links', async t => {
  const temp = await mkdtemp(join(tmpdir(), 'weapp-sdk-origin-'));
  t.after(() => rm(temp, { recursive: true, force: true }));
  const repo = join(temp, 'repository'), root = join(temp, 'worktree');
  await mkdir(join(repo, 'tools'), { recursive: true });
  function git(cwd, args) {
    const result = spawnSync('git', args, { cwd, encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr);
  }
  for (const name of ['check-origin.mjs', 'native-input.mjs', 'source-inventory.mjs']) await cp(new URL(`../tools/${name}`, import.meta.url), join(repo, 'tools', name));
  await writeFile(join(repo, 'header.h'), 'public ABI');
  await writeFile(join(repo, 'ORIGIN.json'), JSON.stringify({ sdkVersion: 'test', sourceCommit: 'fixture', sources: [], generated: [], contract: { headerPath: 'header.h', headerSha256: hash('public ABI') } }));
  git(repo, ['init', '--quiet']); git(repo, ['add', '.']);
  git(repo, ['-c', 'user.name=SDK Provenance Test', '-c', 'user.email=sdk-provenance@example.invalid', 'commit', '--quiet', '-m', 'Historical fixture']);
  git(repo, ['worktree', 'add', '--quiet', '--detach', root]);
  writeSourceInventory(root, { sdkVersion: '0.1.0-preview.5', reason: 'Review independent fixture' });
  git(root, ['add', 'SOURCE-INVENTORY.json']);
  const check = () => spawnSync(process.execPath, [join(root, 'tools/check-origin.mjs')], { encoding: 'utf8' });
  const accepted = check(); assert.equal(accepted.status, 0, accepted.stderr);
  await mkdir(join(root, 'nested')); await writeFile(join(root, 'nested/.git'), 'unreviewed');
  const nested = check(); assert.notEqual(nested.status, 0); assert.match(nested.stderr, /unreviewed or linked Git metadata: nested\/\.git/);
  await rm(join(root, 'nested'), { recursive: true });
  await rm(join(root, '.git')); await symlink('header.h', join(root, '.git'));
  const linked = check(); assert.notEqual(linked.status, 0);
});
