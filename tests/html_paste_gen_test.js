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
assert.strictEqual(sample.schemaVersion, 2);
assert.ok(sample.documentId);
assert.strictEqual(sample.groups.length, 2);
assert.ok(sample.groups.every(group => group.id && Array.isArray(group.items)));
assert.ok(sample.groups.flatMap(group => group.items).every(item => item.id));
assert.ok(sample.groups.flatMap(group => group.items).every(item => Array.isArray(item.children)));

const treeDocument = {
  schemaVersion: 2,
  documentId: 'tree-doc',
  meta: { title: '树测试', filename: 'tree-test.html' },
  groups: [{
    id: 'tree-group',
    title: '树分组',
    collapsed: false,
    items: [{
      id: 'root', title: '根', content: 'root', shortcut: '', link: '', note: '', favorite: false, collapsed: false,
      children: [{
        id: 'child', title: '子', content: 'child', shortcut: '', link: '', note: '', favorite: false, collapsed: false,
        children: [{
          id: 'grandchild', title: '孙', content: 'grandchild', shortcut: '', link: '', note: '', favorite: false, collapsed: false,
          children: [{ id: 'leaf', title: '叶', content: 'leaf', shortcut: '', link: '', note: '', favorite: false, collapsed: false, children: [] }]
        }]
      }]
    }]
  }]
};
const legacyDocument = {
  schemaVersion: 1,
  documentId: 'legacy-doc',
  meta: { title: '旧版', filename: 'legacy.html' },
  groups: [{
    id: 'legacy-group', title: '旧分组', collapsed: false,
    items: [
      { id: 'legacy-a', title: '旧甲', content: 'a', shortcut: 'Alt+1', note: '备注甲', favorite: true, collapsed: false },
      { id: 'legacy-b', title: '旧乙', content: 'b', shortcut: '', note: '', favorite: false, collapsed: false }
    ]
  }]
};
assert.deepStrictEqual(Array.from(core.flattenItems(treeDocument), entry => entry.item.id), ['root', 'child', 'grandchild', 'leaf']);
assert.deepStrictEqual(Array.from(core.itemPath(treeDocument, 'leaf')), ['树分组', '根', '子', '孙', '叶']);
assert.strictEqual(core.itemDepth(treeDocument, 'root'), 1);
assert.strictEqual(core.itemDepth(treeDocument, 'leaf'), 4);
assert.strictEqual(core.descendantCount(treeDocument.groups[0].items[0]), 3);
assert.strictEqual(core.validateDocument(treeDocument).valid, true);
const indentedTree = core.cloneDocument(treeDocument);
indentedTree.groups[0].items[0].children.push({ id: 'sibling', title: '兄弟', content: 'sibling', shortcut: '', link: '', note: '', favorite: false, collapsed: false, children: [] });
assert.strictEqual(core.indentItem(indentedTree, 'sibling').ok, true);
assert.strictEqual(indentedTree.groups[0].items[0].children.length, 1);
assert.strictEqual(indentedTree.groups[0].items[0].children[0].children[1].id, 'sibling');
assert.strictEqual(core.outdentItem(indentedTree, 'sibling').ok, true);
assert.strictEqual(indentedTree.groups[0].items[0].children[1].id, 'sibling');
const cloneSubtree = core.cloneItemTree(treeDocument.groups[0].items[0]);
assert.notStrictEqual(cloneSubtree.id, 'root');
assert.strictEqual(cloneSubtree.shortcut, '');
assert.strictEqual(cloneSubtree.children[0].shortcut, '');
assert.strictEqual(core.subtreeHeight(treeDocument.groups[0].items[0]), 4);
const copiedGroup = core.cloneGroupTree(treeDocument.groups[0]);
assert.notStrictEqual(copiedGroup.id, treeDocument.groups[0].id);
assert.notStrictEqual(copiedGroup.items[0].id, treeDocument.groups[0].items[0].id);
assert.strictEqual(copiedGroup.items[0].shortcut, '');
const childPasteDocument = core.cloneDocument(treeDocument);
const childPaste = core.pasteItemAsChild(
  childPasteDocument,
  'root',
  treeDocument.groups[0].items[0].children[0].children[0].children[0]
);
assert.strictEqual(childPaste.ok, true);
assert.notStrictEqual(childPaste.item.id, 'leaf');
assert.strictEqual(childPasteDocument.groups[0].items[0].children.at(-1).title, '叶（副本）');
const siblingPasteDocument = core.cloneDocument(treeDocument);
const siblingPaste = core.pasteItemAsSibling(
  siblingPasteDocument,
  'child',
  treeDocument.groups[0].items[0].children[0]
);
assert.strictEqual(siblingPaste.ok, true);
assert.strictEqual(siblingPasteDocument.groups[0].items[0].children.length, 2);
const depthPasteDocument = core.cloneDocument(treeDocument);
const depthBefore = JSON.stringify(depthPasteDocument);
const depthPaste = core.pasteItemAsChild(depthPasteDocument, 'leaf', treeDocument.groups[0].items[0]);
assert.strictEqual(depthPaste.ok, false);
assert.match(depthPaste.reason, /最多 4 级/);
assert.strictEqual(JSON.stringify(depthPasteDocument), depthBefore);
const groupPasteDocument = core.cloneDocument(treeDocument);
const groupPaste = core.pasteGroupAfter(groupPasteDocument, 'tree-group', treeDocument.groups[0]);
assert.strictEqual(groupPaste.ok, true);
assert.strictEqual(groupPasteDocument.groups.length, 2);
assert.notStrictEqual(groupPasteDocument.groups[1].id, 'tree-group');
assert.strictEqual(core.indentItem(treeDocument, 'root').ok, false);
assert.strictEqual(core.indentItem(treeDocument, 'leaf').ok, false);
const migrated = core.migrateDocument(legacyDocument);
assert.strictEqual(migrated.schemaVersion, 2);
assert.deepStrictEqual(migrated.groups[0].items.map(entry => entry.id), ['legacy-a', 'legacy-b']);
assert.deepStrictEqual(Array.from(migrated.groups[0].items, entry => Array.from(entry.children)), [[], []]);
assert.strictEqual(JSON.stringify(core.migrateDocument(treeDocument)), JSON.stringify(treeDocument));
assert.strictEqual(core.buildGeneratedHtml(legacyDocument).startsWith('<!DOCTYPE html>'), true);
assert.throws(() => core.migrateDocument({ schemaVersion: 99, groups: [] }), /不支持的数据版本/);
const missingChildren = core.cloneDocument(treeDocument);
delete missingChildren.groups[0].items[0].children;
assert.strictEqual(core.validateDocument(missingChildren).valid, false);
assert.match(Array.from(core.validateDocument(missingChildren).errors).join('\n'), /树分组 \/ 根.*children/);

