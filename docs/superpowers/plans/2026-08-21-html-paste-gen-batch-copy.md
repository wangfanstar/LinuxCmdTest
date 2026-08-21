# HtmlPasteGen Batch Copy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 HtmlPasteGen 生成的成品页增加多选复制篮、临时复制顺序、可配置分隔符、统一与单条正文显隐，以及分组到条目的两级导航。

**Architecture:** 在现有 `window.HtmlPasteGenCore` 中加入可测试的批量拼接与顺序纯函数；成品页运行时维护独立的临时选择状态，不修改文档模型。生成器工作台结构保持不变，只扩展其内嵌的成品 HTML、CSS 与运行时脚本。

**Tech Stack:** HTML5、CSS3、原生 JavaScript、Set、HTML5 Drag and Drop、Clipboard API、Node.js `assert` 与 `vm`。

---

## 文件结构

- Modify: `html/HtmlPasteGen.html` — 增加批量复制纯函数，扩展生成成品页的模板、状态、导航、卡片和复制篮。
- Modify: `tests/html_paste_gen_test.js` — 验证拼接、选择顺序、成品结构、运行时语法和旧功能契约。
- Modify: `tests/html_paste_gen_ui_test.js` — 保持生成器本体契约，并增加成品模板仍为单文件的检查。

### Task 1：批量顺序与文本拼接纯函数

**Files:**
- Modify: `tests/html_paste_gen_test.js`
- Modify: `html/HtmlPasteGen.html`

- [ ] **Step 1：编写批量拼接失败测试**

在核心测试中加入：

```javascript
const batchDoc = core.createSampleDocument();
const batchItems = batchDoc.groups.flatMap(group => group.items);
batchItems[1].content = '';
const order = [batchItems[2].id, batchItems[0].id, batchItems[1].id, 'missing-id'];

assert.strictEqual(core.resolveBatchSeparator('newline', 'x'), '\n');
assert.strictEqual(core.resolveBatchSeparator('blankLine', 'x'), '\n\n');
assert.strictEqual(core.resolveBatchSeparator('space', 'x'), ' ');
assert.strictEqual(core.resolveBatchSeparator('custom', ' | '), ' | ');
assert.strictEqual(core.resolveBatchSeparator('custom', ''), '');

assert.deepStrictEqual(
  JSON.parse(JSON.stringify(core.composeBatchText(batchDoc, order, 'newline', ''))),
  {
    text: `${batchItems[2].content}\n${batchItems[0].content}`,
    includedCount: 2,
    skippedCount: 1,
    missingCount: 1
  }
);

assert.deepStrictEqual(core.toggleCopyOrder([], 'a', true), ['a']);
assert.deepStrictEqual(core.toggleCopyOrder(['a'], 'a', true), ['a']);
assert.deepStrictEqual(core.toggleCopyOrder(['a', 'b'], 'a', false), ['b']);
assert.deepStrictEqual(core.toggleCopyOrder(['a'], 'a', false), []);
assert.deepStrictEqual(core.moveId(['a', 'b', 'c'], 'b', -1), ['b', 'a', 'c']);
assert.deepStrictEqual(core.moveId(['a', 'b', 'c'], 'b', 1), ['a', 'c', 'b']);
assert.deepStrictEqual(core.moveId(['a', 'b'], 'a', -1), ['a', 'b']);
assert.deepStrictEqual(core.moveId(['a', 'b'], 'x', 1), ['a', 'b']);
```

- [ ] **Step 2：运行并确认缺少 API 的失败**

Run: `node tests/html_paste_gen_test.js`

Expected: FAIL，错误包含 `core.resolveBatchSeparator is not a function`。

- [ ] **Step 3：实现批量纯函数**

在 `core-logic` 中增加并导出：

