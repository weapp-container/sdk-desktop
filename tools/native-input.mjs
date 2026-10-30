import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { cpSync, existsSync, lstatSync, mkdirSync, readFileSync, readdirSync } from 'node:fs';
import { basename, isAbsolute, relative, resolve, sep } from 'node:path';

export const SDK_VERSION = '0.1.0-preview.5.6';
export const HEADER_SHA256 = '2a4761c0650c72ccd8d0a791fb66ee48888e1ce182ba99f0c92673a2c44a8e9b';
export const sha256 = path => createHash('sha256').update(readFileSync(path)).digest('hex');

export function sourceProvenance(root) {
  const origin = JSON.parse(readFileSync(resolve(root, 'ORIGIN.json'), 'utf8'));
  const top = spawnSync('git', ['rev-parse', '--show-toplevel'], { cwd: root, encoding: 'utf8' });
  const ownGit = top.status === 0 && resolve(top.stdout.trim()) === resolve(root);
  const revision = ownGit ? spawnSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }) : undefined;
  const status = ownGit ? spawnSync('git', ['status', '--porcelain', '--untracked-files=normal'], { cwd: root, encoding: 'utf8' }) : undefined;
  return {
    sourceCommit: revision?.status === 0 ? revision.stdout.trim() : null,
    sourceDirty: status?.status === 0 ? Boolean(status.stdout.trim()) : null,
    sourceKind: revision?.status === 0 ? 'git-checkout' : 'source-export',
    originSourceCommit: origin.sourceCommit, originManifestSha256: sha256(resolve(root, 'ORIGIN.json')),
  };
}

/** Distribution receipts retain identities; command paths stay in external validation. */
export function nativeIdentity(receipt) {
  const keys = ['schemaVersion', 'sdkVersion', 'abiVersion', 'status', 'target', 'platform', 'profile', 'features', 'viewDiagnostics', 'sourceCommit', 'sourceDirty'];
  const identity = Object.fromEntries(keys.filter(key => Object.hasOwn(receipt, key)).map(key => [key, receipt[key]]));
  identity.artifacts = receipt.artifacts.map(artifact => Object.fromEntries(['file', 'bytes', 'sha256'].filter(key => Object.hasOwn(artifact, key)).map(key => [key, artifact[key]])));
  return identity;
}

export function option(args, name, fallback) {
  const index = args.indexOf(name);
  if (index < 0) return fallback;
  if (!args[index + 1] || args[index + 1].startsWith('--')) throw new Error(`${name} requires a value`);
  return args[index + 1];
}

export function inside(root, name) {
  if (typeof name !== 'string' || !name || isAbsolute(name) || name.includes('\\')) throw new Error('Invalid artifact path');
  const path = resolve(root, name), rel = relative(resolve(root), path);
  if (!rel || rel === '..' || rel.startsWith(`..${sep}`)) throw new Error('Artifact path escapes its input directory');
  let parent = resolve(root);
  if (lstatSync(parent).isSymbolicLink()) throw new Error('Artifact roots must not be symlinks');
  for (const part of rel.split(sep)) {
    parent = resolve(parent, part);
    if (lstatSync(parent).isSymbolicLink()) throw new Error(`Artifact symlink is forbidden: ${name}`);
  }
  return path;
}

