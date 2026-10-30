#!/usr/bin/env node
/** Authorized, content-pinned runtime distribution retrieval. Node.js 24+. */
import { createHash, randomUUID } from 'node:crypto';
import { createReadStream } from 'node:fs';
import { lstat, mkdir, open, readFile, realpath, rename, rm, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';

export const RUNTIME_REPOSITORY = 'weapp-container/runtime-artifacts';
export const SDK_VERSION = '0.1.0-preview.5.7';
export const DEFAULT_ARTIFACT_TIMEOUT_MS = 15 * 60 * 1000;
export const MAX_ARTIFACT_TIMEOUT_MS = 60 * 60 * 1000;
export const METADATA_TIMEOUT_MS = 30 * 1000;
const MAX_BYTES = 8 * 1024 ** 3;
const PLATFORMS = {
  android: ['arm64-v8a', 'x86_64'],
  ios: ['arm64-device', 'arm64-simulator', 'x86_64-simulator'],
  macos: ['arm64', 'x86_64'], windows: ['x86_64'], linux: ['x86_64'],
};
const COMPONENTS = ['runtime', 'android-sdk', 'ios-sdk', 'desktop-sdk'];
const ownKeys = (object, allowed) => Object.keys(object).every(key => allowed.includes(key));
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const semver = value => typeof value === 'string' && /^\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$/.test(value);
function fail(code, message) { const error = new Error(message); error.code = code; throw error; }
function requireValue(condition, message) { if (!condition) fail('ERR_RUNTIME_MANIFEST', message); }
function timeoutValue(value, text = false) {
  if (text && (typeof value !== 'string' || !/^[1-9]\d*$/.test(value))) fail('ERR_RUNTIME_OPTIONS', 'Runtime timeout must be a positive base-10 integer in milliseconds.');
  const number = text ? Number(value) : value;
  if (!Number.isSafeInteger(number) || number < 1 || number > MAX_ARTIFACT_TIMEOUT_MS) fail('ERR_RUNTIME_OPTIONS', `Runtime timeout must be between 1 and ${MAX_ARTIFACT_TIMEOUT_MS} milliseconds.`);
  return number;
}
function networkError() { const error = new Error('Runtime release transfer failed or timed out.'); error.code = 'ERR_RUNTIME_NETWORK'; return error; }
function ensureNetworkActive(signal) { if (signal?.aborted) throw networkError(); }
async function withNetworkDeadline(timeoutMs, operation) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try { return await operation(controller.signal); } finally { clearTimeout(timer); }
}
// A body returned by an injected fetch can ignore AbortSignal. Race its reads,
// and dispose late responses, so the same bounded contract applies to all fetches.
function networkOperation(promise, signal) {
  return new Promise((resolve, reject) => {
    const abort = () => { cleanup(); reject(networkError()); };
    const cleanup = () => signal.removeEventListener('abort', abort);
    signal.addEventListener('abort', abort, { once: true });
    Promise.resolve(promise).then(value => { cleanup(); resolve(value); }, () => { cleanup(); reject(networkError()); });
    if (signal.aborted) abort();
  });
}
function discardBody(body) {
  try { void Promise.resolve(body?.cancel()).catch(() => {}); } catch { /* cancellation cannot reveal remote text */ }
}
async function* networkBody(body, signal) {
  let reader, done = false;
  try { reader = body.getReader(); } catch { throw networkError(); }
  try {
    while (true) {
      ensureNetworkActive(signal);
      let result;
      try { result = await networkOperation(reader.read(), signal); } catch { throw networkError(); }
      ensureNetworkActive(signal);
      if (result.done) { done = true; return; }
      yield result.value;
    }
  } finally {
    if (!done) { try { void Promise.resolve(reader.cancel()).catch(() => {}); } catch { /* best-effort bounded disposal */ } }
    try { reader.releaseLock(); } catch { /* a cancelled pending read cannot block cleanup */ }
  }
}