```javascript
function resolveBatchSeparator(mode, customSeparator) {
  if (mode === 'blankLine') return '\n\n';
  if (mode === 'space') return ' ';
  if (mode === 'custom') return String(customSeparator || '');
  return '\n';
}

function composeBatchText(documentValue, copyOrder, separatorMode, customSeparator) {
  const items = new Map();
  (documentValue.groups || []).forEach(group => {
    (group.items || []).forEach(item => items.set(item.id, item));
  });
  const values = [];
  let skippedCount = 0;
  let missingCount = 0;
  Array.from(new Set(copyOrder || [])).forEach(id => {
    const item = items.get(id);
    if (!item) { missingCount += 1; return; }
    const content = String(item.content || '');
    if (!content) { skippedCount += 1; return; }
    values.push(content);
  });
  return {
    text: values.join(resolveBatchSeparator(separatorMode, customSeparator)),
    includedCount: values.length,
    skippedCount,
    missingCount
  };
}

function toggleCopyOrder(copyOrder, itemId, selected) {
  const result = Array.from(new Set(copyOrder || [])).filter(id => id !== itemId);
  if (selected) result.push(itemId);
  return result;
}

function moveId(copyOrder, itemId, direction) {
  const result = Array.from(new Set(copyOrder || []));
  const index = result.indexOf(itemId);
  const target = index + direction;
  if (index < 0 || target < 0 || target >= result.length) return result;
  result.splice(target, 0, result.splice(index, 1)[0]);
  return result;
}
```

- [ ] **Step 4：运行核心测试并确认通过**

Run: `node tests/html_paste_gen_test.js`

Expected: PASS，并输出 `HtmlPasteGen core model tests passed`。

- [ ] **Step 5：提交纯函数增量**

Run: `git add html/HtmlPasteGen.html tests/html_paste_gen_test.js`

Run: `git commit -m "feat: add HtmlPasteGen batch copy core"`

Expected: 提交只包含目标 HTML 与核心测试。

### Task 2：成品页多选、显隐和两级导航

**Files:**
- Modify: `tests/html_paste_gen_test.js`
- Modify: `tests/html_paste_gen_ui_test.js`
- Modify: `html/HtmlPasteGen.html`

- [ ] **Step 1：编写成品结构失败测试**

在 `generated` 断言区加入：

```javascript
for (const id of [
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
assert.match(generated, /class="item-select"/);
assert.match(generated, /class="item-visibility-toggle"/);
assert.match(generated, /class="nav-item-link"/);
assert.match(generated, /function\s+renderBatchTray\s*\(/);
assert.match(generated, /function\s+toggleSelection\s*\(/);
assert.match(generated, /function\s+toggleItemVisibility\s*\(/);
assert.match(generated, /function\s+copyBatch\s*\(/);
assert.match(generated, /draggable\s*=\s*true/);
assert.match(generated, /dragstart/);
assert.match(generated, /drop/);
assert.match(generated, /选择当前结果/);
assert.match(generated, /一键复制/);
```

继续保留现有 `generated-app-logic` 语法检查、编辑模式、单条复制、重新导出和无外链断言。

- [ ] **Step 2：运行并确认首先缺少复制篮元素**

Run: `node tests/html_paste_gen_test.js`

Expected: FAIL，首先报告 `generated page missing #select-visible-button`。

- [ ] **Step 3：加入静态控件和响应式样式**

成品页顶部用以下控件替换原“收起正文”按钮，并加入批量入口：

```html
<button id="select-visible-button" class="button" type="button">选择当前结果</button>
<button id="content-visibility-toggle" class="button" type="button" aria-pressed="false">显示全部内容</button>
```

在 `main` 后加入固定复制篮：

```html
<aside id="batch-tray" class="batch-tray" aria-label="批量复制篮" hidden>
  <header class="batch-head">
    <strong>复制篮 · <span id="batch-count">0</span> 条</strong>
    <button id="batch-expand-button" class="button" type="button" aria-expanded="true">收起</button>
  </header>
  <div id="batch-body" class="batch-body">
    <ol id="batch-order-list" class="batch-order-list"></ol>
    <div class="batch-options">
      <label>分隔符
        <select id="batch-separator">
          <option value="newline">换行</option>
          <option value="blankLine">空行</option>
          <option value="space">空格</option>
          <option value="custom">自定义</option>
        </select>
      </label>
      <label id="custom-separator-wrap" hidden>自定义分隔符
        <input id="custom-separator" type="text">
      </label>
      <button id="batch-select-visible-button" type="button">选择当前结果</button>
      <button id="batch-clear-button" type="button">清空</button>
      <button id="batch-copy-button" type="button">一键复制</button>
    </div>
    <p id="batch-status" aria-live="polite"></p>
  </div>
</aside>
```

