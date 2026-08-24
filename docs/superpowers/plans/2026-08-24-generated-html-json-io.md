# Generated HTML JSON Import Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task with verification checkpoints.

**Goal:** Add safe JSON import/export controls to every generated standalone HtmlPasteGen page and archive the verified changes in Git and GitHub.

**Architecture:** Extend the generated page template inside `html/HtmlPasteGen.html`. Add file controls to the generated page hero, serialize the current normalized `model` for export, and import through generated-local `normalizeImportedDocument` plus `validateImportedDocument` before any state replacement. Keep the generator workbench JSON flow unchanged and reuse the generated page’s existing download, status, localStorage, and render helpers.

**Tech Stack:** Standalone HTML/CSS/JavaScript, existing HtmlPasteGen core helpers, Node.js contract tests, VM syntax checks, WSL `make`, in-app browser verification, Git/GitHub CLI or `git push`.

---

### Task 1: Add failing contracts for generated-page JSON controls and runtime

**Files:**
- Modify: `tests/html_paste_gen_test.js`
- Modify: `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1: Add generated HTML assertions before implementation**

In `tests/html_paste_gen_test.js`, extend the generated-page assertions after the existing `batch-clear-button` ID checks:

```js
  'generated-import-json-button',
  'generated-export-json-button',
  'generated-import-json-input'
