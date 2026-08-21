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
  'toast-region'
]) {
  assert.match(html, new RegExp(`id=["']${id}["']`), `missing #${id}`);
}

assert.match(html, /class="workspace"/);
assert.match(html, /grid-template-columns:\s*minmax\(190px,\s*0\.72fr\)\s+minmax\(360px,\s*1\.55fr\)\s+minmax\(280px,\s*1fr\)/);
assert.match(html, /@media\s*\(max-width:\s*900px\)/);
assert.match(html, /class="mobile-tabs"[^>]*role="tablist"/);
assert.match(html, /aria-live="polite"/);
assert.match(html, /id="app-logic"/);
assert.match(html, /localStorage\.setItem\(DRAFT_KEY/);
assert.match(html, /function\s+scheduleDraftSave\s*\(/);
assert.match(html, /function\s+generateFile\s*\(/);
assert.match(html, /function\s+handleJsonImport\s*\(/);
assert.match(html, /function\s+renderAll\s*\(/);
assert.match(html, /event\.ctrlKey\s*&&\s*event\.key\s*===\s*'Enter'/);
assert.match(html, /data-action="add-group"/);
assert.match(html, /data-action="add-item"/);
assert.match(html, /location\.protocol\s*===\s*'file:'/);
assert.doesNotMatch(html, /<(?:script|link)[^>]+(?:src|href)=["']https?:/i);
assert.doesNotMatch(html, /\.innerHTML\s*=/);

const appMatch = html.match(/<script id="app-logic">([\s\S]*?)<\/script>/);
assert.ok(appMatch, 'app-logic script must exist');
assert.doesNotThrow(() => new vm.Script(appMatch[1], { filename: htmlPath }));

console.log('HtmlPasteGen UI contract tests passed');
