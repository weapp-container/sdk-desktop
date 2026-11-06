#!/usr/bin/env node
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { inside, sha256 } from './native-input.mjs';
import { verifySourceInventory } from './source-inventory.mjs';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const reviewed = verifySourceInventory(root);
const origin = JSON.parse(readFileSync(resolve(root, 'ORIGIN.json'), 'utf8'));
if (sha256(inside(root, origin.contract.headerPath)) !== origin.contract.headerSha256) throw new Error('Contract header hash differs');
console.log(JSON.stringify({ status: 'passed', sdkVersion: reviewed.sdkVersion, sourceInventorySha256: reviewed.inventorySha256, originSourceCommit: origin.sourceCommit, checkedFiles: reviewed.files.length }));
