'use strict';

const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

const htmlPath = 'html/HtmlPasteGen.html';
const html = fs.readFileSync(htmlPath, 'utf8');

for (const id of [
  'back-link',
  'page-title',
  'output-filename',
  'generate-button',
  'group-list',
  'editor-panel',
  'preview-panel',
  'import-json-input',
  'import-json-button',
  'export-json-button',
  'new-project-button',
  'restore-sample-button',
  'draft-status',
  'validation-summary',
  'toast-region',
  'network-library-panel',
  'network-refresh-button',
  'network-search',
  'network-type-filter',
  'network-file-list',
  'network-preview',
  'network-import-json-button',
  'network-export-json-button',
  'network-overwrite-checkbox',
  'clear-content-button'
]) {
  assert.match(html, new RegExp(`id=["']${id}["']`), `missing #${id}`);
}

assert.match(html, /class="workspace"/);
assert.match(html, /grid-template-columns:\s*minmax\(170px,\s*\.64fr\)\s+minmax\(480px,\s*1\.75fr\)\s+minmax\(300px,\s*1\.05fr\)/);
assert.match(html, /edit-meta-fields/);
assert.match(html, /preview-command-toggle/);
assert.match(html, /显示全部命令/);
assert.match(html, /function\s+togglePreviewCommand\s*\(/);
assert.match(html, /expandedPreviewIds/);
assert.match(html, /min-height:\s*150px/);
assert.match(html, /@media\s*\(max-width:\s*900px\)/);
assert.match(html, /class="mobile-tabs"[^>]*role="tablist"/);
assert.match(html, /aria-live="polite"/);
assert.match(html, /id="app-logic"/);
assert.match(html, /localStorage\.setItem\(DRAFT_KEY/);
assert.match(html, /function\s+scheduleDraftSave\s*\(/);
assert.match(html, /function\s+generateFile\s*\(/);
assert.match(html, /function\s+handleJsonImport\s*\(/);
assert.match(html, /function\s+loadNetworkLibrary\s*\(/);
assert.match(html, /function\s+importNetworkJson\s*\(/);
assert.match(html, /function\s+saveNetworkJson\s*\(/);
assert.match(html, /function\s+clearCurrentContent\s*\(/);
assert.match(html, /页面名称和输出文件名会保留/);
assert.match(html, /function\s+renderNetworkLibrary\s*\(/);
assert.match(html, /function\s+previewNetworkHtml\s*\(/);
assert.match(html, /function\s+renderAll\s*\(/);
assert.match(html, /event\.ctrlKey\s*&&\s*event\.key\s*===\s*'Enter'/);
assert.match(html, /data-action="add-group"/);
assert.match(html, /data-action="add-item"/);
assert.match(html, /generated-import-json-button/);
assert.match(html, /generated-export-json-button/);
assert.match(html, /generated-import-json-input/);
assert.match(html, /location\.protocol\s*===\s*'file:'/);
assert.doesNotMatch(html, /<(?:script|link)[^>]+(?:src|href)=["']https?:/i);
assert.doesNotMatch(html, /\.innerHTML\s*=/);
assert.match(html, /preview-command-clamped/);
assert.match(html, /preview-command-toggle/);
assert.match(html, /event\.stopPropagation\(\)/);
assert.match(html, /fullContent\.length\s*>\s*160/);

const appMatch = html.match(/<script id="app-logic">([\s\S]*?)<\/script>/);
assert.ok(appMatch, 'app-logic script must exist');
assert.doesNotThrow(() => new vm.Script(appMatch[1], { filename: htmlPath }));

console.log('HtmlPasteGen UI contract tests passed');
