#!/usr/bin/env node
// Run after reviewing the English page against its current Chinese source.
import {readFileSync, writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {digest} from './check.mjs';

const args = process.argv.slice(2);
if (args.length !== 4 || args[0] !== '--page' || args[2] !== '--chinese-summary' || !args[3].trim()) throw new Error('Usage: node tools/documentation/review.mjs --page docs/file.md --chinese-summary "已复核的中文摘要"');
const root = process.cwd(), path = resolve(root, 'documentation.json');
const catalog = JSON.parse(readFileSync(path, 'utf8'));
const page = catalog.pages.find(page => page.zh === args[1]);
if (!page) throw new Error('Classify the document pair before recording a review');
page.reviewedChineseSha256 = digest(readFileSync(resolve(root, page.zh)));
page.reviewedChineseSummary = args[3];
writeFileSync(path, JSON.stringify(catalog, null, 2) + '\n');
console.log(`Recorded translation review: ${page.en}`);