```

Add these behavior contracts after the existing generated-page text assertions:

```js
assert.match(generated, /导入 JSON/);
assert.match(generated, /导出 JSON/);
assert.match(generated, /accept=["']application\/json,\.json["']/);
assert.match(generated, /function\s+exportCurrentJson\s*\(/);
assert.match(generated, /function\s+handleJsonImport\s*\(/);
assert.match(generated, /normalizeImportedDocument\(/);
assert.match(generated, /validateImportedDocument\(/);
assert.match(generated, /导入失败，当前内容未改变/);
assert.match(generated, /generated-import-json-input.*value\s*=\s*''|value\s*=\s*''.*generated-import-json-input/);
```

In `tests/html_paste_gen_ui_test.js`, add source-level checks after the existing generator JSON controls:

```js
assert.match(html, /generated-import-json-button/);
assert.match(html, /generated-export-json-button/);
assert.match(html, /generated-import-json-input/);
```

- [ ] **Step 2: Run the focused tests and verify the expected RED failure**

Run:

```bash
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
```

Expected: the core test fails on the missing generated JSON control/function contract; the existing UI test remains green or fails only on the newly added missing control contract. Do not change production code before observing this failure.

- [ ] **Step 3: Commit the failing contracts**

```bash
git add tests/html_paste_gen_test.js tests/html_paste_gen_ui_test.js
git commit -m "test: specify generated HTML JSON import export"
```

### Task 2: Add generated-page JSON controls and export behavior

**Files:**
- Modify: `html/HtmlPasteGen.html` in the `generatedShell` template around the hero action buttons and generated app script.

- [ ] **Step 1: Add the generated page controls**

In the generated page hero action group, add two buttons and one hidden file input next to the existing `edit-mode-toggle` button:

```html
<button id="generated-import-json-button" class="button" type="button">导入 JSON</button>
<button id="generated-export-json-button" class="button" type="button">导出 JSON</button>
<input id="generated-import-json-input" type="file" accept="application/json,.json" hidden>
```

The controls must be generated into every standalone HTML file; they must not add dependencies or server routes.

- [ ] **Step 2: Add a dedicated JSON export helper**

Inside `generated-app-logic`, after `downloadHtml` or beside the existing export helpers, add:

```js
function exportCurrentJson() {
  const filename = safeFilename(model.meta.filename || model.meta.title || 'quick-copy')
    .replace(/\.html$/i, '.json');
  const exported = clone(model);
  if (exported.meta) delete exported.meta.themePreference;
  const text = JSON.stringify(exported, null, 2);
  downloadFile(text, filename, 'application/json;charset=utf-8');
  announce('已导出当前 JSON。', 'success');
}
```

If the existing generated runtime only has `downloadHtml`, factor its Blob/download code into a local `downloadFile(text, filename, type)` helper and make `downloadHtml` call it. Preserve the existing HTML filename behavior and ensure the JSON export uses the current normalized `model` only.

- [ ] **Step 3: Run generated syntax and contract tests**

Run:

```bash
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
```

Expected: the tests still fail only on the import handler contracts; no generated runtime syntax error may appear.

### Task 3: Add validated JSON import with failure isolation

**Files:**
- Modify: `html/HtmlPasteGen.html` in `generated-app-logic`.

- [ ] **Step 1: Add import helpers and replacement flow**

Implement these functions near `exportCurrentJson`:

```js
function normalizeImportedDocument(value) {
  if (!value || Number(value.schemaVersion) !== originalDocument.schemaVersion) throw new Error('不支持的数据版本。');
  const normalized = clone(value);
  normalized.schemaVersion = originalDocument.schemaVersion;
  normalized.documentId = String(normalized.documentId || originalDocument.documentId);
  normalized.meta = {
    title: String(normalized.meta && normalized.meta.title || '').trim(),
    filename: safeFilename(normalized.meta && normalized.meta.filename),
    themePreference: ['system', 'light', 'dark'].includes(normalized.meta && normalized.meta.themePreference)
      ? normalized.meta.themePreference
      : 'system'
  };
  if (!Array.isArray(normalized.groups)) throw new Error('分组列表格式无效。');
  normalized.groups = normalized.groups.map(group => ({
    id: String(group && group.id || ''),
    title: String(group && group.title || '').trim(),
    collapsed: Boolean(group && group.collapsed),
    items: Array.isArray(group && group.items) ? group.items.map(item => ({
      id: String(item && item.id || ''),
      title: String(item && item.title || '').trim(),
      content: String(item && item.content || ''),
      shortcut: normalizeShortcut(item && item.shortcut),
      note: String(item && item.note || ''),
      favorite: Boolean(item && item.favorite),
      collapsed: Boolean(item && item.collapsed)
    })) : []
  }));
  return normalized;
}

function validateImportedDocument(value) {
  const errors = [];
  if (!value || Number(value.schemaVersion) !== originalDocument.schemaVersion) errors.push('不支持的数据版本。');
  if (!value || !value.meta || !String(value.meta.title || '').trim()) errors.push('页面名称不能为空。');
  if (!value || !Array.isArray(value.groups)) errors.push('分组列表格式无效。');
  const groupIds = new Set();
  const itemIds = new Set();
  const shortcuts = new Set();
  (value && Array.isArray(value.groups) ? value.groups : []).forEach((group, groupIndex) => {
    const groupId = String(group && group.id || '');
    if (!String(group && group.title || '').trim()) errors.push('第 ' + (groupIndex + 1) + ' 个分组名称不能为空。');
    if (!groupId || groupIds.has(groupId)) errors.push('分组 ID 重复或为空。');
    groupIds.add(groupId);
    if (!Array.isArray(group && group.items)) { errors.push('分组条目格式无效。'); return; }
    group.items.forEach((item, itemIndex) => {
      const itemId = String(item && item.id || '');
      const title = String(item && item.title || '').trim();
      if (!title) errors.push('第 ' + (groupIndex + 1) + ' 组第 ' + (itemIndex + 1) + ' 条标题不能为空。');
      if (!itemId || itemIds.has(itemId)) errors.push('条目 ID 重复或为空。');
      itemIds.add(itemId);
      const shortcut = String(item && item.shortcut || '');
      if (shortcut && shortcuts.has(shortcut)) errors.push('快捷键重复：' + shortcut + '。');
      if (shortcut) shortcuts.add(shortcut);
    });
  });
  return { valid: errors.length === 0, errors };
}

function summarizeDocument(value) {
  const groups = Array.isArray(value.groups) ? value.groups : [];
  return groups.length + ' 个分组，' + groups.reduce((count, group) => count + group.items.length, 0) + ' 条内容';
}

async function handleJsonImport(event) {
  const input = event.currentTarget;
  const file = input.files && input.files[0];
  input.value = '';
  if (!file) return;
  try {
    const raw = await file.text();
    const parsed = JSON.parse(raw);
    const normalized = normalizeImportedDocument(parsed);
    const validation = validateImportedDocument(normalized);
    if (!validation.valid) {
      const details = validation.errors.slice(0, 3).join('；');
      throw new Error(details || 'JSON 数据校验失败。');
    }
    if (!window.confirm('导入将替换当前内容（' + summarizeDocument(normalized) + '），是否继续？')) return;
    model = clone(normalized);
    selectedIds.clear();
    copyOrder = [];
    revealedIds.clear();
    hiddenIds.clear();
    expandedIds.clear();
    saveOverride();
    render();
    announce('JSON 已导入：' + summarizeDocument(model) + '。', 'success');
  } catch (error) {
    announce('导入失败，当前内容未改变：' + error.message, 'error');
  }
}
```

Keep the generated page self-contained by adding compact generated-local `normalizeImportedDocument` and `validateImportedDocument` helpers. They must enforce the embedded schema version, required titles, non-empty unique IDs, normalized shortcut strings, and duplicate shortcut rejection without depending on the generator workbench script. Do not assign `model` until parsing, normalization, validation, and user confirmation all succeed. If `file.text()` is unavailable in an older browser, use the existing FileReader pattern from the generator workbench instead of introducing a dependency.

- [ ] **Step 2: Bind controls without disrupting existing actions**

Near the existing generated-page event listeners, add:

```js
byId('generated-export-json-button').addEventListener('click', exportCurrentJson);
byId('generated-import-json-button').addEventListener('click', () => byId('generated-import-json-input').click());
byId('generated-import-json-input').addEventListener('change', handleJsonImport);
```

Keep the existing `reexport-button`, `restore-original-button`, batch controls, and edit-mode listeners unchanged.

- [ ] **Step 3: Run the focused tests and verify GREEN**

Run:

```bash
node --check tests/html_paste_gen_test.js
node --check tests/html_paste_gen_ui_test.js
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
```

Expected: both HtmlPasteGen tests pass, including generated runtime VM syntax checks; invalid imports never replace `model`.

- [ ] **Step 4: Commit the implementation**

```bash
git add html/HtmlPasteGen.html tests/html_paste_gen_test.js tests/html_paste_gen_ui_test.js
git commit -m "feat: add JSON import export to generated pages"
```

### Task 4: Browser verification and regression build

**Files:**
- No source changes expected.

- [ ] **Step 1: Serve the project and open the generator**

Run:

```bash
wsl make
wsl bash -lc './bin/simplewebserver -p 8897 -t 2 -q 64 -l /tmp/wfserver-htmlpastegen-json'
```

Open `http://127.0.0.1:8897/HtmlPasteGen.html` in the in-app browser and verify the generator’s existing JSON controls are unchanged.

- [ ] **Step 2: Build a generated page and verify JSON export/import**

Use the generator’s “生成 HTML” flow or an equivalent generated-page fixture. Verify:

1. “导出 JSON” downloads the current page data and the filename ends in `.json`.
2. “导入 JSON” accepts a valid exported file, asks for replacement, and updates the title, navigation, cards, and editor fields.
3. Importing malformed JSON shows “导入失败，当前内容未改变” and leaves the current title/card count unchanged.
4. Importing a structurally invalid document shows validation details and leaves the current page unchanged.
5. Re-selecting the same file works because the hidden input value is cleared.

- [ ] **Step 3: Run the full verification set**

```bash
node --check tests/html_paste_gen_test.js
node --check tests/html_paste_gen_ui_test.js
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
wsl make
git diff --check
git show --check --oneline HEAD
```

Expected: all commands exit 0. Stop the local test server after browser verification.

### Task 5: Review, commit status, and archive on GitHub

**Files:**
- No source changes expected unless review finds an issue.

- [ ] **Step 1: Request a read-only code review**

Review the implementation commit against its parent, focusing on file input reset, validation-before-replacement, no data mutation on import failure, safe download names, existing generated-page behaviors, and browser compatibility.

- [ ] **Step 2: Confirm the worktree and commits**

Run:

```bash
git status --short
git log --oneline -4
git diff --check
```

Do not stage unrelated existing changes in `.claude/settings.local.json`, `html/wiki/sqlite_db/pending_logs.jsonl`, or `nul`.

- [ ] **Step 3: Push the current branch to GitHub**

Because `origin` is already configured as `https://github.com/wangfanstar/LinuxCmdTest.git`, archive the commits with:

```bash
git push -u origin HEAD
```

Expected: the current branch `codex/register-viewer-auto-parse` is created or updated on GitHub without altering unrelated files or branches. If authentication or network policy blocks the push, report the exact blocker and retain the verified local commits.
