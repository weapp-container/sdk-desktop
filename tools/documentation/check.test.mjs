import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtempSync, writeFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {digest, headingIds, links, validateCatalog} from './check.mjs';

function fixture(t) {
  const root = mkdtempSync(join(tmpdir(), 'container-docs-'));
  t.after(() => rmSync(root, {recursive: true, force: true}));
  const zh = '# 文档\n\n[English](README.en.md)\n\n## 生命周期\n\n[章节](#生命周期)\n';
  const en = '# Documentation\n\n[简体中文](README.md)\n\n## Lifecycle\n';
  writeFileSync(join(root, 'README.md'), zh);
  writeFileSync(join(root, 'README.en.md'), en);
  const catalog = {schemaVersion: 1, repository: 'example/repository', defaultLocale: 'zh-CN', locales: ['zh-CN', 'en'], pages: [{id: 'readme', zh: 'README.md', en: 'README.en.md', section: 'overview', source: {repository: 'example/repository', path: 'README.md'}, reviewedChineseSummary: '文档与生命周期章节。', reviewedChineseSha256: digest(zh)}], excluded: []};
  return {root, zh, en, catalog, files: new Set(['README.md', 'README.en.md'])};
}
test('paired documents accept Unicode headings and explicit aliases', t => {
  const f = fixture(t);
  assert.deepEqual(validateCatalog(f.root, f.catalog, f.files).findings, []);
  assert.ok(headingIds('# `Session::load`\n<a id="old-heading"></a>').has('sessionload'));
  assert.ok(headingIds('# Heading\n# Heading').has('heading-1'));
  assert.ok(headingIds('<a id="old-heading"></a>').has('old-heading'));
});
test('example links and IDs cannot satisfy real navigation references', () => {
  const text = '# `Session::load`\n\n`<a id="inline">[Example](inline.md)</a>`\n\n```md\n<a id="fenced"></a>\n[Example](fenced.md)\n```\n\n[Actual](real.md)\n';
  const ids = headingIds(text);
  assert.ok(ids.has('sessionload'));
  assert.equal(ids.has('inline'), false);
  assert.equal(ids.has('fenced'), false);
  assert.deepEqual(links(text), ['real.md']);
});
test('missing counterpart and unclassified new documentation fail', t => {
  const f = fixture(t);
  f.files.delete('README.en.md'); f.files.add('new.md');
  const result = validateCatalog(f.root, f.catalog, f.files);
  assert.ok(result.findings.some(x => /missing documentation counterpart/.test(x.reason)));
  assert.ok(result.findings.some(x => /unclassified/.test(x.reason)));
});
test('Chinese changes invalidate the reviewed English identity', t => {
  const f = fixture(t);
  writeFileSync(join(f.root, 'README.md'), f.zh + '\n新行为\n');
  assert.ok(validateCatalog(f.root, f.catalog, f.files).findings.some(x => /stale/.test(x.reason)));
});
test('broken sections and wrong-language document links fail', t => {
  const f = fixture(t);
  f.catalog.pages.push({id: 'other', zh: 'other.md', en: 'other.en.md', section: 'reference', reviewedChineseSha256: digest('[English](other.en.md)')});
  f.files.add('other.md'); f.files.add('other.en.md');
  writeFileSync(join(f.root, 'other.md'), '[English](other.en.md)');
  writeFileSync(join(f.root, 'other.en.md'), '[简体中文](other.md)');
  writeFileSync(join(f.root, 'README.en.md'), f.en + '\n[wrong locale](other.md)\n[broken](#missing)');
  writeFileSync(join(f.root, 'README.md'), f.zh + '\n[错误语言](other.en.md)');
  const result = validateCatalog(f.root, f.catalog, f.files);
  assert.ok(result.findings.some(x => /English navigation/.test(x.reason)));
  assert.ok(result.findings.some(x => /Chinese navigation/.test(x.reason)));
  assert.ok(result.findings.some(x => /missing section/.test(x.reason)));
});
test('excluded immutable fixture identity cannot silently change', t => {
  const f = fixture(t);
  f.catalog.excluded.push({path: 'historical.md', reason: 'Fixed producer input', sha256: digest('original')});
  f.files.add('historical.md'); writeFileSync(join(f.root, 'historical.md'), 'changed');
  assert.ok(validateCatalog(f.root, f.catalog, f.files).findings.some(x => /excluded.*changed/.test(x.reason)));
});
test('source identities, review summaries and contained resource paths are required', t => {
  const f = fixture(t);
  delete f.catalog.pages[0].source;
  f.catalog.pages[0].reviewedChineseSummary = '';
  f.catalog.assets = [{path: '../outside.svg', sha256: 'a'.repeat(64)}];
  const result = validateCatalog(f.root, f.catalog, f.files);
  assert.ok(result.findings.some(x => /source identity/.test(x.reason)));
  assert.ok(result.findings.some(x => /reviewed Chinese summary/.test(x.reason)));
  assert.ok(result.findings.some(x => /documentation resource/.test(x.reason)));
  f.catalog.pages[0].zh = '../outside.md';
  assert.ok(validateCatalog(f.root, f.catalog, f.files).findings.some(x => /invalid documentation path/.test(x.reason)));
});
test('historical heading aliases remain reachable in both languages', t => {
  const f = fixture(t);
  f.catalog.pages[0].legacyHeadingIds = ['previous-title'];
  assert.equal(validateCatalog(f.root, f.catalog, f.files).findings.filter(x => /historical section/.test(x.reason)).length, 2);
  writeFileSync(join(f.root, 'README.md'), f.zh + '\n<a id="previous-title"></a>\n');
  writeFileSync(join(f.root, 'README.en.md'), f.en + '\n<a id="previous-title"></a>\n');
  f.catalog.pages[0].reviewedChineseSha256 = digest(f.zh + '\n<a id="previous-title"></a>\n');
  assert.deepEqual(validateCatalog(f.root, f.catalog, f.files).findings, []);
});