/** Inputs are versioned private runtime distributions, never a parent source tree. */
export function readNative(nativeRoot, target, requiredLibraries) {
  const directory = inside(nativeRoot, target);
  const producerPath = inside(directory, 'build.json');
  const receipt = JSON.parse(readFileSync(producerPath, 'utf8'));
  if (receipt.schemaVersion !== 1 || receipt.abiVersion !== 1 || receipt.status !== 'built' || receipt.target !== target || receipt.sdkVersion !== SDK_VERSION) {
    throw new Error(`A successfully built ${SDK_VERSION} runtime receipt is required for ${target}`);
  }
  if (!Array.isArray(receipt.artifacts)) throw new Error('Native receipt is missing artifact hashes');
  const names = new Set();
  for (const artifact of receipt.artifacts) {
    if (typeof artifact.file !== 'string' || !artifact.file || isAbsolute(artifact.file) || artifact.file.includes('\\') || artifact.file.split('/').some(part => !part || part === '.' || part === '..') || names.has(artifact.file) || !/^[a-f0-9]{64}$/.test(artifact.sha256)) throw new Error('Invalid or duplicate native artifact');
    names.add(artifact.file);
  }
  let artifacts = receipt.artifacts, distribution;
  if (existsSync(resolve(directory, 'runtime-distribution.json'))) {
    distribution = JSON.parse(readFileSync(inside(directory, 'runtime-distribution.json'), 'utf8'));
    if (!/^[a-f0-9]{40}$/.test(receipt.sourceCommit) || typeof receipt.sourceDirty !== 'boolean') throw new Error('Native distribution requires a concrete producer source identity');
    for (const key of ['schemaVersion', 'sdkVersion', 'abiVersion', 'target', 'sourceCommit', 'sourceDirty']) {
      if (!Object.hasOwn(receipt, key) || distribution[key] !== receipt[key]) throw new Error(`Native distribution identity mismatch: ${key}`);
    }
    if (distribution.producerReceipt?.file !== 'build.json' || distribution.producerReceipt.sha256 !== sha256(producerPath)) throw new Error('Native distribution producer receipt mismatch');
    if (!Array.isArray(distribution.selectedArtifacts) || !distribution.selectedArtifacts.length || !Array.isArray(distribution.omittedArtifacts)) throw new Error('Native distribution must declare selected and omitted artifacts');
    const inventory = new Map(receipt.artifacts.map(item => [item.file, item]));
    const covered = new Set();
    const omittedLibrary = target.endsWith('-linux-android') ? 'libweapp_container_sdk.a' : /-apple-ios(?:-sim)?$/.test(target) ? 'libweapp_container_sdk.dylib' : undefined;
    for (const [group, items] of [['selected', distribution.selectedArtifacts], ['omitted', distribution.omittedArtifacts]]) {
      for (const item of items) {
        if (!item || typeof item !== 'object' || Object.keys(item).sort().join(',') !== 'bytes,file,sha256' || !Number.isSafeInteger(item.bytes) || item.bytes < 0 || covered.has(item.file)) throw new Error('Invalid or duplicate native distribution artifact');
        const original = inventory.get(item.file);
        if (!original || original.bytes !== item.bytes || original.sha256 !== item.sha256) throw new Error(`Native distribution artifact differs from producer: ${item.file}`);
        if (group === 'omitted' && item.file !== omittedLibrary) throw new Error(`Native distribution cannot omit required platform artifact: ${item.file}`);
        covered.add(item.file);
      }
    }
    if (covered.size !== inventory.size) throw new Error('Native distribution does not cover the complete producer inventory');
    artifacts = distribution.selectedArtifacts;
    names.clear();
    for (const item of artifacts) names.add(item.file);
    if (!names.has('weapp_container_sdk.h')) throw new Error('Native distribution must select the reviewed C header');
  }
  for (const artifact of artifacts) {
    if (sha256(inside(directory, artifact.file)) !== artifact.sha256) throw new Error(`Native hash mismatch: ${artifact.file}`);
    if (distribution && lstatSync(inside(directory, artifact.file)).size !== artifact.bytes) throw new Error(`Native size mismatch: ${artifact.file}`);
  }
  for (const name of requiredLibraries) if (!names.has(name)) throw new Error(`Missing hashed native library: ${name}`);
  if (sha256(inside(directory, 'weapp_container_sdk.h')) !== HEADER_SHA256) throw new Error('Native C header does not match reviewed SDK contract');
  return { directory, receipt, distribution };
}

