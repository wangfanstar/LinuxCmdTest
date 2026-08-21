# HtmlPasteGen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增一个无依赖的 `html/HtmlPasteGen.html`，用于编辑分组复制资料并生成可搜索、可快捷复制、可编辑和可再次导出的独立 HTML 文件。

**Architecture:** 目标保持单 HTML 文件，但把纯数据能力集中在 `window.HtmlPasteGenCore`，以便 Node.js 从真实页面提取脚本进行测试。生成器维护一个版本化文档模型；输出页面把模型安全嵌入 `application/json` 数据块，并在再次导出时用当前模型替换该数据块，不依赖原生成器。

**Tech Stack:** HTML5、CSS3、原生 JavaScript、Clipboard API、localStorage、Blob/Object URL、Node.js `assert` 与 `vm`。

---

## 文件结构

- Create: `html/HtmlPasteGen.html` — 生成器视觉、版本化数据核心、草稿持久化、实时预览、独立成品模板和浏览器控制器。
- Create: `tests/html_paste_gen_test.js` — 从真实 HTML 提取 `core-logic`，验证模型、校验、搜索、安全序列化和生成文件契约。
- Create: `tests/html_paste_gen_ui_test.js` — 验证生成器与成品关键语义元素、离线约束、事件入口和响应式契约。

### Task 1：数据模型、文件名与快捷键核心

**Files:**
- Create: `tests/html_paste_gen_test.js`
- Create: `html/HtmlPasteGen.html`

- [ ] **Step 1：编写数据核心失败测试**

创建测试并先要求目标文件与核心 API 存在：

```javascript
'use strict';
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

const htmlPath = 'html/HtmlPasteGen.html';
assert.ok(fs.existsSync(htmlPath), 'html/HtmlPasteGen.html must exist');
const html = fs.readFileSync(htmlPath, 'utf8');
const match = html.match(/<script id="core-logic">([\s\S]*?)<\/script>/);
assert.ok(match, 'core-logic script must exist');
const context = { window: {}, crypto: { randomUUID: () => 'test-id' } };
context.window.window = context.window;
vm.createContext(context);
vm.runInContext(match[1], context, { filename: htmlPath });
const core = context.window.HtmlPasteGenCore;

assert.strictEqual(core.safeHtmlFilename('  我的工具  '), '我的工具.html');
assert.strictEqual(core.safeHtmlFilename('a:b/c*?'), 'a-b-c-.html');
assert.strictEqual(core.safeHtmlFilename('...'), 'quick-copy.html');
assert.strictEqual(core.normalizeShortcut(' ctrl + shift + a '), 'Ctrl+Shift+A');
assert.strictEqual(core.normalizeShortcut('Alt+1'), 'Alt+1');
assert.strictEqual(core.validateShortcut('Ctrl').valid, false);
assert.strictEqual(core.validateShortcut('Ctrl+Shift+A').valid, true);
assert.strictEqual(core.validateShortcut('Ctrl+L').valid, false);

const sample = core.createSampleDocument();
assert.strictEqual(sample.schemaVersion, 1);
assert.ok(sample.documentId);
assert.strictEqual(sample.groups.length, 2);
assert.ok(sample.groups.every(group => group.id && Array.isArray(group.items)));
const validation = core.validateDocument(sample);
assert.strictEqual(validation.valid, true);
assert.strictEqual(core.validateDocument({ schemaVersion: 99, groups: [] }).valid, false);
console.log('HtmlPasteGen core model tests passed');
```

- [ ] **Step 2：运行测试并确认正确失败**

Run: `node tests/html_paste_gen_test.js`

Expected: FAIL，错误包含 `html/HtmlPasteGen.html must exist`。

- [ ] **Step 3：实现最小可测试核心**

创建 HTML 骨架和 `id="core-logic"` 脚本，导出以下 API；快捷键排序固定为 `Ctrl`、`Alt`、`Shift`、`Meta` 加最后一个普通键，并拒绝 `Ctrl+L`、`Ctrl+T`、`Ctrl+W`、`Ctrl+R`、`Ctrl+N`、`Ctrl+P`、`Ctrl+S`、`Ctrl+F`、`Alt+Left`、`Alt+Right` 与仅修饰键：