export function selectArtifact(manifest, expected) {
  const { version, platform, arch, abiVersion = 1, component = 'runtime' } = expected;
  requireValue(semver(version) && Object.hasOwn(PLATFORMS, platform) && typeof arch === 'string'
    && Number.isSafeInteger(abiVersion) && abiVersion > 0 && COMPONENTS.includes(component), 'Explicit version, platform, architecture, component and ABI are required.');
  requireValue(object(manifest) && ownKeys(manifest, ['schemaVersion', 'runtimeVersion', 'abiVersion', 'source', 'artifacts'])
    && manifest.schemaVersion === 1 && manifest.runtimeVersion === version && manifest.abiVersion === abiVersion,
    'Manifest schema, runtime version or ABI does not match the requested distribution.');
  requireValue(object(manifest.source) && ownKeys(manifest.source, ['repository', 'releaseTag'])
    && manifest.source.repository === RUNTIME_REPOSITORY && manifest.source.releaseTag === `runtime-v${version}`,
    'Only an exact versioned release of weapp-container/runtime-artifacts is permitted.');
  requireValue(Array.isArray(manifest.artifacts) && manifest.artifacts.length > 0 && manifest.artifacts.length <= 1000,
    'Manifest requires a bounded non-empty artifact list.');
  const identities = new Set(); const names = new Set();
  for (const entry of manifest.artifacts) {
    requireValue(object(entry) && ownKeys(entry, ['component', 'platform', 'arch', 'architectures', 'fileName', 'size', 'sha256'])
      && COMPONENTS.includes(entry.component) && Object.hasOwn(PLATFORMS, entry.platform), 'Invalid artifact fields or platform.');
    const supported = PLATFORMS[entry.platform];
    requireValue(supported.includes(entry.arch) || entry.arch === 'universal' && supported.length > 1,
      'Unsupported artifact architecture.');
    requireValue(entry.component === 'runtime' || entry.component === `${entry.platform}-sdk`
      || entry.component === 'desktop-sdk' && ['macos', 'windows', 'linux'].includes(entry.platform),
      'Artifact component does not match its platform.');
    if (entry.arch === 'universal') {
      requireValue(Array.isArray(entry.architectures) && entry.architectures.length >= 2
        && new Set(entry.architectures).size === entry.architectures.length
        && entry.architectures.every(value => supported.includes(value)), 'Universal artifacts must name their actual distinct supported slices.');
    } else if (entry.architectures !== undefined) {
      requireValue(Array.isArray(entry.architectures) && entry.architectures.length === 1
        && entry.architectures[0] === entry.arch, 'Architecture metadata does not match the artifact.');
    }
    requireValue(typeof entry.fileName === 'string' && /^[A-Za-z0-9][A-Za-z0-9._-]{0,180}\.tar\.gz$/.test(entry.fileName)
      && Number.isSafeInteger(entry.size) && entry.size > 0 && entry.size <= MAX_BYTES
      && typeof entry.sha256 === 'string' && /^[a-f0-9]{64}$/.test(entry.sha256), 'Artifacts require a safe tar.gz basename, bounded byte size and lowercase SHA-256.');
    const identity = `${entry.component}/${entry.platform}/${entry.arch}`;
    requireValue(!identities.has(identity) && !names.has(entry.fileName), 'Duplicate artifact target or filename.');
    identities.add(identity); names.add(entry.fileName);
  }
  const entry = manifest.artifacts.find(value => value.component === component && value.platform === platform && value.arch === arch);
  if (!entry) fail('ERR_RUNTIME_TARGET', 'The pinned manifest does not contain the exact requested component/platform/architecture.');
  return Object.freeze({ ...entry, runtimeVersion: version, abiVersion, releaseTag: manifest.source.releaseTag });
}

async function digestFile(path, entry) {
  const status = await lstat(path);
  if (!status.isFile() || status.isSymbolicLink() || status.size !== entry.size) fail('ERR_RUNTIME_INTEGRITY', 'Artifact is not a regular file of the pinned size.');
  const hash = createHash('sha256'); let size = 0;
  for await (const chunk of createReadStream(path)) { size += chunk.length; if (size > entry.size) fail('ERR_RUNTIME_INTEGRITY', 'Artifact exceeded its pinned size.'); hash.update(chunk); }
  if (size !== entry.size || hash.digest('hex') !== entry.sha256) fail('ERR_RUNTIME_INTEGRITY', 'Artifact SHA-256 or size does not match its pin.');
}