CSS 要求：`.batch-tray` 固定底部、桌面最大宽度 1180px、移动端占满可用宽度；`body.has-batch-tray` 增加底部留白；`.nav-item-link` 相对分组缩进；`.copy-card.is-selected`、`.copy-card.is-targeted` 和隐藏正文状态有明确反馈；小于 720px 时复制篮列表单列且按钮至少 44px。

- [ ] **Step 4：实现临时状态与一致性清理**

在成品运行时模型声明后加入：

```javascript
const selectedIds = new Set();
let copyOrder = [];
const revealedIds = new Set();
const hiddenIds = new Set();
let revealAll = false;
let separatorMode = 'newline';
let customSeparator = '';
let batchExpanded = true;
let draggedItemId = '';

function allItems() {
  return model.groups.flatMap(group => group.items);
}
function cleanTransientState() {
  const validIds = new Set(allItems().map(item => item.id));
  copyOrder = copyOrder.filter((id, index, list) => validIds.has(id) && list.indexOf(id) === index);
  Array.from(selectedIds).forEach(id => { if (!validIds.has(id) || !copyOrder.includes(id)) selectedIds.delete(id); });
  copyOrder.forEach(id => selectedIds.add(id));
  Array.from(revealedIds).forEach(id => { if (!validIds.has(id)) revealedIds.delete(id); });
  Array.from(hiddenIds).forEach(id => { if (!validIds.has(id)) hiddenIds.delete(id); });
}
function isItemVisible(itemId) {
  if (revealedIds.has(itemId)) return true;
  if (hiddenIds.has(itemId)) return false;
  return revealAll;
}
```

每次 `render()` 首先调用 `cleanTransientState()`；编辑删除条目或分组后的既有 `updateAndRender()` 因此自动清理选择和显隐覆盖。

- [ ] **Step 5：实现选择、排序、显隐与批量复制**

使用以下完整接口：

```javascript
function toggleSelection(itemId, selected) {
  copyOrder = copyOrder.filter(id => id !== itemId);
  if (selected) { selectedIds.add(itemId); copyOrder.push(itemId); }
  else selectedIds.delete(itemId);
  render();
}
function moveSelection(itemId, direction) {
  const index = copyOrder.indexOf(itemId);
  const target = index + direction;
  if (index < 0 || target < 0 || target >= copyOrder.length) return;
  copyOrder.splice(target, 0, copyOrder.splice(index, 1)[0]);
  renderBatchTray();
  renderNavigation(visibleGroups());
  renderItems(visibleGroups());
}
function toggleItemVisibility(itemId) {
  if (isItemVisible(itemId)) {
    revealedIds.delete(itemId);
    hiddenIds.add(itemId);
  } else {
    hiddenIds.delete(itemId);
    revealedIds.add(itemId);
  }
  renderItems(visibleGroups());
}
function toggleAllVisibility() {
  revealAll = !revealAll;
  revealedIds.clear();
  hiddenIds.clear();
  render();
}
async function copyBatch() {
  const result = composeBatch(copyOrder, separatorMode, customSeparator);
  if (!result.includedCount) {
    announce('所选条目没有可复制正文，请补充内容或重新选择。', 'error');
    return;
  }
  const copied = await writeClipboard(result.text);
  if (!copied) { announce('批量复制失败，请手动复制。', 'error'); return; }
  const suffix = result.skippedCount ? `，跳过 ${result.skippedCount} 条空内容` : '';
  announce(`已按当前顺序复制 ${result.includedCount} 条${suffix}。`, 'success');
}
```

`composeBatch` 在生成运行时内实现与核心 `composeBatchText` 相同的查找、去重、跳过空正文和分隔符规则。`writeClipboard(text)` 从现有 `copyItem` 抽取 Clipboard API 与离屏文本域回退，单条复制和批量复制共用它。

- [ ] **Step 6：扩展卡片、两级导航和复制篮渲染**

