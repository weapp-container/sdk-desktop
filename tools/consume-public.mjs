import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { releaseUrl, run, sha256, validateRelease, versionOf } from './public-release.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const version = versionOf(root);
const pin = validateRelease(JSON.parse(readFileSync(join(root, `public-release-assets.${version}.json`))), 'weapp-container/sdk-desktop', version);
const platform = ({ darwin: 'macos-universal', linux: 'linux-x86_64', win32: 'windows-x86_64' })[process.platform];
const asset = pin.assets.find(value => value.component === 'desktop-sdk' && value.platform === platform);
assert.ok(asset, 'A public archive for this host is required');
const parent = join(root, 'artifacts/anonymous-consumers'); mkdirSync(parent, { recursive: true });
const work = mkdtempSync(join(parent, `${platform}-`));
const report = { status: 'running', authentication: 'none', sdkVersion: version, platform, sourceCommit: pin.sourceCommit, asset };
async function download(url, maximum) {
  // Native fetch does not read gh/npm credentials. No Authorization header is supplied.
  let response;
  try { response = await fetch(url, { signal: AbortSignal.timeout(300000) }); }
  catch (error) { throw Error(`Anonymous download failed: ${url} (${error.cause?.code ?? error.message})`); }
  assert.equal(response.status, 200, `Public download unavailable: ${url}; no fallback is permitted`);
  const chunks = []; let size = 0;
  for await (const chunk of response.body) { size += chunk.length; assert.ok(size <= maximum, 'Download exceeds its pin'); chunks.push(chunk); }
  return Buffer.concat(chunks);
}
try {
  const base = `https://github.com/${pin.repository}/releases/download/${pin.tag}/`;
  const metadata = await download(base + 'release.json', 16384);
  assert.deepEqual(JSON.parse(metadata), pin, 'Public manifest differs from the reviewed pin');
  const bytes = await download(releaseUrl(pin, asset), asset.size);
  assert.equal(bytes.length, asset.size); assert.equal(sha256(bytes), asset.sha256);
  const archive = join(work, asset.fileName); writeFileSync(archive, bytes);
  const installed = join(work, 'installed');
  run(process.env.PYTHON ?? 'python3', [join(root, 'tools/unpack-runtime.py'), '--archive', archive, '--destination', installed,
    '--sha256', asset.sha256, '--identity-json', JSON.stringify({ repository: pin.repository, version, platform })], root);
  const fixture = await download('https://raw.githubusercontent.com/weapp-container/sdk-contract/22cafeb56ac5d214c35fc513b1dbe5691e1b801d/fixtures/make-acceptance-pack.mjs', 16384);
  assert.equal(sha256(fixture), '44d4072fa62266233dddccd5297b638c8ad545a7691dabbea290432d8051c515');
  const generator = join(work, 'make-acceptance-pack.mjs'), pack = join(work, 'pack'); writeFileSync(generator, fixture);
  run(process.execPath, [generator, '--out', pack], root);
  report.consumer = JSON.parse(run(process.execPath, [join(root, 'tools/consumer-smoke.mjs'), '--sdk', installed, '--pack', pack], root));
  assert.equal(report.consumer.status, 'passed'); report.status = 'passed';
} catch (error) { report.status = 'failed'; report.error = error.message; process.exitCode = 1; }
writeFileSync(join(work, 'validation.json'), JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify({ status: report.status, report: join(work, 'validation.json'), error: report.error }, null, 2));