const validation = core.validateDocument(sample);
assert.strictEqual(validation.valid, true);
assert.deepStrictEqual(Array.from(validation.errors), []);
assert.strictEqual(core.validateDocument({ schemaVersion: 99, groups: [] }).valid, false);

const duplicateShortcut = core.cloneDocument(sample);
duplicateShortcut.groups[1].items[0].shortcut = 'alt + 1';
const duplicateValidation = core.validateDocument(duplicateShortcut);
assert.strictEqual(duplicateValidation.valid, false);
assert.match(Array.from(duplicateValidation.errors).join('\n'), /Alt\+1.*重复/);

const duplicateItemId = core.cloneDocument(sample);
duplicateItemId.groups[1].items[0].id = duplicateItemId.groups[0].items[0].id;
const duplicateItemIdValidation = core.validateDocument(duplicateItemId);
assert.strictEqual(duplicateItemIdValidation.valid, false);
assert.match(Array.from(duplicateItemIdValidation.errors).join('\n'), /条目 ID.*重复/);

const duplicateGroupId = core.cloneDocument(sample);
duplicateGroupId.groups[1].id = duplicateGroupId.groups[0].id;
const duplicateGroupIdValidation = core.validateDocument(duplicateGroupId);
assert.strictEqual(duplicateGroupIdValidation.valid, false);
assert.match(Array.from(duplicateGroupIdValidation.errors).join('\n'), /分组 ID.*重复/);

