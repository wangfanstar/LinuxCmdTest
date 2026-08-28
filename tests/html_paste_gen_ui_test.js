'use strict';

const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

const htmlPath = 'html/HtmlPasteGen.html';
const html = fs.readFileSync(htmlPath, 'utf8');
const appMatch = html.match(/<script id="app-logic">([\s\S]*?)<\/script>/);
assert.ok(appMatch, 'app-logic script must exist');
const appScript = appMatch[1];

function extractFunctionSource(source, name) {
  const match = source.match(new RegExp(`function\\s+${name}\\s*\\(`));
  assert.ok(match, `missing function ${name}`);
  const openBrace = source.indexOf('{', match.index);
  assert.ok(openBrace >= 0, `missing function body for ${name}`);
  let depth = 0;
  let state = 'code';
  let escaped = false;
  for (let index = openBrace; index < source.length; index += 1) {
    const char = source[index];
    const next = source[index + 1];
    if (state === 'line-comment') {
      if (char === '\n' || char === '\r') state = 'code';
      continue;
    }
    if (state === 'block-comment') {
      if (char === '*' && next === '/') { state = 'code'; index += 1; }
      continue;
    }
    if (state === 'single' || state === 'double' || state === 'template') {
      if (escaped) { escaped = false; continue; }
      if (char === '\\') { escaped = true; continue; }
      if ((state === 'single' && char === "'") || (state === 'double' && char === '"') || (state === 'template' && char === '`')) state = 'code';
      continue;
    }
    if (char === '/' && next === '/') { state = 'line-comment'; index += 1; continue; }
    if (char === '/' && next === '*') { state = 'block-comment'; index += 1; continue; }
    if (char === "'") { state = 'single'; continue; }
    if (char === '"') { state = 'double'; continue; }
    if (char === '`') { state = 'template'; continue; }
    if (char === '{') depth += 1;
    if (char === '}' && --depth === 0) return source.slice(match.index, index + 1);
  }
  assert.fail(`unterminated function ${name}`);
}

