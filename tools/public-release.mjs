import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { existsSync, lstatSync, realpathSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { basename, dirname, isAbsolute, join, relative, resolve, sep } from 'node:path';

export const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
export function run(command, args, cwd, options = {}) {
  const result = spawnSync(command, args, { cwd, encoding: 'utf8', maxBuffer: 32 * 1024 ** 2, ...options });
  if (result.error || result.status !== 0) throw Error(`${command} failed: ${result.error?.message ?? result.stderr ?? result.status}`);
  return result.stdout;
}
export function cleanSource(root) {
  if (realpathSync(run('git', ['rev-parse', '--show-toplevel'], root).trim()) !== realpathSync(root)) throw Error('An independent source checkout is required');
  if (run('git', ['status', '--porcelain=v1', '--untracked-files=all'], root).trim()) throw Error('Publication requires clean, committed source');
  return run('git', ['rev-parse', 'HEAD'], root).trim();
}
export function versionOf(root) {
  const file = join(root, 'package.json');
  return existsSync(file) ? JSON.parse(readFileSync(file)).version : readFileSync(join(root, 'weapp_container/pubspec.yaml'), 'utf8').match(/^version: (\S+)$/m)[1];
}
export function filesUnder(root, prefix = '') {
  return readdirSync(join(root, prefix), { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name)).flatMap(entry => {
    const file = prefix + entry.name;
    if (entry.isDirectory()) return filesUnder(root, `${file}/`);
    if (!entry.isFile() || lstatSync(join(root, file)).isSymbolicLink()) throw Error(`Nonregular distribution member: ${file}`);
    return [file];
  });
}
export function contained(root, file) {
  if (typeof file !== 'string' || !file || isAbsolute(file) || /[\\\x00-\x1f]/.test(file) || file.split('/').some(part => !part || part === '.' || part === '..')) throw Error('Invalid distribution path');
  const path = resolve(root, file), rel = relative(resolve(root), path);
  if (rel.startsWith(`..${sep}`) || rel === '..' || isAbsolute(rel)) throw Error('Distribution path escapes its directory');
  for (let ancestor = path; ancestor !== resolve(root); ancestor = dirname(ancestor)) if (lstatSync(ancestor).isSymbolicLink()) throw Error('Distribution symlinks are forbidden');
  if (!lstatSync(path).isFile()) throw Error('A regular distribution file is required');
  return path;
}
export function assetEntry(directory, fileName, extra = {}) {
  const bytes = readFileSync(contained(directory, fileName));
  return { ...extra, fileName, size: bytes.length, sha256: sha256(bytes) };
}
export function validateRelease(value, repository, version) {
  if (value.schemaVersion !== 1 || value.repository !== repository || value.sdkVersion !== version || value.tag !== `v${version}`
    || value.sourceDirty !== false || !/^[a-f0-9]{40}$/.test(value.sourceCommit ?? '') || !Array.isArray(value.assets) || !value.assets.length) throw Error('Public release identity does not match the requested SDK');
  const names = new Set();
  for (const asset of value.assets) {
    if (!/^[A-Za-z0-9][A-Za-z0-9._-]+$/.test(asset.fileName ?? '') || names.has(asset.fileName) || !/^[a-f0-9]{64}$/.test(asset.sha256 ?? '') || !Number.isSafeInteger(asset.size) || asset.size < 1) throw Error('Invalid, duplicate or unverified release asset');
    names.add(asset.fileName);
  }
  return value;
}
export function releaseUrl(release, asset) {
  validateRelease(release, release.repository, release.sdkVersion);
  if (!/^weapp-container\/sdk-(ios|desktop|android)$/.test(release.repository) || !release.assets.includes(asset)) throw Error('Unexpected public binary repository');
  return `https://github.com/${release.repository}/releases/download/${release.tag}/${asset.fileName}`;
}
export function readRelease(path, repository, version, verifyFiles = false) {
  const value = validateRelease(JSON.parse(readFileSync(path)), repository, version);
  if (verifyFiles) for (const entry of value.assets) {
    const actual = assetEntry(dirname(resolve(path)), entry.fileName);
    if (actual.size !== entry.size || actual.sha256 !== entry.sha256) throw Error(`Release asset changed: ${entry.fileName}`);
  }
  return value;
}
export function writeRelease(root, directory, repository, assets) {
  const sdkVersion = versionOf(root);
  const value = { schemaVersion: 1, repository, sdkVersion, tag: `v${sdkVersion}`, sourceCommit: cleanSource(root), sourceDirty: false, assets };
  validateRelease(value, repository, sdkVersion);
  writeFileSync(join(directory, 'release.json'), JSON.stringify(value, null, 2) + '\n');
  writeFileSync(join(directory, 'SHA256SUMS'), [...assets, assetEntry(directory, 'release.json')].map(entry => `${entry.sha256}  ${entry.fileName}`).join('\n') + '\n');
  return value;
}
/** Freeze and verify bytes before creating a draft. A failed upload is retained for inspection. */
export async function publishRelease(root, path, execute = false) {
  const repository = JSON.parse(readFileSync(path)).repository;
  const release = readRelease(path, repository, versionOf(root), true);
  if (cleanSource(root) !== release.sourceCommit) throw Error('Release must be published from its exact producer commit');
  const directory = dirname(resolve(path));
  if (!execute) return { status: 'verified', release, published: false };
  const existing = spawnSync('gh', ['api', `repos/${repository}/releases/tags/${release.tag}`], { encoding: 'utf8' });
  if (existing.status === 0) throw Error('The release already exists; assets will never be overwritten');
  if (!existing.stderr.includes('404')) throw Error('Cannot establish that the release is absent');
  const ref = spawnSync('gh', ['api', `repos/${repository}/git/ref/tags/${release.tag}`], { encoding: 'utf8' });
  if (ref.status === 0 || !ref.stderr.includes('404')) throw Error('Tag exists or cannot be checked; refusing publication');
  run('gh', ['release', 'create', release.tag, '--repo', repository, '--target', release.sourceCommit, '--draft', '--prerelease', '--title', `SDK ${release.sdkVersion}`, '--notes', 'Public preview. See the versioned SDK README for installation and platform requirements.'], root);
  run('gh', ['release', 'upload', release.tag, '--repo', repository, ...release.assets.map(entry => join(directory, entry.fileName)), path, join(directory, 'SHA256SUMS')], root);
  const remote = JSON.parse(run('gh', ['api', `repos/${repository}/releases/tags/${release.tag}`], root));
  for (const entry of [...release.assets, assetEntry(directory, basename(path)), assetEntry(directory, 'SHA256SUMS')]) {
    const found = remote.assets.find(asset => asset.name === entry.fileName);
    if (found?.size !== entry.size || found?.digest !== `sha256:${entry.sha256}`) throw Error(`Remote asset verification failed: ${entry.fileName}; draft retained`);
  }
  readRelease(path, repository, versionOf(root), true);
  if (cleanSource(root) !== release.sourceCommit) throw Error('Source changed during publication; draft retained');
  run('gh', ['release', 'edit', release.tag, '--repo', repository, '--draft=false', '--prerelease', '--latest=false'], root);
  return { status: 'published', release };
}
