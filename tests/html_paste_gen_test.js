'use strict';

const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

const htmlPath = 'html/HtmlPasteGen.html';
assert.ok(fs.existsSync(htmlPath), 'html/HtmlPasteGen.html must exist');

const html = fs.readFileSync(htmlPath, 'utf8');
const match = html.match(/<script id="core-logic">([\s\S]*?)<\/script>/);
assert.ok(match, 'core-logic script must exist');

let idCounter = 0;
const context = {
  window: {},
  crypto: { randomUUID: () => `test-${++idCounter}` }
};
context.window.window = context.window;
vm.createContext(context);
vm.runInContext(match[1], context, { filename: htmlPath });

const core = context.window.HtmlPasteGenCore;
assert.ok(core, 'window.HtmlPasteGenCore must be exported');

assert.strictEqual(core.safeHtmlFilename('  我的工具  '), '我的工具.html');
assert.strictEqual(core.safeHtmlFilename('a:b/c*?'), 'a-b-c-.html');
assert.strictEqual(core.safeHtmlFilename('report.htm'), 'report.html');
assert.strictEqual(core.safeHtmlFilename('...'), 'quick-copy.html');
assert.strictEqual(core.safeHtmlFilename(''), 'quick-copy.html');

assert.strictEqual(core.normalizeShortcut(' ctrl + shift + a '), 'Ctrl+Shift+A');
assert.strictEqual(core.normalizeShortcut('option + 1'), 'Alt+1');
assert.strictEqual(core.normalizeShortcut('cmd+k'), 'Meta+K');
assert.strictEqual(core.validateShortcut('').valid, true);
assert.strictEqual(core.validateShortcut('Ctrl').valid, false);
assert.strictEqual(core.validateShortcut('Ctrl+Shift+A').valid, true);
assert.strictEqual(core.validateShortcut('Ctrl+L').valid, false);
assert.strictEqual(core.shortcutFromEvent({
  ctrlKey: true,
  altKey: false,
  shiftKey: true,
  metaKey: false,
  key: 'a'
}), 'Ctrl+Shift+A');

const sample = core.createSampleDocument();
assert.strictEqual(sample.schemaVersion, 1);
assert.ok(sample.documentId);
assert.strictEqual(sample.groups.length, 2);
assert.ok(sample.groups.every(group => group.id && Array.isArray(group.items)));
assert.ok(sample.groups.flatMap(group => group.items).every(item => item.id));

const validation = core.validateDocument(sample);
assert.strictEqual(validation.valid, true);
assert.deepStrictEqual(Array.from(validation.errors), []);
assert.strictEqual(core.validateDocument({ schemaVersion: 99, groups: [] }).valid, false);

const duplicateShortcut = core.cloneDocument(sample);
duplicateShortcut.groups[1].items[0].shortcut = 'alt + 1';
const duplicateValidation = core.validateDocument(duplicateShortcut);
assert.strictEqual(duplicateValidation.valid, false);
assert.match(Array.from(duplicateValidation.errors).join('\n'), /Alt\+1.*重复/);

const missingContent = core.cloneDocument(sample);
missingContent.groups[0].items[0].content = '';
const warningValidation = core.validateDocument(missingContent);
assert.strictEqual(warningValidation.valid, true);
assert.match(Array.from(warningValidation.warnings).join('\n'), /复制内容为空/);

const normalized = core.normalizeDocument({
  schemaVersion: 1,
  documentId: 'doc-imported',
  meta: { title: '导入内容', filename: 'imported' },
  groups: [{ id: 'g1', title: '组', items: [{ id: 'i1', title: '条目', content: '正文' }] }]
});
assert.strictEqual(normalized.meta.themePreference, 'system');
assert.strictEqual(normalized.meta.filename, 'imported.html');
assert.strictEqual(normalized.groups[0].collapsed, false);
assert.strictEqual(normalized.groups[0].items[0].favorite, false);
assert.strictEqual(normalized.groups[0].items[0].shortcut, '');

const moved = ['a', 'b', 'c'];
assert.strictEqual(core.moveItem(moved, 0, 1), true);
assert.deepStrictEqual(moved, ['b', 'a', 'c']);
assert.strictEqual(core.moveItem(moved, 0, -1), false);

const batchDoc = core.createSampleDocument();
const batchItems = batchDoc.groups.flatMap(group => group.items);
batchItems[1].content = '';
const batchOrder = [batchItems[2].id, batchItems[0].id, batchItems[1].id, 'missing-id'];
assert.strictEqual(core.resolveBatchSeparator('newline', 'ignored'), '\n');
assert.strictEqual(core.resolveBatchSeparator('blankLine', 'ignored'), '\n\n');
assert.strictEqual(core.resolveBatchSeparator('space', 'ignored'), ' ');
assert.strictEqual(core.resolveBatchSeparator('custom', ' / '), ' / ');
const batchResult = core.composeBatchText(batchDoc, batchOrder, 'newline', '');
assert.strictEqual(batchResult.text, `${batchItems[2].content}\n${batchItems[0].content}`);
assert.strictEqual(batchResult.includedCount, 2);
assert.strictEqual(batchResult.skippedCount, 1);
assert.strictEqual(batchResult.missingCount, 1);
assert.deepStrictEqual(
  Array.from(core.toggleCopyOrder([batchItems[0].id], batchItems[1].id, true)),
  [batchItems[0].id, batchItems[1].id]
);
assert.deepStrictEqual(
  Array.from(core.toggleCopyOrder([batchItems[0].id, batchItems[1].id], batchItems[0].id, false)),
  [batchItems[1].id]
);
assert.deepStrictEqual(
  Array.from(core.moveId([batchItems[0].id, batchItems[1].id, batchItems[2].id], batchItems[2].id, -1)),
  [batchItems[0].id, batchItems[2].id, batchItems[1].id]
);