`createCopyCard(item)` 增加复选框、顺序编号和单条显隐按钮。复选框 `click` 与 `change` 处理器调用 `stopPropagation()`；显隐按钮同样阻止卡片复制事件。正文 `<pre>` 的 `hidden` 属性由 `isItemVisible(item.id)` 决定。

`renderNavigation(groups)` 在每个一级分组链接后追加当前可见条目的二级链接：

```javascript
const itemList = createElement('div', 'nav-item-list');
entry.items.forEach(item => {
  const link = createElement('a', 'nav-item-link');
  link.href = '#item-' + item.id;
  link.append(createElement('span', '', item.title));
  const orderIndex = copyOrder.indexOf(item.id);
  if (orderIndex >= 0) link.append(createElement('span', 'nav-order', String(orderIndex + 1)));
  link.addEventListener('click', event => focusItemFromNavigation(event, item.id));
  itemList.appendChild(link);
});
navigation.appendChild(itemList);
```

`focusItemFromNavigation` 阻止默认事件，关闭移动端导航，调用 `scrollIntoView`，聚焦卡片并临时添加 `.is-targeted`。

`renderBatchTray()` 按 `copyOrder` 创建带 `draggable = true` 的 `<li>`，绑定 `dragstart`、`dragover`、`drop`，并创建上移、下移和移除按钮。`drop` 以被拖 ID 和目标 ID 计算新索引，无效 ID 时保持顺序。

- [ ] **Step 7：运行全部针对性测试**

Run: `node tests/html_paste_gen_test.js`

Expected: PASS，并输出 `HtmlPasteGen core model tests passed`。

Run: `node tests/html_paste_gen_ui_test.js`

Expected: PASS，并输出 `HtmlPasteGen UI contract tests passed`。

- [ ] **Step 8：提交成品页交互增量**

Run: `git add html/HtmlPasteGen.html tests/html_paste_gen_test.js tests/html_paste_gen_ui_test.js`

Run: `git commit -m "feat: add batch copy and nested navigation"`

Expected: 提交只包含目标页面与两份测试。

### Task 3：浏览器与仓库级验证

**Files:**
- Verify: `html/HtmlPasteGen.html`
- Verify: `tests/html_paste_gen_test.js`
- Verify: `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1：运行语法和自动化测试**

Run: `node --check tests/html_paste_gen_test.js`

Expected: exit 0。

Run: `node --check tests/html_paste_gen_ui_test.js`

Expected: exit 0。

Run: `node tests/html_paste_gen_test.js`

Expected: PASS。

Run: `node tests/html_paste_gen_ui_test.js`

Expected: PASS。

- [ ] **Step 2：运行 Linux 构建与空白检查**

Run: `wsl make`

Expected: exit 0。

Run: `git diff --check`

Expected: exit 0。

- [ ] **Step 3：真实浏览器验证批量复制**

生成示例成品页。勾选第一、第二和第三条，取消第二条后重新选择，确认其追加到队尾；用上移按钮和拖动分别调整顺序。依次验证换行、空行、空格、自定义和空自定义分隔符，读取剪贴板并与界面顺序逐字符比较。加入空正文条目后确认状态报告跳过数量。

- [ ] **Step 4：真实浏览器验证显隐和导航**

确认首次加载正文不可见；顶部显示全部后全部可见；单条隐藏只影响该卡；顶部隐藏全部会清除单条覆盖；统一隐藏后单条可再次显示。点击左侧二级条目标题，确认滚动、焦点和短暂高亮；搜索后导航只保留可见子标题，而复制篮选择不丢失。

- [ ] **Step 5：验证编辑同步与响应式布局**

编辑模式永久上移条目并刷新，确认页面顺序持久化；删除已选条目后确认复制篮同步移除。桌面检查复制篮不遮挡最后一组；窄屏检查复制篮可收起、展开且按钮可触达。浏览器控制台预期无 error 或 warning。

- [ ] **Step 6：检查最终范围**

Run: `git status --short`

Expected: 本功能文件无未提交修改；用户原有 `.claude/settings.local.json`、`html/wiki/sqlite_db/pending_logs.jsonl` 与 `nul` 状态保持不变。