```javascript
(function () {
  'use strict';
  const SCHEMA_VERSION = 1;
  const RESERVED = new Set(['Ctrl+L', 'Ctrl+T', 'Ctrl+W', 'Ctrl+R', 'Ctrl+N', 'Ctrl+P', 'Ctrl+S', 'Ctrl+F', 'Alt+Left', 'Alt+Right']);
  const MODIFIERS = ['Ctrl', 'Alt', 'Shift', 'Meta'];

  function newId(prefix) {
    const value = globalThis.crypto && crypto.randomUUID ? crypto.randomUUID() : `${Date.now()}-${Math.random().toString(36).slice(2)}`;
    return `${prefix}-${value}`;
  }
  function safeHtmlFilename(value) {
    let name = String(value || '').trim().replace(/[\\/:*?"<>|\x00-\x1f]+/g, '-').replace(/\s+/g, ' ');
    name = name.replace(/[. ]+$/g, '').slice(0, 120);
    if (!name || /^\.+$/.test(name)) return 'quick-copy.html';
    return /\.html?$/i.test(name) ? name.replace(/\.htm$/i, '.html') : `${name}.html`;
  }
  function normalizeShortcut(value) {
    const aliases = { control: 'Ctrl', ctrl: 'Ctrl', alt: 'Alt', option: 'Alt', shift: 'Shift', meta: 'Meta', cmd: 'Meta', command: 'Meta', escape: 'Escape', esc: 'Escape', space: 'Space' };
    const parts = String(value || '').split('+').map(part => part.trim()).filter(Boolean).map(part => aliases[part.toLowerCase()] || (part.length === 1 ? part.toUpperCase() : part[0].toUpperCase() + part.slice(1)));
    const key = parts.find(part => !MODIFIERS.includes(part));
    return [...MODIFIERS.filter(modifier => parts.includes(modifier)), ...(key ? [key] : [])].join('+');
  }
  function validateShortcut(value) {
    if (!String(value || '').trim()) return { valid: true, normalized: '', message: '' };
    const normalized = normalizeShortcut(value);
    const key = normalized.split('+').filter(part => !MODIFIERS.includes(part));
    if (key.length !== 1) return { valid: false, normalized, message: '快捷键需要且只能包含一个普通按键。' };
    if (RESERVED.has(normalized)) return { valid: false, normalized, message: '该组合键被浏览器或页面功能占用。' };
    return { valid: true, normalized, message: '' };
  }
  function createSampleDocument() {
    return {
      schemaVersion: SCHEMA_VERSION,
      documentId: newId('doc'),
      meta: { title: '我的快捷复制中心', filename: 'quick-copy.html', themePreference: 'system' },
      groups: [
        { id: newId('group'), title: '常用命令', collapsed: false, items: [
          { id: newId('item'), title: '查看监听端口', content: 'ss -lntp', shortcut: 'Alt+1', note: 'Linux', favorite: true, collapsed: false },
          { id: newId('item'), title: '查看进程', content: 'ps aux | grep nginx', shortcut: 'Alt+2', note: '', favorite: false, collapsed: false }
        ] },
        { id: newId('group'), title: '常用文本', collapsed: false, items: [
          { id: newId('item'), title: '问题已处理', content: '问题已处理，请验证后反馈，谢谢。', shortcut: 'Alt+3', note: '回复模板', favorite: false, collapsed: false }
        ] }
      ]
    };
  }
  function validateDocument(documentValue) {
    if (!documentValue || documentValue.schemaVersion !== SCHEMA_VERSION) return { valid: false, errors: ['不支持的数据版本。'], warnings: [] };
    const errors = [];
    const warnings = [];
    const shortcuts = new Map();
    if (!documentValue.meta || !String(documentValue.meta.title || '').trim()) errors.push('页面名称不能为空。');
    if (!Array.isArray(documentValue.groups)) errors.push('分组列表格式无效。');
    (documentValue.groups || []).forEach((group, groupIndex) => {
      if (!String(group.title || '').trim()) errors.push(`第 ${groupIndex + 1} 个分组名称不能为空。`);
      if (!Array.isArray(group.items)) errors.push(`分组“${group.title || groupIndex + 1}”的条目格式无效。`);
      (group.items || []).forEach((item, itemIndex) => {
        if (!String(item.title || '').trim()) errors.push(`第 ${groupIndex + 1} 组第 ${itemIndex + 1} 条标题不能为空。`);
        if (!String(item.content || '')) warnings.push(`“${item.title || '未命名条目'}”的复制内容为空。`);
        const check = validateShortcut(item.shortcut);
        if (!check.valid) errors.push(`“${item.title || '未命名条目'}”：${check.message}`);
        if (check.normalized && shortcuts.has(check.normalized)) errors.push(`快捷键 ${check.normalized} 与“${shortcuts.get(check.normalized)}”重复。`);
        if (check.normalized) shortcuts.set(check.normalized, item.title || '未命名条目');
      });
    });
    return { valid: errors.length === 0, errors, warnings };
  }
  window.HtmlPasteGenCore = { SCHEMA_VERSION, safeHtmlFilename, normalizeShortcut, validateShortcut, createSampleDocument, validateDocument };
}());
```

