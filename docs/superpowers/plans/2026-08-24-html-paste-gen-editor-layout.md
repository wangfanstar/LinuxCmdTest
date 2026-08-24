# HtmlPasteGen 编辑区与长命令展示 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 调整 HtmlPasteGen 工作台桌面布局，合并快捷键/备注视觉栏，扩大命令编辑区，并为预览卡片增加长命令展开控制。

**Architecture:** 继续在单文件 `html/HtmlPasteGen.html` 中维护工作台 CSS 与 app-logic。快捷键和备注只做同一元信息栏的视觉组合，仍写回 `item.shortcut` 与 `item.note`；预览展开状态由 app-logic 的临时 `Set` 管理，不进入文档模型、草稿或生成 HTML。

**Tech Stack:** HTML/CSS、原生 JavaScript、Node.js `assert`/`vm`、现有浏览器验收流程、WSL `make`。

---

### Task 1: 固定新布局与预览展开契约

**Files:**
- Modify: `tests/html_paste_gen_ui_test.js`
- Modify: `tests/html_paste_gen_test.js`

- [ ] **Step 1: Write failing UI contract assertions**

在 `tests/html_paste_gen_ui_test.js` 中追加以下断言，要求工作台存在均衡扩宽比例、编辑元信息栏和可展开预览样式/逻辑：

```javascript
assert.match(html, /grid-template-columns:\s*minmax\(170px,\s*\.64fr\)\s+minmax\(480px,\s*1\.75fr\)\s+minmax\(300px,\s*1\.05fr\)/);
assert.match(html, /class="edit-meta-fields"/);
assert.match(html, /class="preview-command-toggle"/);
assert.match(html, /显示全部命令/);
assert.match(html, /function\s+togglePreviewCommand\s*\(/);
assert.match(html, /expandedPreviewIds/);
assert.match(html, /min-height:\s*150px/);
```

在 `tests/html_paste_gen_test.js` 中追加生成器安全契约，确认展开控件只属于工作台 app-logic，而生成成品页仍不被注入：

```javascript
assert.doesNotMatch(generated, /preview-command-toggle/);
assert.doesNotMatch(generated, /expandedPreviewIds/);
```

- [ ] **Step 2: Run tests and confirm expected RED**

Run:

```bash
node tests/html_paste_gen_ui_test.js
node tests/html_paste_gen_test.js
```

Expected: UI test fails first on the new workspace grid expression or missing `edit-meta-fields`; core test remains green until its new generated-page exclusion assertions are added, then fails only if generated output accidentally contains workbench-only strings.

- [ ] **Step 3: Commit the failing contract tests**

```bash
git add tests/html_paste_gen_ui_test.js tests/html_paste_gen_test.js
git commit -m "test: specify HtmlPasteGen editor layout update"
```

### Task 2: Implement balanced desktop layout and merged metadata field

**Files:**
- Modify: `html/HtmlPasteGen.html:307-470` (workbench CSS and responsive rules)
- Modify: `html/HtmlPasteGen.html:2199-2278` (`createItemEditor`)

- [ ] **Step 1: Change the desktop workspace proportions**

Replace the current default workspace columns with:

```css
.workspace {
  display: grid;
  grid-template-columns: minmax(170px, .64fr) minmax(480px, 1.75fr) minmax(300px, 1.05fr);
  gap: 14px;
  align-items: start;
  margin-top: 14px;
}
```

Update the 1180px fallback to `minmax(165px, .6fr) minmax(430px, 1.55fr) minmax(280px, .95fr)` and keep the existing `max-width: 900px` stacked mobile layout. Do not change the three mobile tabs.

- [ ] **Step 2: Add editor-specific metadata and command sizing styles**

Add these styles beside `.form-grid`:

```css
.edit-meta-fields { display: grid; gap: 8px; }
.edit-meta-fields .helper { margin-top: 4px; }
.content-field { min-height: 150px; resize: vertical; }
```

Keep `.form-grid .wide` for the command field and use `min-height: 150px` only for the command textarea so title and metadata do not grow unnecessarily.

- [ ] **Step 3: Merge shortcut and note into one visual column without changing data fields**

In `createItemEditor`, keep the existing independently-created `shortcut` and `note` controls and their `data-field` values. Replace separate `shortcutLabel`/`noteLabel` insertion with:

```javascript
const meta = createElement('div', 'edit-meta-fields wide');
const shortcutLabel = labeledControl('快捷键', shortcut);
shortcutLabel.appendChild(shortcutHelp);
const noteLabel = labeledControl('备注（可选）', note);
meta.append(shortcutLabel, noteLabel);
grid.append(titleLabel, meta, contentLabel, favoriteLabel);
```

The existing `handleEditorInput` continues to update `item.shortcut` and `item.note` independently. Do not merge or rename the model properties.

