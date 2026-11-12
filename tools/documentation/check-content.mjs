#!/usr/bin/env node
import {existsSync, readFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {fileURLToPath} from 'node:url';

export function codeBlocks(text) {
  return [...text.matchAll(/^\s*(`{3,}|~{3,})([^\n]*)\n([\s\S]*?)^\s*\1\s*$/gm)].map(match => ({language: match[2].trim(), code: match[3]}));
}
export function malformedCodeSpans(text) {
  const body = text.replace(/^\s*(`{3,}|~{3,})[^\n]*\n[\s\S]*?^\s*\1\s*$/gm, '');
  return body.split(/\n\s*\n/).filter(paragraph => {
    const runs = [...paragraph.matchAll(/(?<![\\`])`+/g)].map(match => match[0].length);
    for (let index = 0; index < runs.length; index++) {
      const closing = runs.indexOf(runs[index], index + 1);
      if (closing < 0) return true;
      index = closing;
    }
    return false;
  });
}
export function structure(text) {
  const prose = text.replace(/^---\n[\s\S]*?\n---\n/, '').replace(/^\s*(`{3,}|~{3,})[^\n]*\n[\s\S]*?^\s*\1\s*$/gm, '');
  return {
    headings: [...prose.matchAll(/^(#{1,6})\s+/gm)].map(match => match[1].length),
    tableColumns: prose.split('\n').filter(line => /^\s*\|.*\|\s*$/.test(line)).map(line => line.replace(/`[^`]*`/g, '').replace(/\\\|/g, '').split('|').length - 2),
  };
}
export function prose(text) {
  return text.replace(/^---\n[\s\S]*?\n---\n/, '')
    .replace(/^\s*(`{3,}|~{3,})[^\n]*\n[\s\S]*?^\s*\1\s*$/gm, '')
    .replace(/`[^`\n]+`|<[^>]+>/g, '')
    .replace(/!?\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/^.*(?:简体中文|中文).*English.*$/gm, '');
}
export function hasChineseDefault(text) {
  const body = prose(text);
  // Short package/changelog entries may consist solely of product names.
  // A substantial English body is never a Chinese translation.
  return /[\u3400-\u9fff]/u.test(body) || (body.match(/[A-Za-z]+/g) || []).length < 40;
}
export function untranslatedEnglishParagraphs(text) {
  // Product names and field lists can legitimately remain English. Complete
  // sentences with English grammar belong in the English counterpart.
  return prose(text).split(/\n\s*\n/).filter(paragraph => {
    if (/[\u3400-\u9fff]/u.test(paragraph)) return false;
    const words = paragraph.match(/[A-Za-z]+/g) || [];
    const grammar = words.filter(word => /^(?:the|a|an|is|are|was|were|with|without|from|for|of|to|and|or|must|should|does|not|when|before|after|this|that|it|its)$/i.test(word));
    return words.length >= 18 && grammar.length >= 3;
  });
}
export function checkContent(root) {
  const catalog = JSON.parse(readFileSync(resolve(root, 'documentation.json'), 'utf8'));
  const findings = [];
  for (const page of catalog.pages) {
    if (![page.zh, page.en].every(path => existsSync(resolve(root, path)))) {findings.push({path: page.en, reason: 'Missing bilingual content'}); continue;}
    const zh = readFileSync(resolve(root, page.zh), 'utf8'), en = readFileSync(resolve(root, page.en), 'utf8');
    for (const [path, text] of [[page.zh, zh], [page.en, en]]) if (malformedCodeSpans(text).length) findings.push({path, reason: 'Unbalanced inline code delimiters'});
    if (JSON.stringify(codeBlocks(zh)) !== JSON.stringify(codeBlocks(en))) findings.push({path: page.en, reason: 'Executable examples differ between languages'});
    if (JSON.stringify(structure(zh)) !== JSON.stringify(structure(en))) findings.push({path: page.en, reason: 'Heading or table structure differs between languages'});
    if (/[\u3400-\u9fff]/u.test(prose(en))) findings.push({path: page.en, reason: 'Untranslated Chinese prose remains outside examples and identifiers'});
    if (!hasChineseDefault(zh)) findings.push({path: page.zh, reason: 'The default document still has an English body'});
    if (untranslatedEnglishParagraphs(zh).length) findings.push({path: page.zh, reason: 'Untranslated English paragraphs remain in the Chinese document'});
  }
  return {pages: catalog.pages.length, findings};
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const report = checkContent(process.cwd());
  console.log(JSON.stringify(report, null, 2));
  if (report.findings.length) process.exitCode = 1;
}