const dangerous = core.createSampleDocument();
dangerous.meta.title = '</script><img src=x onerror=alert(1)>';
dangerous.groups[0].items[0].content = '<b>only text</b>\u2028line';
const safeJson = core.safeJsonForHtml(dangerous);
assert.ok(!safeJson.includes('</script>'));
assert.ok(!safeJson.includes('\u2028'));
const restoredDangerous = JSON.parse(safeJson);
assert.strictEqual(restoredDangerous.meta.title, dangerous.meta.title);
assert.strictEqual(restoredDangerous.groups[0].items[0].content, dangerous.groups[0].items[0].content);

const searchDoc = core.createSampleDocument();
const byContent = core.filterDocument(searchDoc, 'NGINX', false);
assert.strictEqual(byContent.groups.length, 1);
assert.strictEqual(byContent.groups[0].items.length, 1);
assert.strictEqual(byContent.groups[0].items[0].title, '查看进程');
const byNote = core.filterDocument(searchDoc, '回复模板', false);
assert.strictEqual(byNote.groups.length, 1);
assert.strictEqual(byNote.groups[0].items[0].title, '问题已处理');
const byGroup = core.filterDocument(searchDoc, '常用命令', false);
assert.strictEqual(byGroup.groups[0].items.length, 2);
const favorites = core.filterDocument(searchDoc, '', true);
assert.ok(favorites.groups.flatMap(group => group.items).every(item => item.favorite));

assert.strictEqual(core.matchesShortcut('Ctrl+Shift+A', {
  ctrlKey: true,
  altKey: false,
  shiftKey: true,
  metaKey: false,
  key: 'a'
}), true);
assert.strictEqual(core.matchesShortcut('Alt+1', {
  ctrlKey: false,
  altKey: false,
  shiftKey: false,
  metaKey: false,
  key: '1'
}), false);

const generated = core.buildGeneratedHtml(searchDoc);
assert.match(generated, /^<!DOCTYPE html>/i);
for (const id of [
  'paste-data',
  'generated-app-logic',
  'generated-title',
  'generated-search',
  'generated-groups',
  'generated-items',
  'edit-mode-toggle',
  'generated-editor',
  'reexport-button',
  'restore-original-button',
  'select-visible-button',
  'content-visibility-toggle',
  'batch-tray',
  'batch-order-list',
  'batch-count',
  'batch-separator',
  'custom-separator',
  'batch-copy-button',
  'batch-clear-button'
]) {
  assert.match(generated, new RegExp(`id=["']${id}["']`), `generated page missing #${id}`);
}
assert.match(generated, /导出最新版 HTML/);
assert.match(generated, /恢复文件原始内容/);
assert.match(generated, /选择当前结果/);
assert.match(generated, /一键复制/);
assert.match(generated, /class=["']item-select["']/);
assert.match(generated, /class=["']item-visibility-toggle["']/);
assert.match(generated, /class=["']nav-item-link["']/);
assert.match(generated, /function\s+renderBatchTray\s*\(/);
assert.match(generated, /function\s+toggleSelection\s*\(/);
assert.match(generated, /function\s+toggleItemVisibility\s*\(/);
assert.match(generated, /function\s+copyBatch\s*\(/);
assert.match(generated, /\.draggable\s*=\s*true/);
assert.match(generated, /['"]dragstart['"]/);
assert.match(generated, /['"]drop['"]/);
assert.doesNotMatch(generated, /<(?:script|link)[^>]+(?:src|href)=["']https?:/i);
assert.strictEqual(JSON.parse(core.extractEmbeddedDocument(generated)).documentId, searchDoc.documentId);
const generatedRuntime = generated.match(/<script id="generated-app-logic">([\s\S]*?)<\/script>/);
assert.ok(generatedRuntime, 'generated app script must exist');
assert.doesNotThrow(() => new vm.Script(generatedRuntime[1], { filename: 'generated-quick-copy.html' }));
const storageFlagDeclaration = generatedRuntime[1].indexOf('let storageAvailable = true;');
const storageReadDuringInit = generatedRuntime[1].indexOf('let model = loadOverride()');
assert.ok(
  storageFlagDeclaration >= 0 && storageFlagDeclaration < storageReadDuringInit,
  'storage fallback flag must be initialized before loadOverride can assign it'
);

const updated = core.cloneDocument(searchDoc);
updated.meta.title = '已更新标题';
const regenerated = core.replaceEmbeddedDocument(generated, updated);
assert.strictEqual(JSON.parse(core.extractEmbeddedDocument(regenerated)).meta.title, '已更新标题');
assert.strictEqual((regenerated.match(/id="paste-data"/g) || []).length, 1);

console.log('HtmlPasteGen core model tests passed');