async function exists(path) { try { await lstat(path); return true; } catch (error) { if (error.code === 'ENOENT') return false; throw error; } }
async function request(url, token, fetchImpl, signal, binary = false) {
  let current = new URL(url);
  for (let redirects = 0; redirects <= 5; redirects++) {
    ensureNetworkActive(signal);
    const api = current.origin === 'https://api.github.com';
    const permittedAPI = api && current.pathname.startsWith(`/repos/${RUNTIME_REPOSITORY}/`);
    if (current.protocol !== 'https:' || current.username || current.password || current.port
      || !(permittedAPI || binary && ['release-assets.githubusercontent.com', 'objects.githubusercontent.com'].includes(current.hostname))) {
      fail('ERR_RUNTIME_REDIRECT', 'Runtime download redirected outside the permitted release endpoints.');
    }
    const headers = { Accept: binary ? 'application/octet-stream' : 'application/vnd.github+json', 'User-Agent': 'weapp-container-runtime-artifacts', 'X-GitHub-Api-Version': '2022-11-28' };
    // Signed storage URLs deliberately receive no Authorization or runtime token.
    if (permittedAPI) headers.Authorization = `Bearer ${token}`;
    let response;
    try {
      const pending = Promise.resolve().then(() => fetchImpl(current, { headers, redirect: 'manual', signal }));
      void pending.then(value => { if (signal.aborted) discardBody(value?.body); }, () => {});
      response = await networkOperation(pending, signal);
    } catch { throw networkError(); }
    if ([301, 302, 303, 307, 308].includes(response.status)) {
      const location = response.headers.get('location');
      discardBody(response.body);
      if (!location || redirects === 5) fail('ERR_RUNTIME_REDIRECT', 'Invalid or excessive runtime download redirects.');
      try { current = new URL(location, current); } catch { fail('ERR_RUNTIME_REDIRECT', 'Invalid runtime download redirect.'); }
      continue;
    }
    if (!response.ok) { discardBody(response.body); fail('ERR_RUNTIME_AUTH', `Runtime release request was refused (HTTP ${response.status}); verify repository read access and the exact release.`); }
    return response;
  }
  fail('ERR_RUNTIME_REDIRECT', 'Excessive runtime download redirects.');
}
async function releaseAsset(entry, token, fetchImpl) {
  return withNetworkDeadline(METADATA_TIMEOUT_MS, async signal => {
    const response = await request(`https://api.github.com/repos/${RUNTIME_REPOSITORY}/releases/tags/${entry.releaseTag}`, token, fetchImpl, signal);
    const chunks = []; let length = 0;
    if (!response.body) fail('ERR_RUNTIME_RELEASE', 'Release response has no body.');
    for await (const chunk of networkBody(response.body, signal)) { length += chunk.length; if (length > 4 * 1024 ** 2) fail('ERR_RUNTIME_RELEASE', 'Release metadata exceeded its limit.'); chunks.push(chunk); }
    let release;
    try { release = JSON.parse(Buffer.concat(chunks).toString('utf8')); } catch { fail('ERR_RUNTIME_RELEASE', 'Release metadata is not valid JSON.'); }
    if (release.tag_name !== entry.releaseTag || release.draft !== false || !Array.isArray(release.assets)) fail('ERR_RUNTIME_RELEASE', 'The exact published versioned release is unavailable.');
    const assets = release.assets.filter(value => value.name === entry.fileName);
    if (assets.length !== 1 || !Number.isSafeInteger(assets[0].id) || assets[0].id <= 0 || assets[0].state !== 'uploaded'
      || assets[0].size !== entry.size || assets[0].digest && assets[0].digest !== `sha256:${entry.sha256}`) {
      fail('ERR_RUNTIME_RELEASE', 'Release asset metadata differs from the pinned distribution.');
    }
    return assets[0].id;
  });
}
async function writeVerified(path, stream, entry, signal) {
  if (!stream) fail('ERR_RUNTIME_INTEGRITY', 'Artifact download has no body.');
  const temporary = join(dirname(path), `.${entry.fileName}.${randomUUID()}.part`);
  let handle;
  try {
    handle = await open(temporary, 'wx', 0o600);
    const hash = createHash('sha256'); let size = 0;
    for await (const chunk of stream) { ensureNetworkActive(signal); size += chunk.length; if (size > entry.size) fail('ERR_RUNTIME_INTEGRITY', 'Artifact exceeded its pinned size.'); hash.update(chunk); await handle.writeFile(chunk); }
    if (size !== entry.size || hash.digest('hex') !== entry.sha256) fail('ERR_RUNTIME_INTEGRITY', 'Downloaded SHA-256 or size does not match its pin.');
    await handle.sync(); await handle.close(); handle = undefined;
    ensureNetworkActive(signal);
    await rename(temporary, path);
  } finally { await handle?.close(); await rm(temporary, { force: true }); }
}

