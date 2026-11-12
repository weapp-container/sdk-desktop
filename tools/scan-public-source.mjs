#!/usr/bin/env node
// The report intentionally never contains matched text or credential values.
import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { chmodSync, copyFileSync, existsSync, lstatSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gunzipSync } from 'node:zlib';

export const SCANNER_VERSION = '8.30.1';
const hash = value => createHash('sha256').update(value).digest('hex');
const localPath = /(?<![\w/])(?:\/Users\/|\/home\/)[A-Za-z0-9_.<>-]+(?:\/[^\s"'`<>]*)?/g;
// The reviewed compressed conformance fixture expands to 17 MiB.
const limit = 32 * 1024 * 1024;
const run = (root, args, options = {}) => execFileSync('git', args, { cwd: root, maxBuffer: 128 * 1024 * 1024, ...options });

export function reviewSecretFindings(findings, policy) {
  return findings.map(item => {
    const path = item.File.replaceAll('\\', '/');
    const sha256 = hash(item.Secret);
    const approval = policy.allowedSecrets?.find(row => row.path === path && row.rule === item.RuleID && row.sha256 === sha256 && row.reason);
    return { path, line: item.StartLine, rule: item.RuleID, ...(item.Commit ? { commit: item.Commit } : {}),
      status: approval ? 'reviewed-fixture' : 'unreviewed', ...(approval ? { reason: approval.reason } : {}) };
  });
}

export function metadataFindings(path, input, policy) {
  if (/(?:^|\/)\.env(?:\.[^/]+)?$/.test(path) && !path.endsWith('.env.example') || /\.(?:p12|pfx|jks|keystore|mobileprovision)$/i.test(path)) return [{ path, rule: 'private-state-or-signing-file', status: 'unreviewed' }];
  if (/^docs\/(?:.*\/)?(?:evidence|[^/]+-evidence|sdk-migration|history)\//.test(path)) return [{ path, rule: 'historical-evidence-tree', status: 'unreviewed' }];
  let bytes = input;
  if (path.endsWith('.gz')) {
    try { bytes = gunzipSync(bytes, { maxOutputLength: limit }); } catch { return [{ path, rule: 'unreadable-archive', status: 'unreviewed' }]; }
  }
  const text = bytes.toString('utf8');
  if (text.startsWith('version https://git-lfs.github.com/spec/v1\n')) return [{ path, rule: 'unreviewed-lfs-payload', status: 'unreviewed' }];
  const matches = [...text.matchAll(localPath)];
  return matches.map(match => {
    const approval = policy.allowedLocalPaths?.find(row => row.path === path && row.sha256 === hash(match[0]) && row.reason);
    return { path, rule: 'local-user-path', status: approval ? 'reviewed-fixture' : 'unreviewed',
      ...(approval ? { reason: approval.reason } : {}) };
  });
}

export function scanRepository({ root, policyPath = resolve(root, '.public-source-policy.json'), scanner, mode = 'history' }) {
  if (!['history', 'source'].includes(mode)) throw Error('Choose history or source mode');
  const policy = JSON.parse(readFileSync(policyPath, 'utf8'));
  if (policy.schemaVersion !== 1) throw Error('Unsupported public-source policy');
  if (execFileSync(scanner, ['version'], { encoding: 'utf8' }).trim() !== SCANNER_VERSION) throw Error('Use the reviewed Gitleaks version');
  const directory = mkdtempSync(resolve(tmpdir(), 'weapp-source-scan-'));
  chmodSync(directory, 0o700);
  try {
    // No repository-provided gitleaks ignore/config can weaken this audit.
    const config = resolve(directory, 'default.toml');
    writeFileSync(config, '[extend]\nuseDefault = true\n', { mode: 0o600 });
    const reportPath = resolve(directory, 'private-report.json');
    let scanRoot = root;
    if (mode === 'source') {
      scanRoot = resolve(directory, 'source'); mkdirSync(scanRoot);
      const paths = run(root, ['ls-files', '-z', '--cached', '--others', '--exclude-standard'], { encoding: 'utf8' }).split('\0').filter(Boolean);
      for (const path of new Set(paths)) {
        const source = resolve(root, path);
        if (!existsSync(source)) continue;
        if (lstatSync(source).isSymbolicLink()) throw Error('Source scan requires explicit review of symbolic links');
        if (!lstatSync(source).isFile()) continue;
        const target = resolve(scanRoot, path); mkdirSync(dirname(target), { recursive: true }); copyFileSync(source, target);
      }
    }
    const args = [mode === 'history' ? 'git' : 'dir', scanRoot,
      ...(mode === 'history' ? ['--log-opts=--all --full-history --diff-merges=first-parent'] : []),
      '--config', config, '--ignore-gitleaks-allow', '--gitleaks-ignore-path', resolve(directory, 'no-ignore'),
      '--max-decode-depth=2', '--max-archive-depth=2', '--no-banner', '--no-color', '--redact=0',
      '--report-format=json', '--report-path', reportPath];
    // Raw output stays in memory/private temporary storage and is never logged.
    const result = spawnSync(scanner, args, { cwd: root, maxBuffer: 16 * 1024 * 1024 });
    if (![0, 1].includes(result.status)) throw Error(`Gitleaks failed (exit ${result.status})`);
    chmodSync(reportPath, 0o600);
    const raw = JSON.parse(readFileSync(reportPath, 'utf8'));
    const findings = reviewSecretFindings(raw.map(item => ({ ...item, File: item.File.startsWith(scanRoot + sep) ? relative(scanRoot, item.File) : item.File })), policy);
    let checked = 0;
    if (mode === 'history') {
      const objects = run(root, ['rev-list', '--objects', '--all'], { encoding: 'utf8' }).split('\n');
      const input = objects.map(row => row.split(' ', 1)[0]).filter(Boolean).join('\n') + '\n';
      const metadata = run(root, ['cat-file', '--batch-check=%(objectname) %(objecttype) %(objectsize)'], { input, encoding: 'utf8' }).trim().split('\n');
      for (let i = 0; i < objects.length && i < metadata.length; i++) {
        const [oid, type, size] = metadata[i].split(' ');
        const path = objects[i].slice(oid.length + 1);
        if (type !== 'blob') continue;
        if (+size > limit) { findings.push({ path, rule: 'oversize-source', status: 'unreviewed' }); continue; }
        findings.push(...metadataFindings(path, run(root, ['cat-file', 'blob', oid]), policy));
        checked++;
      }
    } else {
      const paths = run(root, ['ls-files', '-z', '--cached', '--others', '--exclude-standard'], { encoding: 'utf8' }).split('\0').filter(Boolean);
      for (const path of new Set(paths)) {
        const file = resolve(root, path);
        if (!existsSync(file) || !lstatSync(file).isFile()) continue;
        if (lstatSync(file).size > limit) { findings.push({ path, rule: 'oversize-source', status: 'unreviewed' }); continue; }
        findings.push(...metadataFindings(path, readFileSync(file), policy));
        checked++;
      }
    }
    return { schemaVersion: 1, scanner: `gitleaks-${SCANNER_VERSION}`, mode, checked,
      passed: findings.every(row => row.status === 'reviewed-fixture'), findings };
  } finally { rmSync(directory, { recursive: true, force: true }); }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2), options = {};
  for (let i = 0; i < args.length; i += 2) {
    if (!['--root', '--policy', '--scanner', '--mode', '--report'].includes(args[i]) || !args[i + 1] || options[args[i]]) throw Error('Unknown, duplicate or missing scan option');
    options[args[i]] = args[i + 1];
  }
  const root = resolve(options['--root'] ?? fileURLToPath(new URL('..', import.meta.url)));
  const report = scanRepository({ root, policyPath: options['--policy'], scanner: options['--scanner'] ?? process.env.GITLEAKS_BIN ?? 'gitleaks', mode: options['--mode'] ?? 'history' });
  if (options['--report']) writeFileSync(resolve(options['--report']), JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify({ passed: report.passed, mode: report.mode, checked: report.checked,
    reviewed: report.findings.filter(row => row.status === 'reviewed-fixture').length,
    unreviewed: report.findings.filter(row => row.status === 'unreviewed') }, null, 2));
  if (!report.passed) process.exitCode = 1;
}
