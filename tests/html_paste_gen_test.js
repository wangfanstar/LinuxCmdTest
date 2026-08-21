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

console.log('HtmlPasteGen core model tests passed');