const missingContent = core.cloneDocument(sample);
missingContent.groups[0].items[0].content = '';
const warningValidation = core.validateDocument(missingContent);
assert.strictEqual(warningValidation.valid, true);
assert.match(Array.from(warningValidation.warnings).join('\n'), /复制内容为空/);

const normalized = core.normalizeDocument(core.migrateDocument({
  schemaVersion: 1,
  documentId: 'doc-imported',
  meta: { title: '导入内容', filename: 'imported' },
  groups: [{ id: 'g1', title: '组', items: [{ id: 'i1', title: '条目', content: '正文' }] }]
}));
assert.strictEqual(normalized.meta.themePreference, 'system');
assert.strictEqual(normalized.meta.filename, 'imported.html');
assert.strictEqual(normalized.groups[0].collapsed, false);
assert.strictEqual(normalized.groups[0].items[0].favorite, false);
assert.strictEqual(normalized.groups[0].items[0].shortcut, '');
assert.strictEqual(normalized.groups[0].items[0].link, '');
assert.deepStrictEqual(Array.from(normalized.groups[0].items[0].children), []);
assert.strictEqual(core.validateQuickLink('https://example.com/docs').valid, true);
assert.strictEqual(core.validateQuickLink('https://example.com:8443/docs').valid, true);
assert.strictEqual(core.validateQuickLink('javascript:alert(1)').valid, false);
assert.strictEqual(core.validateQuickLink('https://example.com:bad').valid, false);
assert.strictEqual(core.validateQuickLink('https://example.com:99999').valid, false);
const linkedDocument = core.cloneDocument(normalized);
linkedDocument.groups[0].items[0].link = 'https://example.com/docs';
assert.strictEqual(core.validateDocument(linkedDocument).valid, true);
const invalidLinkDocument = core.cloneDocument(linkedDocument);
invalidLinkDocument.groups[0].items[0].link = 'javascript:alert(1)';
assert.strictEqual(core.validateDocument(invalidLinkDocument).valid, false);

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
assert.strictEqual(core.resolveBatchSeparator('custom', ''), '');
const batchResult = core.composeBatchText(batchDoc, batchOrder, 'newline', '');
assert.strictEqual(batchResult.text, `${batchItems[2].content}\n${batchItems[0].content}`);
assert.strictEqual(batchResult.includedCount, 2);
assert.strictEqual(batchResult.skippedCount, 1);
assert.strictEqual(batchResult.missingCount, 1);
const customBatchEntry = { kind: 'custom', id: 'custom-1', title: '前置说明', content: '请先确认环境' };
const batchEntries = core.insertBatchEntry(
  [{ kind: 'item', id: batchItems[0].id }, { kind: 'item', id: batchItems[2].id }],
  customBatchEntry,
  'after',
  batchItems[0].id
);
assert.deepStrictEqual(Array.from(batchEntries, entry => entry.id), [batchItems[0].id, 'custom-1', batchItems[2].id]);
assert.deepStrictEqual(Array.from(core.moveBatchEntry(batchEntries, 'custom-1', 1), entry => entry.id), [batchItems[0].id, batchItems[2].id, 'custom-1']);
assert.deepStrictEqual(Array.from(core.reorderBatchEntryAtTarget(batchEntries, batchItems[2].id, 'custom-1'), entry => entry.id), [batchItems[0].id, batchItems[2].id, 'custom-1']);
assert.deepStrictEqual(Array.from(core.removeBatchEntry(batchEntries, 'custom-1'), entry => entry.id), [batchItems[0].id, batchItems[2].id]);
const secondCustomBatchEntry = { kind: 'custom', id: 'custom-2', title: '收尾说明', content: '请检查结果' };
const customAfterCustom = core.insertBatchEntry(batchEntries, secondCustomBatchEntry, 'after', 'custom-1');
assert.deepStrictEqual(Array.from(customAfterCustom, entry => entry.id), [batchItems[0].id, 'custom-1', 'custom-2', batchItems[2].id]);
assert.strictEqual(core.validateCustomBatchContent('  ', '正文').valid, true);
assert.strictEqual(core.validateCustomBatchContent('标题', '  \n ').valid, false);
const customBatch = core.composeBatchEntries(batchDoc, batchEntries, 'newline', '');
assert.strictEqual(customBatch.text, `${batchItems[0].content}\n请先确认环境\n${batchItems[2].content}`);
assert.deepStrictEqual(Array.from(customBatch.includedIds), [batchItems[0].id, 'custom-1', batchItems[2].id]);
assert.deepStrictEqual(Array.from(customBatch.includedItemIds), [batchItems[0].id, batchItems[2].id]);
assert.deepStrictEqual(Array.from(customBatch.includedCustomIds), ['custom-1']);
const appended = core.appendCustomDocumentItem(batchDoc, batchDoc.groups[0].id, '文档片段', '持久化正文');
assert.strictEqual(appended.valid, true);
assert.strictEqual(appended.document.groups[0].items.at(-1).content, '持久化正文');
assert.strictEqual(core.appendCustomDocumentItem(batchDoc, batchDoc.groups[0].id, '  ', '正文').valid, false);
assert.strictEqual(core.appendCustomDocumentItem(batchDoc, 'missing-group', '标题', '正文').valid, false);
const emptyBatchDoc = core.cloneDocument(batchDoc);
emptyBatchDoc.groups.forEach(group => group.items.forEach(item => { item.content = ''; }));
const emptyBatch = core.composeBatchText(emptyBatchDoc, batchItems.map(item => item.id), 'custom', '');
assert.strictEqual(emptyBatch.text, '');
assert.strictEqual(emptyBatch.includedCount, 0);
assert.strictEqual(emptyBatch.skippedCount, batchItems.length);
assert.deepStrictEqual(
  Array.from(core.toggleCopyOrder([batchItems[0].id], batchItems[1].id, true)),
  [batchItems[0].id, batchItems[1].id]
);
assert.deepStrictEqual(
  Array.from(core.toggleCopyOrder([batchItems[0].id, batchItems[1].id], batchItems[0].id, false)),
  [batchItems[1].id]
);
assert.deepStrictEqual(
  Array.from(core.toggleCopyOrder([batchItems[0].id, batchItems[0].id], batchItems[0].id, true)),
  [batchItems[0].id]
);
assert.deepStrictEqual(Array.from(core.toggleCopyOrder([], batchItems[0].id, false)), []);
assert.deepStrictEqual(
  Array.from(core.moveId([batchItems[0].id, batchItems[1].id, batchItems[2].id], batchItems[2].id, -1)),
  [batchItems[0].id, batchItems[2].id, batchItems[1].id]
);
assert.deepStrictEqual(Array.from(core.moveId(['a', 'b', 'c'], 'b', 1)), ['a', 'c', 'b']);
assert.deepStrictEqual(Array.from(core.moveId(['a', 'b'], 'a', -1)), ['a', 'b']);
assert.deepStrictEqual(Array.from(core.moveId(['a', 'b'], 'missing', 1)), ['a', 'b']);
assert.deepStrictEqual(Array.from(core.reorderIdAtTarget(['a', 'b', 'c'], 'a', 'b')), ['b', 'a', 'c']);
assert.deepStrictEqual(Array.from(core.reorderIdAtTarget(['a', 'b', 'c'], 'a', 'c')), ['b', 'c', 'a']);
assert.deepStrictEqual(Array.from(core.reorderIdAtTarget(['a', 'b', 'c'], 'c', 'a')), ['c', 'a', 'b']);
assert.deepStrictEqual(Array.from(core.reorderIdAtTarget(['a', 'b'], 'missing', 'b')), ['a', 'b']);

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
searchDoc.groups[0].items[0].link = 'https://example.com/docs';
const byContent = core.filterDocument(searchDoc, 'NGINX', false);
assert.strictEqual(byContent.groups.length, 1);
assert.strictEqual(byContent.groups[0].items.length, 1);
assert.strictEqual(byContent.groups[0].items[0].title, '查看进程');
const byNote = core.filterDocument(searchDoc, '回复模板', false);
assert.strictEqual(byNote.groups.length, 1);
assert.strictEqual(byNote.groups[0].items[0].title, '问题已处理');
const byGroup = core.filterDocument(searchDoc, '常用命令', false);
assert.strictEqual(byGroup.groups[0].items.length, 2);
const nestedSearch = core.filterDocument(treeDocument, '叶', false);
assert.strictEqual(nestedSearch.groups[0].items.length, 1);
assert.strictEqual(nestedSearch.groups[0].items[0].children[0].children[0].children[0].id, 'leaf');
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
const generatedRuntimeMatch = generated.match(/<script id="generated-app-logic">([\s\S]*?)<\/script>/);
assert.ok(generatedRuntimeMatch, 'generated-app-logic script must exist');
assert.doesNotThrow(() => new vm.Script(generatedRuntimeMatch[1], { filename: 'generated-app-logic' }));
assert.strictEqual((generatedRuntimeMatch[1].match(/\bstate\./g) || []).length, 0, 'generated runtime must not depend on editor state');
const generatedIds = new Set(Array.from(generated.matchAll(/\bid=["']([^"']+)["']/g), match => match[1]));
const listenerTargetIds = Array.from(generatedRuntimeMatch[1].matchAll(/byId\(["']([^"']+)["']\)\.addEventListener/g), match => match[1]);
listenerTargetIds.forEach(id => assert.ok(generatedIds.has(id), `generated listener target #${id} must exist`));
assert.doesNotMatch(generated, /preview-command-toggle/);
assert.doesNotMatch(generated, /expandedPreviewIds/);
for (const id of [
  'paste-data',
  'generated-app-logic',
  'generated-title',
  'download-current-html-button',
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
  'batch-clear-button',
  'custom-content-button',
  'custom-content-dialog',
  'custom-content-title',
  'custom-content-text',
  'custom-content-position',
  'custom-save-to-document',
  'custom-document-group',
  'custom-document-title',
  'custom-submit-button',
  'custom-submit-copy-button',
  'generated-import-json-button',
  'generated-export-json-button',
  'generated-import-json-input'
]) {
  assert.match(generated, new RegExp(`id=["']${id}["']`), `generated page missing #${id}`);
}
for (const id of ['generated-statistics', 'statistics-summary-count', 'statistics-content', 'statistics-groups']) {
  assert.match(generated, new RegExp(`id=["']${id}["']`), `generated page missing #${id}`);
}
assert.match(generated, /<details id=["']generated-statistics["']/);
assert.match(generated, /class=["']statistics-summary["']/);
assert.match(generated, /function\s+renderStatistics\s*\(/);
assert.match(generated, /全部命令/);
assert.match(generated, /当前搜索结果/);
assert.match(generated, /分组汇总/);
assert.match(generated, /导出最新版 HTML/);
assert.match(generated, /下载 HTML/);
assert.match(generated, /download-current-html-button[\s\S]*exportCurrentHtml/);
assert.match(generated, /scroll-padding-top/);
assert.match(generated, /scroll-margin-top/);
assert.match(generated, /generated-toolbar-height[\s\S]*16px/);
assert.match(generated, /恢复文件原始内容/);
assert.match(generated, /导入 JSON/);
assert.match(generated, /导出 JSON/);
assert.match(generated, /accept=["']application\/json,\.json["']/);
assert.match(generated, /function\s+exportCurrentJson\s*\(/);
assert.match(generated, /function\s+handleJsonImport\s*\(/);
assert.match(generated, /normalizeImportedDocument\(/);
assert.match(generated, /validateImportedDocument\(/);
assert.match(generated, /validateImportedDocument\(normalized,\s*parsed\)/);
assert.match(generated, /ordinary\.length\s*!==\s*1/);
assert.match(generated, /Ctrl\+F/);
assert.match(generated, /Alt\+Left/);
assert.match(generated, /导入失败，当前内容未改变/);
assert.match(generated, /generated-import-json-input[\s\S]*?value\s*=\s*''/);
assert.match(generated, /选择当前结果/);
assert.match(generated, /一键复制/);
assert.match(generated, /class=["']hero-actions["'][\s\S]*id=["']custom-content-button["']/);
assert.match(generated, /const core = \{\s*newId,/);
assert.match(generated, /class=["']item-select["']/);
assert.match(generated, /class=["']item-visibility-toggle["']/);
assert.match(generated, /class=["']item-expand-toggle["']/);
assert.match(generated, /edit-meta-fields/);
assert.match(generated, /显示全部命令/);
assert.match(generated, /收起命令/);
assert.match(generated, /expandedIds/);
assert.match(generated, /function\s+toggleItemExpanded\s*\(/);
assert.match(generated, /grid-template-columns:\s*minmax\(180px,\s*\.72fr\)\s+minmax\(280px,\s*1\.28fr\)/);
assert.match(generated, /class=["']nav-item-link["']/);
assert.match(generated, /card-order-badge/);
assert.match(generated, /card-copy-action/);
assert.match(generated, /function\s+copyLine\s*\(/);
assert.match(generated, /function\s+copySelection\s*\(/);
assert.match(generated, /function\s+openCustomContentDialog\s*\(/);
assert.match(generated, /function\s+submitCustomContent\s*\(/);
assert.match(generated, /renderCustomPositionOptions/);
assert.match(generated, /custom-save-to-document/);
assert.match(generated, /加入并复制/);
assert.match(generated, /复制选中/);
assert.match(generated, /复制全部/);
assert.match(generated, /command-line/);
assert.match(generated, /window\.getSelection\(\)/);
assert.match(generated, /copySelectionButton\.addEventListener\(['"]mousedown['"]/);
assert.match(generated, /card\.addEventListener\(['"]keydown['"]/);
assert.match(generated, /function\s+restoreTransientFocus\s*\(/);
assert.match(generated, /内容将直接连接/);
assert.match(generated, /item-link/);
assert.match(generated, /link\.target\s*=\s*["']_blank["']/);
assert.match(generated, /link\.rel\s*=\s*["']noopener noreferrer["']/);
assert.doesNotMatch(generated, /network-library-panel/);
assert.doesNotMatch(generated, /\/api\/html-paste\/delete/);
assert.doesNotMatch(generated, /network-draft-content/);

const invalidDraft = core.cloneDocument(searchDoc);
invalidDraft.groups[0].items[0].shortcut = 'Ctrl+';
assert.strictEqual(core.validateDocument(invalidDraft).valid, false);
assert.strictEqual(JSON.parse(JSON.stringify(searchDoc)).meta.title, searchDoc.meta.title);

for (const networkId of [
  'network-library-toolbar',
  'network-library-panel',
  'network-refresh-button',
  'network-search',
  'network-type-filter',
  'network-file-list',
  'network-preview',
  'network-import-json-button',
  'network-export-json-button',
  'network-overwrite-checkbox'
]) {
  assert.match(html, new RegExp(`id=["']${networkId}["']`), `editor missing #${networkId}`);
}
assert.match(html, /function\s+loadNetworkLibrary\s*\(/);
assert.match(html, /function\s+importNetworkJson\s*\(/);
assert.match(html, /function\s+saveNetworkJson\s*\(/);
assert.match(html, /function\s+saveNetworkBundle\s*\(/);
assert.match(html, /network-export-json-button[\s\S]*导出 HTML \+ JSON/);
assert.match(html, /saveNetworkBundle[\s\S]*\.json[\s\S]*\.html/);
assert.match(html, /saveNetworkBundle[\s\S]*networkFiles/);
assert.match(html, /function\s+saveNetworkFile\s*\([\s\S]*\/api\/html-paste\/save/);
assert.match(html, /saveNetworkBundle[\s\S]*saveNetworkFile\(jsonFilename[\s\S]*saveNetworkFile\(htmlFilename/);
assert.match(html, /function\s+renderNetworkLibrary\s*\(/);
assert.match(html, /function\s+previewNetworkHtml\s*\(/);
assert.match(html, /\/api\/html-paste\/list/);
assert.match(html, /\/api\/html-paste\/read\?name=/);
assert.match(html, /\/api\/html-paste\/save/);
assert.match(html, /覆盖保存/);
assert.match(html, /新窗口打开/);
assert.match(html, /sandbox=["']allow-scripts allow-forms allow-modals["']/);
assert.match(html, /window\.open\(\s*networkFileUrl\(/);
assert.doesNotMatch(html, /window\.open\(\s*['"]about:blank['"]/);
assert.match(html, /noopener,noreferrer/);
assert.match(generated, /const expandedIds = new Set\(allItems\(\)\.map/);
assert.match(generated, /function\s+walkItems\s*\(/);
assert.match(generated, /function\s+renderNavItems\s*\(/);
assert.match(generated, /nav-tree-toggle/);
assert.match(generated, /generated-indent-item/);
assert.match(generated, /generated-outdent-item/);
assert.match(generated, /aria-level/);
assert.match(generated, /MAX_ITEM_DEPTH/);
assert.match(generated, /let revealAll = true;/);
assert.match(generated, /class=["']generated-toolbar["']/);
assert.match(generated, /class=["']generated-toolbar-slot["']/);
assert.match(generated, /generated-toolbar[\s\S]*position:\s*fixed/);
assert.match(generated, /function\s+toggleGroupSelection\s*\(/);
assert.match(generated, /createElement\(['"]button['"],\s*['"]group-select-button['"]/);
assert.match(generated, /本组全选/);
assert.match(generated, /function\s+markCopiedCards\s*\(/);
assert.match(generated, /composeBatch[\s\S]*const includedIds = \[\]/);
assert.match(generated, /composeBatch[\s\S]*includedIds\.push\(id\)/);
assert.match(generated, /copyBatch[\s\S]*markCopiedCards\(result\.includedIds\)/);
assert.match(generated, /function\s+renderBatchTray\s*\(/);
assert.match(generated, /function\s+toggleSelection\s*\(/);
assert.match(generated, /function\s+toggleItemVisibility\s*\(/);
assert.match(generated, /function\s+copyBatch\s*\(/);
assert.match(generated, /function\s+reorderIdAtTarget\s*\(/);
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

const treeGenerated = core.buildGeneratedHtml(treeDocument);
assert.match(treeGenerated, /根/);
assert.match(treeGenerated, /子/);
assert.match(treeGenerated, /孙/);
assert.match(treeGenerated, /叶/);
assert.match(treeGenerated, /renderNavItems/);
assert.match(treeGenerated, /setAttribute\('aria-level', String\(depth\)\)/);

const updated = core.cloneDocument(searchDoc);
updated.meta.title = '已更新标题';
const regenerated = core.replaceEmbeddedDocument(generated, updated);
assert.strictEqual(JSON.parse(core.extractEmbeddedDocument(regenerated)).meta.title, '已更新标题');
assert.strictEqual((regenerated.match(/id="paste-data"/g) || []).length, 1);

console.log('HtmlPasteGen core model tests passed');