/** Does not build core sources; downloaded and installed bytes are always pinned. */
export async function downloadRuntimeArtifact(options) {
  const { manifestPath, version = SDK_VERSION, platform, arch, component = 'runtime', abiVersion = 1,
    outDirectory, installDirectory, artifactDirectory = process.env.WEAPP_CONTAINER_RUNTIME_ARTIFACT_DIR,
    token = process.env.WEAPP_CONTAINER_RUNTIME_TOKEN, fetchImpl = globalThis.fetch,
    python = process.env.PYTHON ?? 'python3' } = options;
  const timeoutMs = options.timeoutMs !== undefined ? timeoutValue(options.timeoutMs)
    : process.env.WEAPP_CONTAINER_RUNTIME_TIMEOUT_MS !== undefined ? timeoutValue(process.env.WEAPP_CONTAINER_RUNTIME_TIMEOUT_MS, true)
      : DEFAULT_ARTIFACT_TIMEOUT_MS;
  if (!manifestPath || !outDirectory) fail('ERR_RUNTIME_OPTIONS', 'Explicit manifestPath and outDirectory are required.');
  if ((await lstat(manifestPath)).size > 1024 ** 2) fail('ERR_RUNTIME_MANIFEST', 'Artifact manifest exceeded its size limit.');
  let manifest;
  try { manifest = JSON.parse(await readFile(manifestPath, 'utf8')); } catch { fail('ERR_RUNTIME_MANIFEST', 'Artifact manifest is not valid JSON.'); }
  const entry = selectArtifact(manifest, { version, platform, arch, component, abiVersion });
  const directory = resolve(outDirectory); await mkdir(directory, { recursive: true });
  const path = join(directory, entry.fileName);
  let source;
  if (await exists(path)) { await digestFile(path, entry); source = 'verified-cache'; }
  else if (artifactDirectory !== undefined) {
    if (!artifactDirectory) fail('ERR_RUNTIME_OPTIONS', 'The explicit offline artifact directory cannot be empty.');
    const localRoot = await realpath(artifactDirectory); const local = join(localRoot, entry.fileName);
    await digestFile(local, entry);
    if (dirname(await realpath(local)) !== localRoot) fail('ERR_RUNTIME_INTEGRITY', 'Offline artifact escaped its selected directory.');
    await writeVerified(path, createReadStream(local), entry); source = 'offline';
  } else {
    if (typeof token !== 'string' || !token.trim() || /[\r\n]/.test(token)) fail('ERR_RUNTIME_CREDENTIALS', 'WEAPP_CONTAINER_RUNTIME_TOKEN with read access to private weapp-container/runtime-artifacts is required; or supply an explicit offline artifact directory.');
    const assetId = await releaseAsset(entry, token, fetchImpl);
    await withNetworkDeadline(timeoutMs, async signal => {
      const response = await request(`https://api.github.com/repos/${RUNTIME_REPOSITORY}/releases/assets/${assetId}`, token, fetchImpl, signal, true);
      const declared = response.headers.get('content-length');
      if (declared !== null && Number(declared) !== entry.size) { discardBody(response.body); fail('ERR_RUNTIME_INTEGRITY', 'Download Content-Length differs from the artifact pin.'); }
      if (!response.body) fail('ERR_RUNTIME_INTEGRITY', 'Artifact download has no body.');
      try { await writeVerified(path, networkBody(response.body, signal), entry, signal); source = 'github'; }
      finally { discardBody(response.body); }
    });
  }
  const receipt = { schemaVersion: 1, repository: RUNTIME_REPOSITORY, runtimeVersion: version, abiVersion,
    component, platform, arch, ...(entry.architectures ? { architectures: entry.architectures } : {}),
    releaseTag: entry.releaseTag, fileName: entry.fileName, size: entry.size, sha256: entry.sha256, source };
  const receiptPath = `${path}.receipt.json`;
  const temporaryReceipt = `${receiptPath}.${randomUUID()}.tmp`;
  try { await writeFile(temporaryReceipt, JSON.stringify(receipt, null, 2) + '\n', { flag: 'wx' }); await rename(temporaryReceipt, receiptPath); }
  finally { await rm(temporaryReceipt, { force: true }); }
  let installation;
  if (installDirectory) {
    try {
      const result = await promisify(execFile)(python, [fileURLToPath(new URL('./unpack-runtime.py', import.meta.url)),
        '--archive', path, '--destination', resolve(installDirectory), '--sha256', entry.sha256,
        '--identity-json', JSON.stringify({ runtimeVersion: version, abiVersion, component, platform, arch })], { maxBuffer: 1024 ** 2 });
      installation = JSON.parse(result.stdout);
    } catch { fail('ERR_RUNTIME_INSTALL', 'Runtime installation failed; existing installation was preserved. Validate the archive layout, destination and Python 3 availability.'); }
  }
  return { path, receiptPath, receipt, ...(installation ? { installDirectory: installation.destination, installation } : {}) };
}

