#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { lstatSync, readFileSync, readdirSync, realpathSync, statSync, writeFileSync } from 'node:fs';
import { dirname, isAbsolute, join, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

export const INVENTORY_FILE = 'SOURCE-INVENTORY.json';
const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
const fail = message => { throw new Error(`Source inventory: ${message}`); };
function git(root, args) {
  const value = spawnSync('git', args, { cwd: root, encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 });
  if (value.status !== 0) fail(`git ${args[0]} failed: ${value.stderr}`);
  return value.stdout;
}
function checkoutFiles(directory, writing = false) {
  const root = realpathSync(directory);
  const top = realpathSync(git(root, ['rev-parse', '--show-toplevel']).trim());
  // NTFS can spell the same directory using long/8.3 names or different drive
  // letter casing. Check the actual directory identity, without case-folding
  // unrelated paths or accepting a parent checkout.
  const rootInfo = statSync(root, { bigint: true }), topInfo = statSync(top, { bigint: true });
  if (top !== root && (!rootInfo.ino || rootInfo.ino !== topInfo.ino || rootInfo.dev !== topInfo.dev)) fail('a parent checkout cannot supply source provenance');
  const ignored = git(root, ['ls-files', '--others', '--ignored', '--exclude-standard', '--directory', '-z']).split('\0').filter(Boolean).map(path => path.replace(/\/$/, ''));
  function inspectMetadata(directory, prefix = '') {
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      const path = `${prefix}${entry.name}`;
      if (entry.name === '.git') {
        if (prefix || entry.isSymbolicLink()) fail(`unreviewed or linked Git metadata: ${path}`);
        continue;
      }
      if (ignored.some(item => path === item || path.startsWith(`${item}/`))) continue;
      if (entry.isDirectory()) inspectMetadata(join(directory, entry.name), `${path}/`);
    }
  }
  inspectMetadata(root);
  const untracked = git(root, ['ls-files', '--others', '--exclude-standard', '-z']).split('\0').filter(Boolean);
  if (untracked.some(file => !writing || file !== INVENTORY_FILE)) fail(`untracked source must be reviewed and staged first: ${untracked.join(', ')}`);
  const files = git(root, ['ls-files', '--cached', '-z']).split('\0').filter(Boolean).sort();
  if (new Set(files).size !== files.length) fail('duplicate or conflicted tracked paths');
  return { root, files };
}
function checkedBytes(root, path) {
  if (typeof path !== 'string' || isAbsolute(path) || path.includes('\\') || path.split('/').some(part => !part || part === '.' || part === '..')) fail(`invalid path: ${path}`);
  if (/(?:^|\/)(?:crates|target|\.gradle|\.cxx|Pods|\.symlinks)(?:\/|$)/.test(path) || /\.(?:rs|aar|a|so|dylib|dll|exe|wasm|tgz|tar\.gz|class|pyc)$/i.test(path) || path === 'Cargo.toml') fail(`core, binary or cache path is forbidden: ${path}`);
  const absolute = join(root, path);
  let ancestor = absolute;
  while (ancestor !== root) {
    if (lstatSync(ancestor).isSymbolicLink()) fail(`symbolic link is forbidden: ${path}`);
    ancestor = dirname(ancestor);
  }
  const stat = lstatSync(absolute);
  if (!stat.isFile() || !realpathSync(absolute).startsWith(`${root}${sep}`)) fail(`not a contained regular file: ${path}`);
  const bytes = readFileSync(absolute);
  if (bytes.includes(0)) fail(`binary source bytes are forbidden: ${path}`);
  // Windows filesystem modes have no Unix executable bit. Preserve the
  // reviewed tracked Git mode there; Unix still checks the actual file mode.
  let mode = (stat.mode & 0o111) ? '100755' : '100644';
  if (process.platform === 'win32') {
    const stage = git(root, ['ls-files', '--stage', '--', path]);
    const match = /^(100644|100755) [a-f0-9]+ 0\t/.exec(stage);
    if (!match || stage.trimEnd().split('\n').length !== 1) fail(`invalid tracked file mode: ${path}`);
    mode = match[1];
  }
  return { bytes, mode };
}
function entriesFor(root, files) {
  return files.filter(path => path !== INVENTORY_FILE).map(path => {
    const { bytes, mode } = checkedBytes(root, path);
    return { path, sha256: sha256(bytes), size: bytes.length, mode };
  });
}
function historicalOrigin(root) {
  // ORIGIN is the historical extraction receipt. A new current inventory must
  // not silently rewrite it to describe later SDK evolution.
  const original = git(root, ['show', 'HEAD:ORIGIN.json']);
  const current = checkedBytes(root, 'ORIGIN.json').bytes;
  if (sha256(original) !== sha256(current)) fail('ORIGIN.json differs from the committed historical receipt');
  return { path: 'ORIGIN.json', sha256: sha256(current) };
}
export function writeSourceInventory(directory, { sdkVersion, reason } = {}) {
  if (!/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(sdkVersion ?? '') || typeof reason !== 'string' || !reason.trim()) fail('refresh requires an explicit SDK version and review reason');
  const { root, files } = checkoutFiles(directory, true);
  const entries = entriesFor(root, files);
  const value = {
    schemaVersion: 2,
    sdkVersion,
    // Bind reviewed bytes without depending on Git ancestry or a self-referential commit hash.
    sourceFilesSha256: sha256(Buffer.from(JSON.stringify(entries))),
    review: reason.trim(),
    scope: 'Current reviewed SDK wrapper source bytes only; this inventory is not build, artifact, device or parity acceptance evidence.',
    historicalOrigin: historicalOrigin(root),
    files: entries,
  };
  writeFileSync(join(root, INVENTORY_FILE), `${JSON.stringify(value, null, 2)}\n`);
  return value;
}
export function verifySourceInventory(directory) {
  const { root, files } = checkoutFiles(directory);
  const { bytes } = checkedBytes(root, INVENTORY_FILE);
  const value = JSON.parse(bytes);
  const validIdentity = value.schemaVersion === 1
    ? /^[a-f0-9]{40}$/.test(value.sourceBaseCommit ?? '')
    : value.schemaVersion === 2 && /^[a-f0-9]{64}$/.test(value.sourceFilesSha256 ?? '');
  if (!validIdentity || !value.sdkVersion || !value.review || !Array.isArray(value.files)) fail('invalid inventory metadata');
  if (value.schemaVersion === 1) git(root, ['merge-base', '--is-ancestor', value.sourceBaseCommit, 'HEAD']);
  else if (value.sourceFilesSha256 !== sha256(Buffer.from(JSON.stringify(value.files)))) fail('reviewed source fingerprint mismatch');
  const expected = [...value.files.map(entry => entry.path), INVENTORY_FILE].sort();
  if (new Set(expected).size !== expected.length || JSON.stringify(files) !== JSON.stringify(expected)) fail('tracked source differs from the reviewed allowlist');
  const actual = entriesFor(root, files);
  if (JSON.stringify(actual) !== JSON.stringify(value.files)) fail('source hash, size, mode or ordering differs from the reviewed inventory');
  const origin = actual.find(entry => entry.path === 'ORIGIN.json');
  if (value.historicalOrigin?.path !== 'ORIGIN.json' || value.historicalOrigin.sha256 !== origin?.sha256) fail('historical ORIGIN identity mismatch');
  return { files, sdkVersion: value.sdkVersion, inventorySha256: sha256(bytes) };
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
  const args = process.argv.slice(2), option = name => { const i = args.indexOf(name); return i < 0 ? undefined : args[i + 1]; };
  if (args.includes('--write')) {
    const value = writeSourceInventory(root, { sdkVersion: option('--version'), reason: option('--reason') });
    console.log(`Reviewed ${value.files.length} source files for ${value.sdkVersion}. Stage ${INVENTORY_FILE} and run verification.`);
  } else {
    const value = verifySourceInventory(root);
    console.log(JSON.stringify({ status: 'verified', sdkVersion: value.sdkVersion, files: value.files.length, inventorySha256: value.inventorySha256 }));
  }
}
