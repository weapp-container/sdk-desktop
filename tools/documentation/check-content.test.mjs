import test from 'node:test';
import assert from 'node:assert/strict';
import {codeBlocks, structure, hasChineseDefault, untranslatedEnglishParagraphs, malformedCodeSpans} from './check-content.mjs';

test('examples preserve language, comments, field names and executable bytes', () => {
  assert.deepEqual(codeBlocks('# Heading\n\n```js\nwx.nextTick(() => {}); // 中文示例\n```\n'), [{language: 'js', code: 'wx.nextTick(() => {}); // 中文示例\n'}]);
  assert.notDeepEqual(codeBlocks('```json\n{"enabled":false}\n```'), codeBlocks('```json\n{"enabled":true}\n```'));
});
test('a language switch, code comment or identifier cannot disguise an English default body', () => {
  const body = '**简体中文** · [English](README.en.md)\n\n' + 'The host must close the session before destroying the rendering surface. '.repeat(5);
  assert.equal(hasChineseDefault(body + '\n```js\n// 中文示例\n```\n`中文标识符`'), false);
  assert.equal(hasChineseDefault(body + '\n宿主在销毁绘制表面前必须关闭会话。'), true);
});
test('translations preserve headings and table cells while ignoring examples', () => {
  const original = '# 接入\n## 生命周期\n| 平台 | 状态 |\n| --- | --- |\n| `A | B` | 已核验 |\n```md\n### example\n```';
  const translated = '# Integration\n## Lifecycle\n| Platform | Status |\n| --- | --- |\n| `A | B` | Verified |';
  assert.deepEqual(structure(original), structure(translated));
  assert.notDeepEqual(structure(original), structure(translated.replace('## Lifecycle', 'Lifecycle')));
  assert.notDeepEqual(structure(original), structure(translated.replace('| Verified |', '| Verified | extra |')));
});
test('a translated paragraph cannot hide a second English paragraph', () => {
  const english = 'The host must close the session and wait for the completion event before it releases the current rendering surface.';
  assert.deepEqual(untranslatedEnglishParagraphs('宿主必须等待关闭完成。\n\n' + english), [english]);
  assert.deepEqual(untranslatedEnglishParagraphs('宿主必须等待关闭完成。\n\n```text\n' + english + '\n```'), []);
  assert.deepEqual(untranslatedEnglishParagraphs('Android iOS Flutter React Native C++ Desktop QuickJS Skia Metal OpenGL WebAssembly JSON ABI FFI API SDK HTTP HTTPS'), []);
});

test('inline commands preserve matching delimiters across line wraps', () => {
  assert.deepEqual(malformedCodeSpans('Run `node tool.mjs\n--out report.json` now.\n\nUse ``a`b``.'), []);
  assert.equal(malformedCodeSpans('node tool.mjs\n--out report.json` is the command.').length, 1);
  assert.equal(malformedCodeSpans('Use `runtime.gpuSurface now.').length, 1);
  assert.deepEqual(malformedCodeSpans('```js\nconst tick = "`";\n```'), []);
});
