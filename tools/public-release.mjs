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
/** Inspect uncompressed installed bytes, including static library metadata. */
export function verifyPortableDistribution(root) {
  const privatePath = /(?<![A-Za-z0-9_./-])\/(?:Users\/|home\/|private\/(?:var|tmp)\/|var\/folders\/|Volumes\/)|[A-Za-z]:[\\/](?:Users|a|Temp|work|build|actions-runner)[\\/]|\\\\[A-Za-z0-9_.-]{3,}\\[A-Za-z0-9_.-]{3,}\\/;
  const secret = /(?:github_pat_[A-Za-z0-9_]{15,}|gh[pousr]_[A-Za-z0-9]{20,}|AKIA[A-Z0-9]{16}|-----BEGIN (?:RSA |OPENSSH |EC |DSA )?PRIVATE KEY-----)/;
  for (const file of filesUnder(root)) {
    const bytes = readFileSync(join(root, file)).toString('latin1');
    if (privatePath.test(bytes)) throw Error(`Absolute private/local path found in ${file}`);
    if (secret.test(bytes)) throw Error(`Credential material found in ${file}`);
  }
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
export async function publishRelease(root, path, execute = false, invoke = run) {
  const repository = JSON.parse(readFileSync(path)).repository;
  const release = readRelease(path, repository, versionOf(root), true);
  if (cleanSource(root) !== release.sourceCommit) throw Error('Release must be published from its exact producer commit');
  const directory = dirname(resolve(path));
  if (!execute) return { status: 'verified', release, published: false };
  const pages = JSON.parse(invoke('gh', ['api', '--paginate', '--slurp', `repos/${repository}/releases?per_page=100`], root));
  if (!Array.isArray(pages) || !pages.every(Array.isArray)) throw Error('Cannot inspect existing releases and drafts');
  if (pages.flat().some(value => value.tag_name === release.tag)) throw Error('The release or draft already exists; assets will never be overwritten');
  let absent = false;
  try { invoke('gh', ['api', `repos/${repository}/git/ref/tags/${release.tag}`], root); }
  catch (error) { if (/HTTP 404/.test(error.message)) absent = true; else throw error; }
  if (!absent) throw Error('Tag exists; refusing publication');
  const created = JSON.parse(invoke('gh', ['api', `repos/${repository}/releases`, '--method', 'POST',
    '-f', `tag_name=${release.tag}`, '-f', `target_commitish=${release.sourceCommit}`, '-F', 'draft=true', '-F', 'prerelease=true',
    '-f', `name=SDK ${release.sdkVersion}`, '-f', 'body=Public preview. See the versioned SDK README for installation and platform requirements.'], root));
  if (!Number.isSafeInteger(created.id) || created.id < 1) throw Error('Draft creation returned no release ID');
  const endpoint = `repos/${repository}/releases/${created.id}`;
  invoke('gh', ['release', 'upload', release.tag, '--repo', repository, ...release.assets.map(entry => join(directory, entry.fileName)), path, join(directory, 'SHA256SUMS')], root);
  // The tag endpoint may return 404 for an unpublished draft. Keep its creation ID.
  const remote = JSON.parse(invoke('gh', ['api', endpoint], root));
  const files = [...release.assets, assetEntry(directory, basename(path)), assetEntry(directory, 'SHA256SUMS')];
  if (remote.id !== created.id || remote.draft !== true || remote.prerelease !== true || remote.tag_name !== release.tag
    || remote.target_commitish !== release.sourceCommit || remote.assets?.length !== files.length) throw Error('Remote draft identity differs; draft retained');
  for (const entry of files) {
    const matches = remote.assets.filter(asset => asset.name === entry.fileName);
    if (matches.length !== 1 || matches[0].state !== 'uploaded' || matches[0].size !== entry.size || matches[0].digest !== `sha256:${entry.sha256}`) throw Error(`Remote asset verification failed: ${entry.fileName}; draft retained`);
  }
  readRelease(path, repository, versionOf(root), true);
  if (cleanSource(root) !== release.sourceCommit) throw Error('Source changed during publication; draft retained');
  invoke('gh', ['api', endpoint, '--method', 'PATCH', '-F', 'draft=false', '-F', 'prerelease=true', '-f', 'make_latest=false'], root);
  return { status: 'published', release };
}