- [ ] **Step 4：运行数据核心测试并确认通过**

Run: `node tests/html_paste_gen_test.js`

Expected: PASS，并输出 `HtmlPasteGen core model tests passed`。

- [ ] **Step 5：提交数据核心**

Run: `git add html/HtmlPasteGen.html tests/html_paste_gen_test.js`

Run: `git commit -m "feat: add HtmlPasteGen data core"`

Expected: 提交仅包含目标 HTML 初始骨架和核心测试。

### Task 2：安全序列化、搜索与独立成品生成

**Files:**
- Modify: `tests/html_paste_gen_test.js`
- Modify: `html/HtmlPasteGen.html`

- [ ] **Step 1：加入生成文件失败测试**

在核心测试末尾加入：

```javascript
const dangerous = core.createSampleDocument();
dangerous.meta.title = '</script><img src=x onerror=alert(1)>';
dangerous.groups[0].items[0].content = '<b>only text</b>\u2028line';
const json = core.safeJsonForHtml(dangerous);
assert.ok(!json.includes('</script>'));
assert.ok(!json.includes('\u2028'));
const restored = JSON.parse(json);
assert.strictEqual(restored.meta.title, dangerous.meta.title);

const searchDoc = core.createSampleDocument();
assert.strictEqual(core.filterDocument(searchDoc, 'NGINX', false).groups[0].items.length, 1);
assert.strictEqual(core.filterDocument(searchDoc, 'Linux', false).groups[0].items.length, 1);
assert.ok(core.filterDocument(searchDoc, '', true).groups.flatMap(group => group.items).every(item => item.favorite));

const generated = core.buildGeneratedHtml(searchDoc);
assert.match(generated, /^<!DOCTYPE html>/i);
assert.match(generated, /id="paste-data" type="application\/json"/);
assert.match(generated, /id="generated-app-logic"/);
assert.match(generated, /导出最新版 HTML/);
assert.doesNotMatch(generated, /<(?:script|link)[^>]+(?:src|href)=["']https?:/i);
assert.strictEqual(JSON.parse(core.extractEmbeddedDocument(generated)).documentId, searchDoc.documentId);
```

- [ ] **Step 2：运行并确认因缺少序列化 API 失败**

Run: `node tests/html_paste_gen_test.js`

Expected: FAIL，错误包含 `core.safeJsonForHtml is not a function`。

- [ ] **Step 3：实现纯函数并加入成品模板**

在核心中新增并导出：

