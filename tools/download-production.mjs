import { mkdirSync, readFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { cleanSource, run, versionOf } from './public-release.mjs';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const id = process.argv[2];
if (!/^\d+$/.test(id ?? '')) throw Error('Supply the completed Public native production run ID');
cleanSource(root);
const repository = 'weapp-container/weapp-container';
const production = JSON.parse(run('gh', ['api', `repos/${repository}/actions/runs/${id}`], root));
if (production.status !== 'completed' || production.conclusion !== 'success' || production.path !== '.github/workflows/public-native.yml') throw Error('A successful public native production run is required');
const platform = JSON.parse(readFileSync(resolve(root, 'package.json'))).weappContainerPlatform;
const targets = platform === 'android' ? ['aarch64-linux-android', 'x86_64-linux-android'] : platform === 'ios' ? ['aarch64-apple-ios', 'aarch64-apple-ios-sim', 'x86_64-apple-ios'] : process.platform === 'darwin' ? ['aarch64-apple-darwin', 'x86_64-apple-darwin'] : process.platform === 'win32' ? ['x86_64-pc-windows-msvc'] : ['x86_64-unknown-linux-gnu'];
for (const target of targets) {
  for (const kind of ['native', 'licenses']) {
    const destination = resolve(root, '.runtime', kind, target);
    mkdirSync(destination, { recursive: true });
    run('gh', ['run', 'download', id, '--repo', repository, '--name', `sdk-${kind}-${target}`, '--dir', destination], root);
  }
  const receipt = JSON.parse(readFileSync(resolve(root, '.runtime/native', target, 'build.json')));
  if (receipt.sdkVersion !== versionOf(root) || receipt.sourceCommit !== production.head_sha || receipt.sourceDirty !== false || receipt.status !== 'built' || receipt.target !== target || receipt.distributionProfile !== 'full') throw Error(`Production identity mismatch: ${target}`);
}
const { copyFileSync } = await import('node:fs');
copyFileSync(resolve(root, '.runtime/native', targets[0], 'RUNTIME-LICENSE'), resolve(root, '.runtime/native/RUNTIME-LICENSE'));
console.log(`Downloaded exact ${versionOf(root)} production ${production.head_sha}; SDK build tools will verify every native member`);
