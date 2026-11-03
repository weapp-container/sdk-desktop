import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

test('reference runner rejects missing, extra and unknown modes before importing a backend or starting resources', () => {
  const runner = fileURLToPath(new URL('./run-host-services-reference.mjs', import.meta.url));
  for (const [args, expected] of [
    [[], 'Expected reference root'],
    [['missing-backend', 'missing-binary', 'missing-pack', '--unknown'], 'Unknown host service reference mode'],
    [['missing-backend', 'missing-binary', 'missing-pack', '--backend-errors-only', 'extra'], 'Expected reference root'],
  ]) {
    const result = spawnSync(process.execPath, [runner, ...args], { encoding: 'utf8' });
    assert.equal(result.status, 1);
    assert.ok(result.stderr.includes(expected), result.stderr);
    assert.ok(!result.stderr.includes('ERR_MODULE_NOT_FOUND'), 'mode validation happened after importing the backend');
  }
});
