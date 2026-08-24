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
  'quick-actions-bar',
  'floating-add-item-button',
  'floating-add-group-button',
  'floating-editor-button',
  'floating-context',
  'network-library-toolbar',
  'network-library-panel',
  'network-refresh-button',
  'network-new-json-button',
  'network-search',
  'network-type-filter',
  'network-file-list',
  'network-network-editor',
  'network-draft-name',
  'network-draft-content',
  'network-draft-save',
  'network-draft-cancel',
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
assert.match(html, /function\s+startNetworkJsonCreate\s*\(/);
assert.match(html, /function\s+editNetworkJson\s*\(/);
assert.match(html, /function\s+saveNetworkDraft\s*\(/);
assert.match(html, /function\s+deleteNetworkJson\s*\(/);
assert.match(html, /\/api\/html-paste\/delete\?name=/);
assert.match(html, /method:\s*['"]DELETE['"]/);
assert.match(html, /encodeURIComponent\(file\.name\)/);
assert.match(html, /networkBusy/);
assert.match(html, /response\.status === 409/);
assert.match(html, /state\.networkFiles/);
assert.match(html, /校验并保存/);
assert.match(html, /删除网络库文件/);
assert.match(html, /当前编辑草稿未改变/);
assert.match(html, /快速链接/);
assert.match(html, /dataset\.field\s*=\s*["']link["']/);
assert.match(html, /function\s+validateQuickLink\s*\(/);
assert.match(html, /function\s+clearCurrentContent\s*\(/);
assert.match(html, /function\s+updateQuickActions\s*\(/);
assert.match(html, /quick-actions/);
assert.match(html, /position:\s*fixed/);
assert.match(html, /页面名称和输出文件名会保留/);
assert.match(html, /function\s+renderNetworkLibrary\s*\(/);
assert.match(html, /function\s+previewNetworkHtml\s*\(/);
assert.match(html, /id=["']network-overwrite-checkbox["'][^>]*checked/);
assert.match(html, /class=["'][^"']*network-library-toolbar[^"']*["']/);
assert.match(html, /window\.open\(\s*networkFileUrl\(/);
assert.doesNotMatch(html, /window\.open\(\s*['"]about:blank['"]/);
assert.match(html, /network-preview[\s\S]*addEventListener\(['"]error['"]/);
assert.match(html, /class=["']generated-toolbar["']/);
assert.match(html, /class=["']generated-toolbar-slot["']/);
assert.match(html, /generated-toolbar[\s\S]*position:\s*fixed/);
assert.match(html, /generated-toolbar[\s\S]*top:\s*0/);
assert.match(html, /group-select-button/);
assert.match(html, /function\s+markCopiedCards\s*\(/);
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
