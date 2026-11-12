#!/usr/bin/env node
// Shared documentation validator v1. Vendored copies are verified by the core repository.
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {existsSync, readFileSync} from 'node:fs';
import {dirname, relative, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

export const digest = value => createHash('sha256').update(value).digest('hex');
export const englishPath = path => path.replace(/\.md$/, '.en.md');
const stripFences = text => text.replace(/^\s*(`{3,}|~{3,})[^\n]*\n[\s\S]*?^\s*\1\s*$/gm, '');
const stripCode = text => stripFences(text).replace(/`([^`\n]+)`/g, '$1');
export function headingIds(text) {
  const prose = stripFences(text).replace(/`+[^`\n]*`+/g, '');
  const ids = new Set([...prose.matchAll(/\b(?:id|name)=["']([^"']+)["']/g)].map(match => match[1]));
  const counts = new Map();
  for (const match of stripCode(text).matchAll(/^#{1,6}\s+(.+?)\s*#*\s*$/gm)) {
    const base = match[1].replace(/<[^>]*>/g, '').replace(/!?\[([^\]]+)\]\([^)]*\)/g, '$1').toLowerCase().replace(/[^\p{L}\p{N}\p{M}_\-\s]/gu, '').replace(/\s/g, '-');
    const count = counts.get(base) || 0;
    counts.set(base, count + 1);
    ids.add(count ? `${base}-${count}` : base);
  }
  return ids;
}
export function links(text) {
  const prose = stripFences(text).replace(/`+[^`\n]*`+/g, '');
  return [
    ...[...prose.matchAll(/!?\[[^\]\n]*\]\(\s*(<[^>\n]+>|[^\s)]+)(?:\s+[^)]*)?\)/g)].map(match => match[1]),
    ...[...prose.matchAll(/^\s*\[[^\]\n]+\]:\s*(<[^>\n]+>|\S+)/gm)].map(match => match[1]),
    ...[...prose.matchAll(/\b(?:href|src)=["']([^"']+)["']/g)].map(match => match[1]),
  ].map(link => link.replace(/^<|>$/g, ''));
}
export function validateCatalog(root, catalog, files) {
  const findings = [];
  const fail = (path, reason) => findings.push({path, reason});
  if (catalog.schemaVersion !== 1 || catalog.defaultLocale !== 'zh-CN' || JSON.stringify(catalog.locales) !== '["zh-CN","en"]') fail('documentation.json', 'unsupported documentation catalog');
  const covered = new Set(), ids = new Set();
  const contained = path => typeof path === 'string' && !path.startsWith('/') && !path.includes('\\') && !path.split('/').some(part => !part || part === '.' || part === '..');
  const resources = new Set();
  for (const asset of catalog.assets || []) {
    if (resources.has(asset.path) || !/\.(?:svg|png|jpe?g|webp|gif)$/i.test(asset.path) || asset.path.startsWith('/') || asset.path.split('/').includes('..')) {fail(asset.path, 'invalid or duplicate documentation resource'); continue;}
    resources.add(asset.path);
    if (!existsSync(resolve(root, asset.path)) || digest(readFileSync(resolve(root, asset.path))) !== asset.sha256) fail(asset.path, 'documentation resource hash differs');
  }
  const routes = new Map((catalog.pages || []).filter(page => page.route).flatMap(page => [[page.route, page.zh], [`/en${page.route}`, page.en]]));
  for (const page of catalog.pages || []) {
    if (![page.zh, page.en].every(path => contained(path) && path.endsWith('.md'))) {fail('documentation.json', 'invalid documentation path'); continue;}
    if (!page.id || ids.has(page.id) || !page.section) fail(page.zh, 'missing or duplicate document identity/classification');
    if (page.source?.repository !== catalog.repository || page.source?.path !== page.zh) fail(page.zh, 'missing or inconsistent source identity');
    if (!page.reviewedChineseSummary?.trim()) fail(page.en, 'missing reviewed Chinese summary');
    ids.add(page.id);
    if (page.en !== englishPath(page.zh)) fail(page.zh, 'English counterpart must use .en.md');
    for (const path of [page.zh, page.en]) {
      if (covered.has(path)) fail(path, 'document classified more than once');
      covered.add(path);
      if (!files.has(path) || !existsSync(resolve(root, path))) fail(path, 'missing documentation counterpart');
    }
    if (!existsSync(resolve(root, page.zh)) || !existsSync(resolve(root, page.en))) continue;
    const chinese = readFileSync(resolve(root, page.zh));
    if (page.reviewedChineseSha256 !== digest(chinese)) fail(page.en, 'English translation is stale; review against the current Chinese document');
    for (const path of [page.zh, page.en]) {
      const ids = headingIds(readFileSync(resolve(root, path), 'utf8'));
      for (const id of page.legacyHeadingIds || []) if (!ids.has(id)) fail(path, `missing historical section alias: ${id}`);
    }
    for (const [path, other, language] of [[page.zh, page.en, 'zh-CN'], [page.en, page.zh, 'en']]) {
      const text = readFileSync(resolve(root, path), 'utf8');
      const counterpart = relative(dirname(path), other).split('\\').join('/');
      if (!links(text).includes(counterpart)) fail(path, 'missing language-switch link');
      for (const link of links(text)) {
        if (/^(?:[a-z][a-z\d+.-]*:|\/\/)/i.test(link)) continue;
        let target, fragment;
        try { const [rawPath, hash] = link.split('#', 2); target = decodeURIComponent(rawPath.split('?', 1)[0]); fragment = hash ? decodeURIComponent(hash) : ''; }
        catch { fail(path, `invalid URL encoding: ${link}`); continue; }
        const route = routes.get(target);
        const targetFile = route ? resolve(root, route) : target ? resolve(root, dirname(path), target) : resolve(root, path);
        // Site routes and immutable GitHub source URLs are checked by the website
        // build against its complete route graph, including generated API pages.
        if (target.startsWith('/') && !route && catalog.repository === 'weapp-container/website') continue;
        if (!existsSync(targetFile)) { fail(path, `missing link target: ${link}`); continue; }
        if (fragment && targetFile.endsWith('.md') && !headingIds(readFileSync(targetFile, 'utf8')).has(fragment)) fail(path, `missing section: ${link}`);
        if (language === 'en' && target.startsWith('/docs/')) fail(path, `English navigation links to Chinese route: ${link}`);
        if (language === 'zh-CN' && target.startsWith('/en/docs/')) fail(path, `Chinese navigation links to English route: ${link}`);
        if (language === 'en' && target.endsWith('.md') && !target.endsWith('.en.md') && link !== counterpart) {
          const local = relative(root, targetFile).split('\\').join('/');
          if ((catalog.pages || []).some(item => item.zh === local)) fail(path, `English navigation links to Chinese document: ${link}`);
        }
        if (language === 'zh-CN' && target.endsWith('.en.md') && link !== counterpart) {
          const local = relative(root, targetFile).split('\\').join('/');
          if ((catalog.pages || []).some(item => item.en === local)) fail(path, `Chinese navigation links to English document: ${link}`);
        }
      }
    }
  }
  for (const item of catalog.excluded || []) {
    if (!contained(item.path)) {fail('documentation.json', 'invalid exclusion path'); continue;}
    if (covered.has(item.path)) fail(item.path, 'document classified more than once');
    if (!item.reason || !item.sha256 || !existsSync(resolve(root, item.path))) { fail(item.path, 'invalid immutable-document exclusion'); continue; }
    covered.add(item.path);
    if (digest(readFileSync(resolve(root, item.path))) !== item.sha256) fail(item.path, 'excluded historical/third-party document changed');
  }
  for (const path of files) if (path.endsWith('.md') && !covered.has(path)) fail(path, 'unclassified document; add both languages or an exact justified exclusion');
  return {schemaVersion: 1, repository: catalog.repository, pages: ids.size, excluded: (catalog.excluded || []).length, findings};
}
export function checkDocumentation(root) {
  const paths = execFileSync('git', ['ls-files', '-z', '--cached', '--others', '--exclude-standard'], {cwd: root, encoding: 'utf8', maxBuffer: 32 * 1024 * 1024}).split('\0').filter(path => path && existsSync(resolve(root, path)));
  const catalog = JSON.parse(readFileSync(resolve(root, 'documentation.json'), 'utf8'));
  // Imported bodies have a separate exact-file manifest. Do not classify them
  // as newly authored translations; verify their provenance and hashes below.
  const imported = catalog.repository === 'weapp-container/website' && existsSync(resolve(root, 'website/src/data/documentation-sources.json'))
    ? JSON.parse(readFileSync(resolve(root, 'website/src/data/documentation-sources.json'), 'utf8')) : null;
  const generated = new Set(imported ? [
    ...imported.pages.flatMap(page => [page.source, page.output]),
    ...imported.sources.map(source => source.catalog),
  ] : []);
  const report = validateCatalog(root, catalog, new Set(paths.filter(path => !generated.has(path))));
  if (imported) {
    try {
      const verification = JSON.parse(execFileSync(process.execPath, ['tools/documentation-snapshot.mjs', '--check'], {cwd: root, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe']}));
      if (!(verification.pages > 0)) throw new Error('Empty documentation snapshot');
    } catch {
      report.findings.push({path: 'website/src/data/documentation-sources.json', reason: 'Imported documentation manifest, source, output or resource verification failed'});
    }
  }
  return report;
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2);
  if (args.length && (args.length !== 2 || args[0] !== '--root')) throw new Error('Usage: node check-docs.mjs [--root /repository]');
  const report = checkDocumentation(resolve(args[1] || process.cwd()));
  console.log(JSON.stringify(report, null, 2));
  if (report.findings.length) process.exitCode = 1;
}