```javascript
function deepClone(value) { return JSON.parse(JSON.stringify(value)); }
function safeJsonForHtml(value) {
  return JSON.stringify(value).replace(/&/g, '\\u0026').replace(/</g, '\\u003c').replace(/>/g, '\\u003e').replace(/\u2028/g, '\\u2028').replace(/\u2029/g, '\\u2029');
}
function filterDocument(value, query, favoritesOnly) {
  const needle = String(query || '').trim().toLocaleLowerCase();
  const copy = deepClone(value);
  copy.groups = copy.groups.map(group => ({
    ...group,
    items: group.items.filter(item => {
      if (favoritesOnly && !item.favorite) return false;
      const haystack = [group.title, item.title, item.content, item.note].join('\n').toLocaleLowerCase();
      return !needle || haystack.includes(needle);
    })
  })).filter(group => group.items.length || (!needle && !favoritesOnly));
  return copy;
}
function extractEmbeddedDocument(html) {
  const match = String(html).match(/<script id="paste-data" type="application\/json">([\s\S]*?)<\/script>/);
  if (!match) throw new Error('未找到内嵌数据。');
  return match[1];
}
function buildGeneratedHtml(value) {
  const validated = validateDocument(value);
  if (!validated.valid) throw new Error(validated.errors.join('\n'));
  return generatedShell(safeJsonForHtml(value));
}
```

`generatedShell(data)` 返回完整 `<!DOCTYPE html>` 文档。实现时保留以下明确的 DOM 和运行时接口，CSS 扩展这些类但不改变 ID：

```javascript
function generatedShell(data) {
  return `<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>快捷复制中心</title><style>
