#!/usr/bin/env node
import { lstatSync, readFileSync, readdirSync } from 'node:fs';
import { dirname, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { inside, sha256 } from './native-input.mjs';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const origin = JSON.parse(readFileSync(resolve(root, 'ORIGIN.json'), 'utf8'));
let count = 0;
const expected = new Set(['ORIGIN.json']);
for (const entry of [...origin.sources, ...origin.generated]) {
  if (sha256(inside(root, entry.destinationPath)) !== entry.destinationSha256) throw new Error(`Source differs from the reviewed extraction: ${entry.destinationPath}`);
  if (expected.has(entry.destinationPath)) throw new Error(`Duplicate source mapping: ${entry.destinationPath}`);
  expected.add(entry.destinationPath);
  count++;
}
if (sha256(inside(root, origin.contract.headerPath)) !== origin.contract.headerSha256) throw new Error('Contract header hash differs');
const ignoredDirectories = new Set(['.git', '.runtime', 'artifacts', '.gradle', '.cxx', 'build', 'Frameworks', '.build', '.swiftpm', 'DerivedData', 'Pods']);
const visit = directory => {
  for (const name of readdirSync(directory)) {
    const path = resolve(directory, name), stat = lstatSync(path);
    if (stat.isSymbolicLink()) throw new Error(`Source symlink is forbidden: ${path}`);
    if (stat.isDirectory()) { if (!ignoredDirectories.has(name)) visit(path); continue; }
    const local = relative(root, path).replaceAll('\\', '/');
    if (!stat.isFile() || !expected.has(local)) throw new Error(`Unreviewed source path: ${local}`);
    if (/\.(so|a|lib|dll|dylib|aar|apk|ipa|wasm)$/i.test(name)) throw new Error(`Binary artifact is forbidden in SDK source: ${local}`);
  }
};
visit(root);
console.log(JSON.stringify({ status: 'passed', sdkVersion: origin.sdkVersion, sourceCommit: origin.sourceCommit, checkedFiles: count }));