function parseArguments(args) {
  const values = {}; const allowed = ['manifest', 'version', 'platform', 'arch', 'component', 'abi', 'out-dir', 'artifact-dir', 'install-dir', 'timeout-ms'];
  for (let index = 0; index < args.length; index += 2) {
    const key = args[index]?.slice(2);
    if (!args[index]?.startsWith('--') || !allowed.includes(key) || values[key] !== undefined || !args[index + 1] || args[index + 1].startsWith('--')) fail('ERR_RUNTIME_OPTIONS', 'Unknown, duplicated or missing runtime artifact argument.');
    values[key] = args[index + 1];
  }
  if (values.abi !== undefined && !/^[1-9]\d*$/.test(values.abi)) fail('ERR_RUNTIME_OPTIONS', '--abi must be a positive integer.');
  return { manifestPath: values.manifest, version: values.version ?? SDK_VERSION, platform: values.platform, arch: values.arch,
    component: values.component ?? 'runtime', abiVersion: values.abi === undefined ? 1 : Number(values.abi),
    outDirectory: values['out-dir'], artifactDirectory: values['artifact-dir'] ?? process.env.WEAPP_CONTAINER_RUNTIME_ARTIFACT_DIR,
    installDirectory: values['install-dir'], ...(values['timeout-ms'] === undefined ? {} : { timeoutMs: timeoutValue(values['timeout-ms'], true) }) };
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try { process.stdout.write(JSON.stringify(await downloadRuntimeArtifact(parseArguments(process.argv.slice(2))), null, 2) + '\n'); }
  catch (error) { process.stderr.write(`${error.code?.startsWith('ERR_RUNTIME_') ? error.code : 'ERR_RUNTIME_IO'}: ${error.code?.startsWith('ERR_RUNTIME_') ? error.message : 'Cannot read or write the explicitly selected runtime artifact paths.'}\n`); process.exitCode = 1; }
}