for (const id of [
  'back-link',
  'page-title',
  'output-filename',
  'generate-button',
  'download-current-html-button',
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

for (const id of [
  'custom-content-button',
  'custom-content-dialog',
  'custom-content-title',
  'custom-content-text',
  'custom-content-position',
  'custom-save-to-document',
  'custom-document-group',
  'custom-document-title',
  'custom-submit-button',
  'custom-submit-copy-button'
]) {
  assert.match(html, new RegExp(`id=["']${id}["']`), `missing #${id}`);
}

assert.match(html, /class="workspace"/);
assert.match(html, /\.project-toolbar\s*\{[\s\S]*position:\s*sticky/);
assert.match(html, /id=["']network-library-toggle["'][^>]*aria-expanded=["']false["']/);
assert.match(html, /id=["']network-library-dialog["'][^>]*hidden/);
assert.match(html, /function\s+setNetworkLibraryDialogOpen\s*\(/);
assert.match(html, /network-library-dialog[\s\S]*role=["']dialog["']/);
assert.match(html, /network-library-toggle[\s\S]*aria-expanded/);
assert.match(html, /event\.key\s*===\s*['"]Escape['"][\s\S]*setNetworkLibraryDialogOpen/);
assert.match(html, /class=["'][^"']*generation-bar[^"']*["'][^>]*aria-label=["']生成设置["']/);
assert.match(html, /generation-bar[\s\S]*position:\s*sticky/);
assert.match(html, /generation-bar[\s\S]*top:\s*0/);
assert.match(html, /generation-bar[\s\S]*generate-button/);
assert.match(html, /\.app-shell\s*\{[^}]*width:\s*min\(1880px,\s*calc\(100%\s*-\s*32px\)\)/);
assert.match(html, /\.workspace\s*\{(?=[^}]*--structure-width:\s*330px)(?=[^}]*grid-template-columns:\s*minmax\(260px,\s*var\(--structure-width[^)]*\)\)\s+8px\s+minmax\(560px,\s*1\.7fr\)\s+minmax\(320px,\s*1fr\))[^}]*\}/);
assert.match(html, /<button(?=[^>]*id=["']workspace-resizer["'])(?=[^>]*aria-label=["']调整左侧导航宽度["'])[^>]*>/);
assert.match(html, /<button(?=[^>]*id=["']workspace-resizer["'])(?=[^>]*aria-valuemin=["']260["'])[^>]*>/);
assert.match(html, /<button(?=[^>]*id=["']workspace-resizer["'])(?=[^>]*aria-valuemax=["']520["'])[^>]*>/);
assert.match(html, /<button(?=[^>]*id=["']workspace-resizer["'])(?=[^>]*aria-valuenow=["']330["'])[^>]*>/);
assert.match(html, /<button(?=[^>]*id=["']workspace-resizer["'])(?=[^>]*aria-orientation=["']vertical["'])[^>]*>/);
assert.match(html, /@media\s*\(max-width:\s*960px\)[\s\S]*?workspace-resizer[^}]*display:\s*none/);

assert.match(appScript, /const\s+STRUCTURE_WIDTH_KEY\s*=/);
assert.match(appScript, /const\s+STRUCTURE_WIDTH_DEFAULT\s*=/);
assert.match(appScript, /const\s+STRUCTURE_WIDTH_MIN\s*=\s*260/);
assert.match(appScript, /const\s+STRUCTURE_WIDTH_MAX\s*=\s*520/);
const clampSource = extractFunctionSource(appScript, 'clampStructureWidth');
const readSource = extractFunctionSource(appScript, 'readStructureWidth');
const setSource = extractFunctionSource(appScript, 'setStructureWidth');
const saveSource = extractFunctionSource(appScript, 'saveStructureWidth');
const startSource = extractFunctionSource(appScript, 'startWorkspaceResize');
const updateSource = extractFunctionSource(appScript, 'updateStructureWidthFromPointer');
const finishSource = extractFunctionSource(appScript, 'finishWorkspaceResize');
const initializeSource = extractFunctionSource(appScript, 'initializeWorkspaceResizer');
assert.match(clampSource, /Number\.isFinite/);
assert.match(clampSource, /STRUCTURE_WIDTH_DEFAULT/);
assert.match(clampSource, /STRUCTURE_WIDTH_MIN/);
assert.match(clampSource, /STRUCTURE_WIDTH_MAX/);
assert.match(clampSource, /Math\.min/);
assert.match(clampSource, /Math\.max/);
assert.match(readSource, /localStorage\.getItem\(STRUCTURE_WIDTH_KEY/);
assert.match(readSource, /catch/);
assert.match(readSource, /STRUCTURE_WIDTH_DEFAULT/);
assert.match(setSource, /style\.setProperty\(['"]--structure-width['"]/);
assert.match(setSource, /setAttribute\(['"]aria-valuenow['"]/);
assert.match(saveSource, /localStorage\.setItem\(STRUCTURE_WIDTH_KEY/);
assert.match(startSource, /pointerdown/);
assert.match(startSource, /preventDefault\(\)/);
assert.match(startSource, /structureResizeActive/);
assert.match(startSource, /setPointerCapture/);
assert.match(startSource, /updateStructureWidthFromPointer/);
assert.match(updateSource, /clientX/);
assert.match(updateSource, /getBoundingClientRect/);
assert.match(updateSource, /setStructureWidth/);
assert.match(finishSource, /releasePointerCapture/);
assert.match(finishSource, /structureResizeActive/);
assert.match(initializeSource, /setStructureWidth\(readStructureWidth\(\)\)/);
assert.match(initializeSource, /pointerdown[\s\S]*startWorkspaceResize/);
assert.match(initializeSource, /pointermove/);
assert.match(initializeSource, /addEventListener\(\s*["']pointerup["'][\s\S]*(?:finishWorkspaceResize|finishStructureResize)/);
assert.match(initializeSource, /addEventListener\(\s*["']pointercancel["'][\s\S]*(?:finishWorkspaceResize|finishStructureResize)/);
assert.match(initializeSource, /dblclick/);
assert.match(initializeSource, /keydown/);
assert.match(initializeSource, /ArrowLeft/);
assert.match(initializeSource, /ArrowRight/);
assert.match(initializeSource, /shiftKey/);
assert.match(initializeSource, /event\.key\s*===\s*["']Home["']/);
assert.match(initializeSource, /event\.key\s*===\s*["']End["']/);
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
assert.match(html, /download-current-html-button[\s\S]*exportCurrentHtml/);
assert.match(html, /scroll-padding-top/);
assert.match(html, /scroll-margin-top/);
assert.match(html, /generated-toolbar-height[\s\S]*16px/);
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
assert.match(html, /<details id=["']generated-statistics["']/);
assert.doesNotMatch(html, /<details id=["']generated-statistics["'][^>]*\bopen\b/);
assert.match(html, /\.generated-statistics\s*\{/);
assert.match(html, /statistics-metrics/);
assert.match(html, /function\s+renderStatistics\s*\(/);
assert.match(html, /\.sidebar\s*\{[\s\S]*max-height:\s*calc\(100vh\s*-\s*var\(--generated-toolbar-height/);
assert.match(html, /\.sidebar\s*\{[\s\S]*overflow-y:\s*auto/);
assert.match(html, /setProperty\(['"]--generated-toolbar-height['"]/);
assert.match(html, /group-select-button/);
assert.match(html, /group-item-list/);
assert.match(html, /group-item-link/);
assert.match(html, /toggle-group-items/);
assert.match(html, /select-item/);
assert.match(html, /expandedNavGroupIds/);
assert.match(html, /expandedNavNodeIds/);
assert.match(html, /function\s+renderItemTree\s*\(/);
assert.match(html, /function\s+renderEditorTree\s*\(/);
assert.match(html, /data-action="expand-all-nav"/);
assert.match(html, /data-action="collapse-all-nav"/);
assert.match(html, /展开全部/);
assert.match(html, /折叠全部/);
assert.match(html, /function\s+cleanExpandedNavNodeIds\s*\(/);
assert.match(html, /indent-item/);
assert.match(html, /outdent-item/);
assert.match(html, /add-child-item/);
assert.match(html, /add-sibling-item/);
assert.match(html, /aria-level/);
assert.match(html, /aria-setsize/);
assert.match(html, /id=["']tree-context-menu["']/);
assert.match(html, /class=["'][^"']*tree-context-menu[^"']*["']/);
const editorCoreScriptIndex = html.indexOf('<script id="core-logic">');
const editorContextMenuIndex = html.indexOf('<div id="tree-context-menu"');
assert.ok(editorContextMenuIndex >= 0 && editorContextMenuIndex < editorCoreScriptIndex, 'editor context menu must be in the editor document, before the core script');
assert.match(html, /contextmenu/);
assert.match(html, /treeClipboard/);
assert.match(html, /contextMenuState/);
assert.match(html, /function\s+openTreeContextMenu\s*\(/);
assert.match(html, /function\s+handleTreeContextAction\s*\(/);
assert.match(html, /data-context-action/);
assert.match(html, /pasteItemAsChild\(/);
assert.match(html, /pasteItemAsSibling\(/);
assert.match(html, /pasteGroupAfter\(/);
assert.match(html, /demote-group-to-item/);
assert.match(html, /promote-item-to-group/);
assert.match(html, /groupDemotionStatus\(/);
assert.match(html, /function\s+demoteGroupFromEditor\s*\(/);
assert.match(html, /function\s+promoteItemFromEditor\s*\(/);
assert.match(html, /降级为上一分组的条目/);
assert.match(html, /直接升级为分组/);
assert.match(html, /currentDepth\s*===\s*1[\s\S]*升级为分组/);
assert.match(html, /confirm\([\s\S]*升级为新分组/);
assert.match(html, /Escape/);
assert.match(html, /MAX_ITEM_DEPTH/);
assert.match(html, /最多只能|最多 4 级/);
assert.match(html, /migrateDocument/);
assert.match(html, /html-paste-gen:draft:v2/);
assert.match(html, /aria-expanded/);
assert.match(html, /aria-controls/);
assert.match(html, /默认折叠/);
assert.match(html, /function\s+markCopiedCards\s*\(/);
assert.match(html, /batchEntries/);
assert.match(html, /insertBatchEntry/);
assert.match(html, /composeBatchEntries/);
assert.match(html, /加入并复制/);
assert.match(html, /保存到文档/);
assert.match(html, /保存到文档时必须填写文档条目标题/);
assert.match(html, /function\s+openCustomContentDialog\s*\(/);
assert.match(html, /function\s+renderCustomBatchEntry\s*\(/);
assert.match(html, /copiedCustomIds/);
assert.match(html, /const saved = saveOverride\(\);[\s\S]*if \(!saved\)/);
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

assert.doesNotThrow(() => new vm.Script(appScript, { filename: htmlPath }));

console.log('HtmlPasteGen UI contract tests passed');