*{box-sizing:border-box}body{margin:0;font-family:system-ui,"Microsoft YaHei",sans-serif;background:#f4f7fb;color:#172033}
.app{width:min(1180px,calc(100% - 32px));margin:auto;padding:24px 0}.layout{display:grid;grid-template-columns:220px 1fr;gap:18px}
.card{border:1px solid #dce3ef;border-radius:12px;background:#fff;padding:14px}@media(max-width:720px){.layout{grid-template-columns:1fr}}
</style></head><body><main class="app">
<header><h1 id="generated-title"></h1><input id="generated-search" type="search" aria-label="搜索全部内容">
<button id="favorites-filter" type="button">仅看收藏</button><button id="theme-toggle" type="button">切换主题</button>
<button id="edit-mode-toggle" type="button">编辑模式</button><p id="copy-status" aria-live="polite"></p></header>
<div class="layout"><nav id="generated-groups" aria-label="分组"></nav><section id="generated-items"></section></div>
<section id="generated-editor" hidden></section><button id="reexport-button" type="button" hidden>导出最新版 HTML</button>
<button id="restore-original-button" type="button" hidden>恢复文件原始内容</button></main>
<script id="paste-data" type="application/json">${data}<\/script>
<script id="generated-app-logic">
(function(){'use strict';
  const original = JSON.parse(document.getElementById('paste-data').textContent);
  const storageKey = 'html-paste-page:' + original.documentId + ':v1';
  let model = loadOverride() || JSON.parse(JSON.stringify(original));
  let query = '';
  let favoritesOnly = false;
  let editMode = false;
  function loadOverride(){try{const value=localStorage.getItem(storageKey);return value?JSON.parse(value):null}catch(error){return null}}
  function saveOverride(){try{localStorage.setItem(storageKey,JSON.stringify(model));return true}catch(error){return false}}
  function element(tag,className,text){const node=document.createElement(tag);if(className)node.className=className;if(text!==undefined)node.textContent=text;return node}
  function matches(group,item){const needle=query.trim().toLocaleLowerCase();const haystack=[group.title,item.title,item.content,item.note].join('\n').toLocaleLowerCase();return(!favoritesOnly||item.favorite)&&(!needle||haystack.includes(needle))}
  function render(){
    document.getElementById('generated-title').textContent=model.meta.title;
    const groups=document.getElementById('generated-groups');const items=document.getElementById('generated-items');groups.replaceChildren();items.replaceChildren();
    model.groups.forEach(group=>{const visible=group.items.filter(item=>matches(group,item));if(!visible.length&&(query||favoritesOnly))return;
      const link=element('a','group-link',group.title+' · '+visible.length);link.href='#group-'+group.id;groups.appendChild(link);
      const section=element('section','generated-group');section.id='group-'+group.id;section.appendChild(element('h2','group-title',group.title));
      visible.forEach(item=>{const card=element('article','card');card.dataset.itemId=item.id;card.tabIndex=0;card.appendChild(element('h3','item-title',item.title));card.appendChild(element('pre','item-content',item.content));if(item.note)card.appendChild(element('p','item-note',item.note));const button=element('button','copy-button','复制'+(item.shortcut?' · '+item.shortcut:''));button.type='button';button.addEventListener('click',()=>copyItem(item,card));card.appendChild(button);section.appendChild(card)});
      items.appendChild(section)});
    renderEditor();
  }
  async function copyItem(item,card){try{await navigator.clipboard.writeText(item.content)}catch(error){const area=element('textarea','copy-fallback',item.content);document.body.appendChild(area);area.select();if(!document.execCommand('copy')){document.getElementById('copy-status').textContent='自动复制失败，请手动复制。';return}area.remove()}card.dataset.copied='true';document.getElementById('copy-status').textContent='已复制：'+item.title}
  function renderEditor(){const editor=document.getElementById('generated-editor');editor.hidden=!editMode;document.getElementById('reexport-button').hidden=!editMode;document.getElementById('restore-original-button').hidden=!editMode;if(!editMode)return;editor.replaceChildren();model.groups.forEach(group=>group.items.forEach(item=>{const row=element('div','edit-row');const title=element('input','edit-title');title.value=item.title;title.addEventListener('input',()=>{item.title=title.value;saveOverride()});const content=element('textarea','edit-content');content.value=item.content;content.addEventListener('input',()=>{item.content=content.value;saveOverride()});row.append(title,content);editor.appendChild(row)}))}
  function exportCurrentHtml(){document.getElementById('paste-data').textContent=JSON.stringify(model).replace(/&/g,'\\u0026').replace(/</g,'\\u003c').replace(/>/g,'\\u003e');const blob=new Blob(['<!DOCTYPE html>\n'+document.documentElement.outerHTML],{type:'text/html;charset=utf-8'});const url=URL.createObjectURL(blob);const link=element('a');link.href=url;link.download=model.meta.filename||'quick-copy.html';link.click();setTimeout(()=>URL.revokeObjectURL(url),0)}
  document.getElementById('generated-search').addEventListener('input',event=>{query=event.target.value;render()});
  document.getElementById('favorites-filter').addEventListener('click',()=>{favoritesOnly=!favoritesOnly;render()});
  document.getElementById('edit-mode-toggle').addEventListener('click',()=>{editMode=!editMode;render()});
  document.getElementById('reexport-button').addEventListener('click',exportCurrentHtml);
  document.getElementById('restore-original-button').addEventListener('click',()=>{if(confirm('恢复文件内嵌的原始内容？')){model=JSON.parse(JSON.stringify(original));localStorage.removeItem(storageKey);render()}});
  document.addEventListener('keydown',event=>{if(event.target.matches('input,textarea,[contenteditable="true"]'))return;if(event.key==='/'&&!event.ctrlKey&&!event.altKey){event.preventDefault();document.getElementById('generated-search').focus();return}model.groups.flatMap(group=>group.items).some(item=>{if(!item.shortcut)return false;const parts=item.shortcut.split('+');const matched=event.ctrlKey===parts.includes('Ctrl')&&event.altKey===parts.includes('Alt')&&event.shiftKey===parts.includes('Shift')&&event.metaKey===parts.includes('Meta')&&parts.includes(event.key.length===1?event.key.toUpperCase():event.key);if(matched){event.preventDefault();copyItem(item,document.querySelector('[data-item-id="'+item.id+'"]')||document.body)}return matched})});
  render();
}());
<\/script></body></html>`;
}
```

生产实现继续用 `textContent` 和 `createElement` 渲染所有用户字段，并在编辑器中补齐规格要求的分组与条目新增、复制、删除、排序、备注、快捷键和收藏控件。复制优先调用 `navigator.clipboard.writeText`，失败时创建离屏 `textarea` 调用 `document.execCommand('copy')`，再次失败则保留可选择文本并显示手动复制说明。

- [ ] **Step 4：运行核心测试并确认生成文件契约通过**

Run: `node tests/html_paste_gen_test.js`

Expected: PASS，并输出 `HtmlPasteGen core model tests passed`。

- [ ] **Step 5：提交成品生成能力**

Run: `git add html/HtmlPasteGen.html tests/html_paste_gen_test.js`

Run: `git commit -m "feat: generate editable quick-copy pages"`

Expected: 提交只包含目标页面和核心测试增量。

### Task 3：三栏生成器、草稿持久化与导入导出

**Files:**
- Create: `tests/html_paste_gen_ui_test.js`
- Modify: `html/HtmlPasteGen.html`

- [ ] **Step 1：编写界面契约失败测试**

创建：

```javascript
'use strict';
const assert = require('assert');
const fs = require('fs');
const html = fs.readFileSync('html/HtmlPasteGen.html', 'utf8');

for (const id of ['page-title', 'output-filename', 'generate-button', 'group-list', 'editor-panel', 'preview-panel', 'import-json-input', 'export-json-button', 'new-project-button', 'restore-sample-button', 'draft-status', 'toast-region']) {
  assert.match(html, new RegExp(`id=["']${id}["']`), `missing #${id}`);
}
assert.match(html, /class="workspace"/);
assert.match(html, /grid-template-columns:\s*minmax\(190px,\s*0\.72fr\)\s+minmax\(360px,\s*1\.55fr\)\s+minmax\(280px,\s*1fr\)/);
assert.match(html, /@media\s*\(max-width:\s*900px\)/);
assert.match(html, /role="tablist"/);
assert.match(html, /aria-live="polite"/);
assert.match(html, /localStorage/);
assert.match(html, /Ctrl\+Enter/);
assert.match(html, /id="app-logic"/);
assert.doesNotMatch(html, /<(?:script|link)[^>]+(?:src|href)=["']https?:/i);
console.log('HtmlPasteGen UI contract tests passed');
```

- [ ] **Step 2：运行并确认界面契约失败**

Run: `node tests/html_paste_gen_ui_test.js`

Expected: FAIL，首先报告缺少 `#page-title` 或 `#generate-button`。

- [ ] **Step 3：实现完整三栏页面结构**

页面采用下列语义骨架：

```html
<main class="app-shell">
  <header class="hero">工具说明、离线徽章、页面名称、文件名、生成按钮</header>
  <nav class="project-actions">JSON 导入、JSON 导出、新建项目、恢复示例、草稿状态</nav>
  <div class="mobile-tabs" role="tablist">结构、编辑、预览</div>
  <section class="workspace">
    <aside class="panel structure-panel"><div id="group-list"></div></aside>
    <section id="editor-panel" class="panel editor-panel"></section>
    <aside id="preview-panel" class="panel preview-panel"></aside>
  </section>
  <div id="toast-region" aria-live="polite"></div>
</main>
```

视觉使用沉稳蓝灰背景、靛蓝主按钮、青绿色成功反馈和琥珀色警告。桌面 `workspace` 为 `minmax(190px, .72fr) minmax(360px, 1.55fr) minmax(280px, 1fr)`；小于 900px 改为页签单栏。按钮最小高度 40px，移动端 44px。危险按钮只在相关对象的更多操作区出现。

- [ ] **Step 4：实现生成器控制器**

控制器维护 `state = { document, selectedGroupId, mobilePanel, dirty }`，并实现：

```javascript
function update(mutator, message) {
  mutator(state.document);
  state.dirty = true;
  renderAll();
  scheduleDraftSave();
  if (message) announce(message);
}
function scheduleDraftSave() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    try {
      localStorage.setItem('html-paste-gen:draft:v1', JSON.stringify(state.document));
      setDraftStatus('已自动保存');
    } catch (error) {
      setDraftStatus('无法自动保存，请导出 JSON 备份', 'warning');
    }
  }, 250);
}
function downloadText(text, filename, type) {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 0);
}
function generateFile() {
  syncMetaInputs();
  const result = core.validateDocument(state.document);
  renderValidation(result);
  if (!result.valid) return announce(`还有 ${result.errors.length} 个问题需要处理。`, 'error');
  downloadText(core.buildGeneratedHtml(state.document), core.safeHtmlFilename(state.document.meta.filename), 'text/html;charset=utf-8');
  announce(result.warnings.length ? `已生成；另有 ${result.warnings.length} 条空内容警告。` : 'HTML 已生成并开始下载。', result.warnings.length ? 'warning' : 'success');
}
```

事件委托通过 `data-action` 处理分组与条目的新增、复制、删除、上下移动、收藏和折叠；文本输入通过 `data-field` 回写模型。条目正文按 `Ctrl+Enter` 新增下一条并聚焦标题。所有删除、新建空白和恢复示例操作使用包含目标名称的确认文字。JSON 导入在解析和 `validateDocument` 全部成功后才替换状态；失败时保持旧状态。

- [ ] **Step 5：运行界面与核心测试**

Run: `node tests/html_paste_gen_ui_test.js`

Expected: PASS，并输出 `HtmlPasteGen UI contract tests passed`。

Run: `node tests/html_paste_gen_test.js`

Expected: PASS，并输出 `HtmlPasteGen core model tests passed`。

- [ ] **Step 6：提交三栏生成器**

Run: `git add html/HtmlPasteGen.html tests/html_paste_gen_test.js tests/html_paste_gen_ui_test.js`

Run: `git commit -m "feat: finish HtmlPasteGen workbench"`

Expected: 提交只包含目标 HTML 和两份测试。

### Task 4：验证生成、编辑与再次导出的完整链路

**Files:**
- Verify: `html/HtmlPasteGen.html`
- Verify: `tests/html_paste_gen_test.js`
- Verify: `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1：运行语法与自动化测试**

Run: `node --check tests/html_paste_gen_test.js`

Expected: exit 0。

Run: `node --check tests/html_paste_gen_ui_test.js`

Expected: exit 0。

Run: `node tests/html_paste_gen_test.js`

Expected: PASS，并输出 `HtmlPasteGen core model tests passed`。

Run: `node tests/html_paste_gen_ui_test.js`

Expected: PASS，并输出 `HtmlPasteGen UI contract tests passed`。

- [ ] **Step 2：运行仓库构建**

Run: `wsl make`

Expected: exit 0，生成 `bin/simplewebserver`。

- [ ] **Step 3：执行真实浏览器桌面检查**

打开 `html/HtmlPasteGen.html`，确认示例数据出现、三栏宽度合理、切换分组不丢输入、上下移动和复制使用稳定 ID、重复快捷键阻止生成、JSON 导入失败不覆盖现有内容、刷新后恢复草稿。生成 `quick-copy.html` 后确认搜索、分组、收藏、点击复制、`Alt+1`、主题切换和编辑模式均可操作。

- [ ] **Step 4：验证成品再次导出与安全文本**

在成品编辑模式加入标题 `</script><img src=x onerror=alert(1)>` 和正文 `<b>only text</b>`，导出最新版并重新打开。预期字符串以纯文本显示、无弹窗、无网络请求；修改内容仍存在，并能继续第三次导出。执行“恢复文件原始内容”后回到该文件内嵌数据。

- [ ] **Step 5：执行窄屏与键盘检查**

将视口缩至 390px，确认三栏切换为“结构 / 编辑 / 预览”页签、分组与条目操作不横向溢出、按钮可触达。仅用键盘完成选择分组、编辑标题、`Ctrl+Enter` 新增、聚焦预览复制和生成文件。

- [ ] **Step 6：检查最终变更范围**

Run: `git diff --check HEAD~3 -- html/HtmlPasteGen.html tests/html_paste_gen_test.js tests/html_paste_gen_ui_test.js`

Expected: exit 0，无尾随空格或冲突标记。

Run: `git status --short`

Expected: 本功能文件无未提交变化；用户原有 `.claude/settings.local.json`、`html/wiki/sqlite_db/pending_logs.jsonl` 与其他无关状态保持不变。