export function readNotices(nativeRoot, licensesRoot, target) {
  const notices = inside(licensesRoot, target);
  const inventory = JSON.parse(readFileSync(inside(notices, 'licenses.json'), 'utf8'));
  if (inventory.schemaVersion !== 1 || inventory.target !== target || !Array.isArray(inventory.packages) || !inventory.packages.length || inventory.packages.some(pkg => !Array.isArray(pkg.files) || !pkg.files.length)) {
    throw new Error(`Complete native dependency notices are required for ${target}`);
  }
  for (const pkg of inventory.packages) for (const file of pkg.files) {
    if (!/^[a-f0-9]{64}$/.test(file.sha256) || sha256(inside(notices, file.path)) !== file.sha256) throw new Error(`Native notice integrity mismatch: ${file.path}`);
  }
  const runtimeLicense = inside(nativeRoot, 'RUNTIME-LICENSE');
  if (!readFileSync(runtimeLicense, 'utf8').trim()) throw new Error('The private runtime license is empty');
  return { notices, runtimeLicense };
}

export function copyVerifiedTree(source, destination) {
  const visit = path => {
    const stat = lstatSync(path);
    if (stat.isSymbolicLink()) throw new Error(`Distribution symlink is forbidden: ${path}`);
    if (stat.isDirectory()) for (const name of readdirSync(path)) visit(resolve(path, name));
    else if (!stat.isFile()) throw new Error(`Distribution contains a non-file: ${path}`);
  };
  visit(source);
  cpSync(source, destination, { recursive: true });
}

const canonical = value => {
  if (Array.isArray(value)) return `[${value.map(canonical).join(',')}]`;
  if (value !== null && typeof value === 'object') return `{${Object.keys(value).sort((a, b) => Buffer.compare(Buffer.from(a), Buffer.from(b))).map(key => `${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}`;
  return JSON.stringify(value);
};

/** Consumers receive an explicitly supplied pack; the SDK never imports a core fixture. */
export function copyPack(source, destination) {
  if (!source) throw new Error('--pack must name a verified local portable pack');
  source = resolve(source); destination = resolve(destination);
  const relation = relative(source, destination);
  if (!relation || (!relation.startsWith(`..${sep}`) && relation !== '..' && !isAbsolute(relation))) throw new Error('Pack destination must not be inside its source');
  const manifest = JSON.parse(readFileSync(inside(source, 'manifest.json'), 'utf8'));
  const original = manifest.integrity?.root;
  if (manifest.schemaVersion !== 1 || !/^[a-f0-9]{64}$/.test(original || '')) throw new Error('Invalid portable manifest');
  manifest.integrity.root = '';
  if (createHash('sha256').update(canonical(manifest)).digest('hex') !== original) throw new Error('Portable manifest integrity mismatch');
  const blobs = inside(source, 'blobs/sha256');
  for (const file of readdirSync(blobs)) if (!/^[a-f0-9]{64}$/.test(file) || sha256(inside(blobs, file)) !== basename(file)) throw new Error(`Portable blob integrity mismatch: ${file}`);
  const references = value => {
    if (typeof value === 'string' && /^sha256:[a-f0-9]{64}$/.test(value)) inside(blobs, value.slice(7));
    else if (Array.isArray(value)) for (const child of value) references(child);
    else if (value && typeof value === 'object') for (const [key, child] of Object.entries(value)) {
      if (key === 'sha256' && typeof child === 'string') inside(blobs, child);
      else references(child);
    }
  };
  references(manifest);
  mkdirSync(resolve(destination, '..'), { recursive: true });
  copyVerifiedTree(source, destination);
  return original;
}

export function requireNativeRoot(args) {
  const value = option(args, '--native-root', process.env.WEAPP_CONTAINER_RUNTIME_NATIVE_ROOT);
  if (!value) throw new Error('--native-root is required; prepare authorized version-pinned runtime artifacts first');
  const path = resolve(value);
  if (!existsSync(path)) throw new Error(`Native input directory is missing: ${path}`);
  return path;
}
