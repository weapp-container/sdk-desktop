#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cpSync, mkdirSync, mkdtempSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { copyPack } from './native-input.mjs';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
function option(name, fallback) {
  const index = args.indexOf(name);
  if (index < 0) return fallback;
  if (!args[index + 1] || args[index + 1].startsWith('--')) throw new Error(`${name} requires a value`);
  return args[index + 1];
}
const source = option('--sdk');
if (!source) throw new Error('--sdk <installed desktop SDK> is required');
const architecture = option('--arch', process.arch);
if (!['arm64', 'x64'].includes(architecture)) throw new Error('--arch must be arm64 or x64');
if (process.platform !== 'darwin' && architecture !== process.arch) throw new Error('Non-Apple consumers must execute the native host architecture');
const validation = resolve(root, 'artifacts/validation'); mkdirSync(validation, { recursive: true });
const work = mkdtempSync(resolve(validation, `desktop-${process.platform}-${architecture}-`));
const staged = resolve(work, 'sdk-original'), installed = resolve(work, 'sdk-relocated');
const report = { schemaVersion: 1, sdkVersion: '0.1.0-preview.5', status: 'running', architecture, execution: process.platform === 'darwin' && process.arch !== architecture ? 'Rosetta x86_64' : `${process.platform}-${process.arch}`, checks: [] };
const save = () => writeFileSync(resolve(work, 'validation.json'), `${JSON.stringify(report, null, 2)}\n`);
function run(label, command, argv) {
  const result = spawnSync(command, argv, { cwd: work, encoding: 'utf8', maxBuffer: 32 * 1024 * 1024, timeout: 120_000 });
  const log = `${report.checks.length + 1}-${label}.log`;
  writeFileSync(resolve(work, log), `${result.stdout || ''}${result.stderr || ''}`);
  report.checks.push({ label, exitCode: result.status, signal: result.signal, error: result.error?.message, log }); save();
  if (result.error || result.status !== 0) throw new Error(`${label} failed; see ${resolve(work, log)}`);
}
try {
  const receipt = JSON.parse(readFileSync(resolve(source, 'package.json'), 'utf8'));
  if (!['verified', 'packaged'].includes(receipt.status) || receipt.sdkVersion !== report.sdkVersion) throw new Error('A successfully packaged SDK receipt is required');
  for (const item of receipt.artifacts) {
    if (createHash('sha256').update(readFileSync(resolve(source, item.file))).digest('hex') !== item.sha256) throw new Error(`Distribution checksum mismatch: ${item.file}`);
  }
  report.sourceCommit = receipt.sourceCommit; report.artifacts = receipt.artifacts;
  cpSync(resolve(source), staged, { recursive: true }); renameSync(staged, installed);
  report.packIntegrity = copyPack(option('--pack'), resolve(work, 'portable-pack'));
  const build = resolve(work, 'build');
  const apple = process.platform === 'darwin' ? [`-DCMAKE_OSX_ARCHITECTURES=${architecture === 'x64' ? 'x86_64' : 'arm64'}`, '-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0'] : [];
  run('configure', 'cmake', ['-S', resolve(installed, 'share/weapp-container/consumer'), '-B', build, `-DCMAKE_PREFIX_PATH=${installed}`, '-DCMAKE_BUILD_TYPE=Release', ...apple]);
  run('build-without-rust', 'cmake', ['--build', build, '--config', 'Release', '--parallel', '4']);
  const binary = resolve(build, process.platform === 'win32' ? 'Release/desktop_consumer.exe' : 'desktop_consumer');
  const input = [resolve(work, 'portable-pack'), resolve(work, 'consumer-result.txt')];
  if (process.platform === 'darwin' && architecture === 'x64') run('execute', 'arch', ['-x86_64', binary, ...input]);
  else run('execute', binary, input);
  report.status = 'passed';
} catch (error) { report.status = 'failed'; report.error = error.message; process.exitCode = 1; }
report.finishedAt = new Date().toISOString(); save();
console.log(JSON.stringify({ status: report.status, report: resolve(work, 'validation.json'), error: report.error }, null, 2));
