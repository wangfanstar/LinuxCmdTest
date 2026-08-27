# Generated Command Statistics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task with review checkpoints.

**Goal:** Add a default-collapsed, dynamically refreshed statistics panel to generated shortcut-copy HTML pages.

**Architecture:** Extend the existing generated HTML template in `HtmlPasteGen.html` with a `<details>` panel and theme-aware responsive styles. Add one runtime renderer that derives full-document and current-filter metrics from existing recursive helpers, builds DOM nodes with `textContent`, and is called from `renderMainOnly()` so all existing refresh paths update the panel.

**Tech Stack:** Plain HTML/CSS/JavaScript embedded in `html/HtmlPasteGen.html`; Node.js `assert`-based static/runtime tests; GNU Make via WSL for the C server build.

---

### Task 1: Add red tests for generated statistics contracts

**Files:**
- Modify: `tests/html_paste_gen_test.js:290-383`
- Modify: `tests/html_paste_gen_ui_test.js:126-181`

- [ ] **Step 1: Write the failing generated-runtime assertions**

In `tests/html_paste_gen_test.js`, after the existing generated HTML assertions, add:

```js
for (const id of ['generated-statistics', 'statistics-summary-count', 'statistics-content', 'statistics-groups']) {
  assert.match(generated, new RegExp(`id=["']${id}["']`), `generated page missing #${id}`);
}
assert.match(generated, /<details id=["']generated-statistics["']/);
assert.match(generated, /class=["']statistics-summary["']/);
assert.match(generated, /function\s+renderStatistics\s*\(/);
assert.match(generated, /全部命令/);
assert.match(generated, /当前搜索结果/);
assert.match(generated, /分组汇总/);
```

In `tests/html_paste_gen_ui_test.js`, after the generated toolbar assertions, add:

```js
assert.match(html, /<details id=["']generated-statistics["']/);
assert.doesNotMatch(html, /<details id=["']generated-statistics["'][^>]*\bopen\b/);
assert.match(html, /\.generated-statistics\s*\{/);
assert.match(html, /statistics-metrics/);
assert.match(html, /function\s+renderStatistics\s*\(/);
```

- [ ] **Step 2: Run the focused tests and verify the expected RED failure**

Run:

```bash
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
```

Expected: the first command fails on the missing `generated-statistics` ID (or the first missing statistics assertion), and the second command fails on the missing generated statistics markup. No production code is changed before these failures are observed.

### Task 2: Add the collapsed statistics panel markup and responsive styles

**Files:**
- Modify: `html/HtmlPasteGen.html:1655-1826,1858-1905`

- [ ] **Step 1: Add markup after the fixed generated toolbar slot**

Inside `generatedShell(data)`, immediately after the closing `</div>` for `.generated-toolbar-slot` and before `<div class="layout">`, add:

```html
<details id="generated-statistics" class="generated-statistics">
  <summary class="statistics-summary">
    <span class="statistics-title">命令统计</span>
    <span id="statistics-summary-count" class="statistics-summary-count">0 条命令</span>
  </summary>
  <div id="statistics-content" class="statistics-content" aria-live="polite">
    <div id="statistics-groups" class="statistics-groups"></div>
  </div>
</details>
```

Do not add the `open` attribute so the panel is collapsed on first load.

- [ ] **Step 2: Add theme-aware statistics styles**

Add styles near the generated layout rules:

```css
.generated-statistics { margin-top: 12px; border: 1px solid var(--line); border-radius: var(--radius); background: var(--surface); box-shadow: var(--shadow); overflow: hidden; }
.statistics-summary { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 12px 16px; cursor: pointer; list-style: none; font-weight: 800; }
.statistics-summary::-webkit-details-marker { display: none; }
.statistics-summary::before { content: '▸'; color: var(--primary); margin-right: 8px; }
.generated-statistics[open] .statistics-summary::before { content: '▾'; }
.statistics-title { display: inline-flex; align-items: center; gap: 8px; }
.statistics-summary-count { color: var(--primary); font-size: 12px; white-space: nowrap; }
.statistics-content { display: grid; gap: 12px; padding: 0 16px 16px; }
.statistics-metrics { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px; }
.statistics-metric { padding: 10px 12px; border: 1px solid var(--line); border-radius: 10px; background: var(--surface-muted); }
.statistics-metric strong { display: block; color: var(--text); font-size: 18px; line-height: 1.2; }
.statistics-metric span { display: block; margin-top: 4px; color: var(--muted); font-size: 12px; }
.statistics-groups { display: grid; gap: 6px; }
.statistics-groups-title { margin: 0 0 2px; color: var(--text); font-size: 13px; }
.statistics-group-row { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 8px 10px; border: 1px solid var(--line); border-radius: 8px; background: var(--surface-muted); }
.statistics-group-name { min-width: 0; overflow: hidden; color: var(--text); text-overflow: ellipsis; white-space: nowrap; }
.statistics-group-count { color: var(--muted); font-size: 12px; white-space: nowrap; }
@media (max-width: 700px) { .statistics-metrics { grid-template-columns: repeat(2, minmax(0, 1fr)); } }
```

- [ ] **Step 3: Run the focused tests**

Run the two focused Node commands from Task 1. Expected: markup/style assertions pass, while the generated-runtime `renderStatistics` assertion remains the only failure until Task 3.

### Task 3: Implement dynamic statistics rendering and wire it into refreshes

**Files:**
- Modify: `html/HtmlPasteGen.html:2656-3310`

- [ ] **Step 1: Add `renderStatistics(groups)` beside other generated render helpers**

Insert before `renderMainOnly()`:

```js
function renderStatistics(groups) {
  const content = byId('statistics-content');
  const summaryCount = byId('statistics-summary-count');
  const groupSummary = byId('statistics-groups');
  if (!content || !summaryCount || !groupSummary) return;
  const totalItems = allItems();
  const visibleItems = (groups || []).reduce((count, entry) => {
    return count + flattenItems({ groups: [{ items: entry.items }] }).length;
  }, 0);
  summaryCount.textContent = `${totalItems.length} 条命令`;
  content.replaceChildren();
  const metrics = createElement('div', 'statistics-metrics');
  [
    ['全部命令', totalItems.length],
    ['当前搜索结果', visibleItems],
    ['分组', model.groups.length],
    ['收藏', totalItems.filter(item => item.favorite).length],
    ['快捷键', totalItems.filter(item => String(item.shortcut || '').trim()).length],
    ['快速链接', totalItems.filter(item => String(item.link || '').trim()).length]
  ].forEach(([label, value]) => {
    const metric = createElement('div', 'statistics-metric');
    metric.append(createElement('strong', '', String(value)), createElement('span', '', label));
    metrics.appendChild(metric);
  });
  const groupsBox = createElement('div', 'statistics-groups');
  groupsBox.id = 'statistics-groups';
  groupsBox.appendChild(createElement('h3', 'statistics-groups-title', '分组汇总'));
  if (!model.groups.length) {
    groupsBox.appendChild(createElement('p', 'empty-state', '暂无分组'));
  } else {
    model.groups.forEach(group => {
      const count = flattenItems({ groups: [{ items: group.items }] }).length;
      const row = createElement('div', 'statistics-group-row');
      row.append(
        createElement('span', 'statistics-group-name', group.title || '未命名分组'),
        createElement('span', 'statistics-group-count', `${group.items.length} 个根节点 · ${count} 条命令`)
      );
      groupsBox.appendChild(row);
    });
  }
  content.append(metrics, groupsBox);
}
```

Because `content.replaceChildren()` removes the original `#statistics-groups` placeholder, the renderer assigns the ID to the newly created group container before appending it. All user-controlled labels are inserted with `textContent` through `createElement`.

- [ ] **Step 2: Call the renderer from `renderMainOnly()`**

Immediately after `renderItems(groups);`, add:

```js
renderStatistics(groups);
```

Keep the existing details `open` state untouched; re-rendering only replaces the content inside the panel.

- [ ] **Step 3: Run the focused tests and verify GREEN**

Run:

```bash
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
```

Expected: both commands pass with exit code 0.

### Task 4: Regression verification and commit

**Files:**
- Verify: `html/HtmlPasteGen.html`, `tests/html_paste_gen_test.js`, `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1: Run the complete project checks**

```bash
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
node tests/http_handler_html_paste_test.js
node tests/index_navigation_test.js
node tests/simplewebserver_restart_test.js
node tests/html_paste_bundle_api_test.js
git diff --check
wsl make -B
```

Expected: all Node tests exit 0, `git diff --check` reports no whitespace errors, and `wsl make -B` exits 0 with `Build successful: bin/simplewebserver`.

- [ ] **Step 2: Review the diff and preserve unrelated worktree changes**

Run:

```bash
git diff --stat
git status --short
```

Only stage the three feature/test files; do not stage existing `.claude/settings.local.json`, deleted `html/wiki/sqlite_db/*`, `.port`, `.superpowers/`, or `nul`.

- [ ] **Step 3: Commit the implementation**

```bash
git add html/HtmlPasteGen.html tests/html_paste_gen_test.js tests/html_paste_gen_ui_test.js
git commit -m "feat: add collapsible generated command statistics"
```

- [ ] **Step 4: Verify the commit and worktree**

```bash
git show --stat --oneline --summary HEAD
git status --short
```

Expected: the new commit contains only the intended implementation/test files and unrelated pre-existing changes remain unstaged.