- [ ] **Step 4: Run focused tests and commit the layout change**

```bash
node tests/html_paste_gen_ui_test.js
node tests/html_paste_gen_test.js
git diff --check
git add html/HtmlPasteGen.html tests/html_paste_gen_ui_test.js tests/html_paste_gen_test.js
git commit -m "feat: widen HtmlPasteGen editor layout"
```

Expected: both tests pass and the new layout contract is present.

### Task 3: Add full-command preview expansion

**Files:**
- Modify: `html/HtmlPasteGen.html:2180-2420` (app state, preview rendering, event handling)
- Modify: `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1: Add temporary expansion state and cleanup helper**

Near `state` in `app-logic`, add:

```javascript
const expandedPreviewIds = new Set();
function cleanExpandedPreviewIds() {
  const validIds = new Set(state.document.groups.flatMap(group => group.items.map(item => item.id)));
  expandedPreviewIds.forEach(id => { if (!validIds.has(id)) expandedPreviewIds.delete(id); });
}
function togglePreviewCommand(itemId) {
  if (expandedPreviewIds.has(itemId)) expandedPreviewIds.delete(itemId);
  else expandedPreviewIds.add(itemId);
  renderPreview();
}
```

Call `cleanExpandedPreviewIds()` at the start of `renderPreview()` so search, filter and editor deletion cannot retain invalid IDs.

- [ ] **Step 2: Render a safe expand/collapse control for long commands**

In `renderPreview`, build the content and toggle using DOM methods:

```javascript
const fullContent = String(item.content || '（空内容）');
const needsExpansion = fullContent.length > 160 || fullContent.split('\n').length > 4;
const expanded = expandedPreviewIds.has(item.id);
const content = createElement('span', 'preview-card-content', fullContent);
if (!expanded) content.classList.add('preview-command-clamped');
card.append(top, content);
if (needsExpansion) {
  const toggle = createElement('span', 'preview-command-toggle', expanded ? '收起命令' : '显示全部命令');
  toggle.setAttribute('role', 'button');
  toggle.tabIndex = 0;
  toggle.addEventListener('click', event => {
    event.stopPropagation();
    togglePreviewCommand(item.id);
  });
  toggle.addEventListener('keydown', event => {
    if (event.key !== 'Enter' && event.key !== ' ') return;
    event.preventDefault();
    event.stopPropagation();
    togglePreviewCommand(item.id);
  });
  card.appendChild(toggle);
}
```

The card's existing click handler remains responsible for copying `item.content`. The toggle stops propagation so it never copies by accident. Add CSS for `.preview-command-clamped` (three-line clamp) and `.preview-command-toggle` (small but visible keyboard-focusable action).

- [ ] **Step 3: Add tests for expansion contract**

Extend `tests/html_paste_gen_ui_test.js` with:

```javascript
assert.match(html, /class="preview-command-clamped"/);
assert.match(html, /preview-command-toggle/);
assert.match(html, /event\.stopPropagation\(\)/);
assert.match(html, /fullContent\.length\s*>\s*160/);
```

- [ ] **Step 4: Run tests and commit the preview behavior**

```bash
node tests/html_paste_gen_ui_test.js
node tests/html_paste_gen_test.js
node --check tests/html_paste_gen_ui_test.js
git diff --check
git add html/HtmlPasteGen.html tests/html_paste_gen_ui_test.js tests/html_paste_gen_test.js
git commit -m "feat: expand long commands in HtmlPasteGen preview"
```

### Task 4: Browser, build, and Git archive verification

**Files:**
- Verify: `html/HtmlPasteGen.html`
- Verify: `tests/html_paste_gen_ui_test.js`
- Verify: `tests/html_paste_gen_test.js`

- [ ] **Step 1: Run fresh verification commands**

```bash
node --check tests/html_paste_gen_test.js
node --check tests/html_paste_gen_ui_test.js
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
wsl make
git diff --check
```

- [ ] **Step 2: Browser-check the requested workflow**

Open the local generator in the browser and verify:

1. The middle editor grows while the structure column remains usable and the preview remains visible at desktop width;
2. Shortcut and note inputs appear in one metadata column but editing either writes the corresponding independent field;
3. A long command preview shows “显示全部命令”; clicking it reveals the full text and changes the label to “收起命令”; clicking again collapses it;
4. Clicking the toggle does not copy the card, while clicking the card body still copies the complete command;
5. Deleting or filtering an item does not leave stale expanded state; mobile tabs still switch panels.

- [ ] **Step 3: Commit final verification/archive state**

```bash
git status --short
git log -4 --oneline -- html/HtmlPasteGen.html tests/html_paste_gen_ui_test.js
```

Keep the current branch and preserve unrelated pre-existing modifications. The final response links `html/HtmlPasteGen.html` and reports the verification results.
