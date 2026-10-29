import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { chmodSync, mkdtempSync, mkdirSync, readFileSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { INVENTORY_FILE, verifySourceInventory, writeSourceInventory } from '../tools/source-inventory.mjs';

function git(root, args) {
  const value = spawnSync('git', args, { cwd: root, encoding: 'utf8' });
  assert.equal(value.status, 0, value.stderr);
}
function fixture(run) {
  const root = mkdtempSync(join(tmpdir(), 'sdk-reviewed-source-'));
  try {
    git(root, ['init', '--quiet']);
    writeFileSync(join(root, 'ORIGIN.json'), '{"historical":"extraction"}\n');
    writeFileSync(join(root, 'wrapper.mjs'), 'export const value = 1;\n');
    git(root, ['add', '.']);
    git(root, ['-c', 'user.name=SDK Inventory Test', '-c', 'user.email=sdk-inventory@example.invalid', 'commit', '--quiet', '-m', 'Historical fixture']);
    writeSourceInventory(root, { sdkVersion: '0.1.0-preview.5.3', reason: 'Review fixture sources' });
    git(root, ['add', INVENTORY_FILE]);
    return run(root);
  } finally { rmSync(root, { recursive: true, force: true }); }
}
test('a reviewed current inventory preserves the historical extraction receipt', () => fixture(root => {
  const historical = readFileSync(join(root, 'ORIGIN.json'));
  const result = verifySourceInventory(root);
  assert.deepEqual(result.files, ['ORIGIN.json', INVENTORY_FILE, 'wrapper.mjs'].sort());
  assert.equal(result.sdkVersion, '0.1.0-preview.5.3');
  assert.match(result.inventorySha256, /^[a-f0-9]{64}$/);
  assert.deepEqual(readFileSync(join(root, 'ORIGIN.json')), historical);
}));
test('modified bytes and executable mode require a new explicit review', () => fixture(root => {
  writeFileSync(join(root, 'wrapper.mjs'), 'export const value = 2;\n');
  assert.throws(() => verifySourceInventory(root), /hash, size, mode/);
  writeFileSync(join(root, 'wrapper.mjs'), 'export const value = 1;\n');
  chmodSync(join(root, 'wrapper.mjs'), 0o755);
  assert.throws(() => verifySourceInventory(root), /hash, size, mode/);
}));
test('new source is rejected until staged, reviewed and inventoried', () => fixture(root => {
  writeFileSync(join(root, 'new.mjs'), 'export const added = true;\n');
  assert.throws(() => verifySourceInventory(root), /untracked source/);
  assert.throws(() => writeSourceInventory(root, { sdkVersion: '0.1.0-preview.5.3', reason: 'Review change' }), /untracked source/);
  git(root, ['add', 'new.mjs']);
  assert.throws(() => verifySourceInventory(root), /reviewed allowlist/);
  writeSourceInventory(root, { sdkVersion: '0.1.0-preview.5.3', reason: 'Review new wrapper' });
  assert.ok(verifySourceInventory(root).files.includes('new.mjs'));
}));
test('refresh cannot silently rewrite historical ORIGIN', () => fixture(root => {
  writeFileSync(join(root, 'ORIGIN.json'), '{"historical":"rewritten"}\n');
  assert.throws(() => writeSourceInventory(root, { sdkVersion: '0.1.0-preview.5.3', reason: 'Review change' }), /historical receipt/);
}));
test('links, binary bytes and core source cannot enter a reviewed inventory', () => fixture(root => {
  symlinkSync('wrapper.mjs', join(root, 'alias.mjs'));
  git(root, ['add', 'alias.mjs']);
  assert.throws(() => writeSourceInventory(root, { sdkVersion: '0.1.0-preview.5.3', reason: 'Review change' }), /symbolic link/);
  git(root, ['rm', '--cached', 'alias.mjs']); rmSync(join(root, 'alias.mjs'));
  writeFileSync(join(root, 'binary.txt'), Buffer.from([0, 1, 2])); git(root, ['add', 'binary.txt']);
  assert.throws(() => writeSourceInventory(root, { sdkVersion: '0.1.0-preview.5.3', reason: 'Review change' }), /binary source bytes/);
  git(root, ['rm', '--cached', 'binary.txt']); rmSync(join(root, 'binary.txt'));
  mkdirSync(join(root, 'crates')); writeFileSync(join(root, 'crates', 'lib.rs'), 'private runtime'); git(root, ['add', 'crates']);
  assert.throws(() => writeSourceInventory(root, { sdkVersion: '0.1.0-preview.5.3', reason: 'Review change' }), /forbidden/);
}));
test('a nested directory cannot borrow parent Git provenance', () => fixture(root => {
  mkdirSync(join(root, 'nested'));
  assert.throws(() => verifySourceInventory(join(root, 'nested')), /parent checkout/);
}));
