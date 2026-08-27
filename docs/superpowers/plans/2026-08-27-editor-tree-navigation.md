# HtmlPasteGen 真实树形目录导航实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (recommended) to implement this plan task-by-task with review checkpoints. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 HtmlPasteGen 的分组条目升级为最多 4 级的真实递归树，并让编辑器、生成 HTML、复制、搜索和 JSON 网络库共享同一套树操作。

**Architecture:** 在 `core-logic` 中把 schema 升级为 v2，所有条目统一包含 `children` 数组；新增纯树工具返回节点位置、路径和移动结果，避免编辑器与生成页各自实现层级算法。所有 JSON 导入入口先调用 `migrateDocument()`：v1 扁平条目在边界转换为 v2，内部处理与导出只使用 v2。生成页保持自包含，在模板中使用同名递归工具处理导航、卡片、快捷键和批量复制；编辑器通过事件委托调用核心树操作，目录展开状态单独保存在瞬时 Set 中。

**Tech Stack:** Standalone HTML/CSS/JavaScript、Node `assert`/`vm` 契约测试、WSL `make`。

---

### Task 1: 锁定 schema v2、v1 迁移和纯树 API 的失败测试

**Files:**
- Modify: `tests/html_paste_gen_test.js`

- [ ] **Step 1: 更新 schema 预期并加入递归 fixture**

将 sample schema 断言从 `1` 改为 `2`，新增一个包含四级节点的测试文档，并断言每个节点都有数组类型的 `children`；同时保留一个旧版扁平 v1 fixture：

```js
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
```

旧版 fixture 至少包含两个分组、每组两个扁平条目和一个快捷键，用于确认字段、顺序及 ID 在迁移后保持不变。

- [ ] **Step 2: 写入纯树工具的行为断言**

在 `tests/html_paste_gen_test.js` 中加入以下期望 API；这些断言先故意失败，驱动后续实现：

```js
assert.deepStrictEqual(core.flattenItems(treeDocument).map(entry => entry.item.id), ['root', 'child', 'grandchild', 'leaf']);
assert.deepStrictEqual(core.itemPath(treeDocument, 'leaf'), ['树分组', '根', '子', '孙', '叶']);
assert.strictEqual(core.itemDepth(treeDocument, 'root'), 1);
assert.strictEqual(core.itemDepth(treeDocument, 'leaf'), 4);
assert.strictEqual(core.descendantCount(treeDocument.groups[0].items[0]), 3);
assert.strictEqual(core.validateDocument(treeDocument).valid, true);
const migrated = core.migrateDocument(legacyDocument);
assert.strictEqual(migrated.schemaVersion, 2);
assert.deepStrictEqual(migrated.groups[0].items.map(entry => entry.id), ['legacy-a', 'legacy-b']);
assert.deepStrictEqual(migrated.groups[0].items.map(entry => entry.children), [[], []]);
assert.deepStrictEqual(core.migrateDocument(treeDocument), treeDocument);
```

- [ ] **Step 3: 运行测试确认 RED**

运行 `node tests/html_paste_gen_test.js`，预期首先因 `SCHEMA_VERSION` 或 `flattenItems` 尚未实现而失败；失败必须是断言失败，不得是测试语法错误。

### Task 2: 实现 schema v2、递归校验和树工具

**Files:**
- Modify: `html/HtmlPasteGen.html:838-1450`（`core-logic`）
- Modify: `tests/html_paste_gen_test.js`

- [ ] **Step 1: 升级规范对象和构造器**

将 `SCHEMA_VERSION` 改为 `2`，`MAX_ITEM_DEPTH` 为 `4`，`createItem()` 默认返回 `children: []`；`DRAFT_KEY` 使用 `html-paste-gen:draft:v2`。新增 `migrateDocument()` 作为所有 JSON 导入入口的唯一边界：v1 扁平条目递归补齐 `children: []` 并保留字段，v2 深拷贝后返回；未知版本或非法结构返回带节点路径的错误。`normalizeDocument()` 和 `validateDocument()` 只处理迁移后的 v2 文档，沿递归路径生成错误：

```js
const SCHEMA_VERSION = 2;
const MAX_ITEM_DEPTH = 4;

function createItem(overrides) {
  return Object.assign({
    id: newId('item'), title: '新条目', content: '', shortcut: '', link: '', note: '',
    favorite: false, collapsed: false, children: []
  }, overrides || {});
}
```

- [ ] **Step 2: 添加统一递归工具**

在 `moveItem()` 前加入以下职责边界：`walkItems(items, visitor, parent, depth, path)` 只负责遍历；`flattenItems(document)` 返回 `{ item, group, parent, siblings, depth, path }`；`findItemLocation(document, itemId)` 返回同样的位置对象；`itemPath()`、`itemDepth()`、`descendantCount()` 只读取树；`cloneItemTree()` 递归生成新 ID 并清空所有后代快捷键。

层级操作使用位置对象并保证一次性变更：

```js
function indentItem(document, itemId) {
  const location = findItemLocation(document, itemId);
  if (!location || location.depth >= MAX_ITEM_DEPTH || location.index === 0) return { ok: false, reason: '无法再降低层级。' };
  const previous = location.siblings[location.index - 1];
  location.siblings.splice(location.index, 1);
  previous.children.push(location.item);
  return { ok: true, depth: location.depth + 1 };
}

function outdentItem(document, itemId) {
  const location = findItemLocation(document, itemId);
  if (!location || !location.parent) return { ok: false, reason: '根节点不能提升层级。' };
  const parentLocation = findItemLocation(document, location.parent.id);
  location.siblings.splice(location.index, 1);
  parentLocation.siblings.splice(parentLocation.index + 1, 0, location.item);
  return { ok: true, depth: location.depth - 1 };
}
```

同时导出 `MAX_ITEM_DEPTH`、`walkItems`、`flattenItems`、`findItemLocation`、`itemPath`、`itemDepth`、`descendantCount`、`cloneItemTree`、`indentItem`、`outdentItem`。

- [ ] **Step 3: 让校验、规范化和聚合函数递归工作**

用 `walkItems()` 替换快捷键/ID/快速链接校验中的平面循环；`filterDocument()` 保留匹配节点及其祖先；`composeBatchEntries()`、`composeBatchText()`、`appendCustomDocumentItem()` 的查找改用 `flattenItems()`；所有计数函数统计整棵树。为每个错误使用 `path.join(' / ')`，并在四级节点再调用 `indentItem()` 时返回失败而不改动树。

- [ ] **Step 4: 运行核心测试确认 GREEN**

运行 `node tests/html_paste_gen_test.js`，确认 schema v2、四级遍历、路径、深度边界、缩进/反缩进、复制子树、搜索和批量复制断言全部通过。

- [ ] **Step 5: 提交核心模型阶段**

```bash
git add tests/html_paste_gen_test.js html/HtmlPasteGen.html
git commit -m "feat: add recursive tree document model"
```

### Task 3: 将生成 HTML 改为递归导航和递归内容运行时

**Files:**
- Modify: `html/HtmlPasteGen.html:1812-3280`（`generatedShell`）
- Modify: `tests/html_paste_gen_test.js`

- [ ] **Step 1: 添加生成结果的失败契约**

对 `treeDocument` 调用 `core.buildGeneratedHtml()`，增加断言：生成结果的运行时代码包含 `setAttribute('aria-level', String(depth))`、四级递归调用、四个树节点标题、`nav-tree-toggle`、`generated-indent-item`、`generated-outdent-item`，且运行时不再使用 `model.groups.flatMap(group => group.items)` 这种平面查找。不要断言静态 HTML 直接出现 `aria-level="4"`，因为层级值在浏览器渲染时由递归深度计算。

- [ ] **Step 2: 注入生成页树工具并替换平面查找**

在 generated runtime 中增加 `walkItems()`、`flattenItems()`、`findItemLocation()`、`descendantCount()`；让 `allItems()`、快捷键监听、批量选择、过滤、统计、JSON 校验全部使用递归结果。`normalizeImportedDocument()` 先执行与编辑器一致的 v1→v2 迁移，再递归校验；缺失或非数组时报错并包含节点路径。

- [ ] **Step 3: 递归渲染生成页导航和命令卡片**

将 `renderNavigation()` 改为 `renderNavItems(items, group, depth, parentId)`：每个节点输出 `aria-level`、折叠按钮和指向 `#item-<id>` 的链接；导航默认折叠但点击节点自动展开祖先。`renderItems()` 递归渲染同一树的卡片，命令卡片默认全部显示；组/节点计数使用后代统计。

- [ ] **Step 4: 运行生成页测试确认 GREEN**

运行 `node tests/html_paste_gen_test.js`，确认四级导航、所有命令卡片、快捷键复制、批量选择和生成页 JSON 导入导出通过；再运行 `node tests/html_paste_gen_ui_test.js` 确认原有编辑器契约仍存在。

### Task 4: 实现 HtmlPasteGen 编辑器树形渲染与层级按钮

**Files:**
- Modify: `html/HtmlPasteGen.html:431-470, 3280-4580`（CSS 与 `app-logic`）
- Modify: `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1: 添加 UI 失败契约**

在 UI 测试中加入精确契约：

```js
assert.match(html, /indent-item/);
assert.match(html, /outdent-item/);
assert.match(html, /add-child-item/);
assert.match(html, /aria-level/);
assert.match(html, /aria-setsize/);
assert.match(html, /function\s+renderItemTree\s*\(/);
assert.match(html, /function\s+handleTreeAction\s*\(/);
assert.match(html, /MAX_ITEM_DEPTH/);
assert.match(html, /最多 4 级/);
```

- [ ] **Step 2: 增加编辑器树状态和操作适配器**

在 `app-logic` 中增加 `expandedNavNodeIds`，并实现 `resetTreeUiState()`、`expandAncestors(itemId)`、`selectedItemLocation()`；所有 `state.document` 替换、新建、清空、恢复示例和网络库导入后调用重置。`handleTreeAction()` 只负责读取 `data-item-id` 并调用 core 的 `indentItem`/`outdentItem`/`moveItem`，成功后 `updateAfterAction()`，失败只播报 `reason`。

- [ ] **Step 3: 递归渲染左侧导航**

实现 `renderItemTree(items, group, depth, container)`：默认 `expandedNavNodeIds` 为空；有子节点时渲染 `toggle-item-children`，每个节点渲染层级徽标、后代计数、`indent-item`、`outdent-item`、`select-item`，设置 `aria-level`/`aria-setsize`/`aria-posinset`。点击节点标题展开祖先并调用现有 `selectAndFocus()`。

- [ ] **Step 4: 递归渲染编辑卡片和添加入口**

将 `renderEditor()` 的 `group.items.forEach()` 改为递归 `renderEditorItem(item, group, parentItems, depth)`；编辑卡片以 `data-item-id`/`data-parent-id` 标记层级，保留快捷键、备注、快速链接、收藏和正文折叠。增加“添加子条目”和“添加同级条目”动作，分别写入 `item.children` 或当前父数组。

- [ ] **Step 5: 增加可读的层级样式**

添加 `.tree-node`、`.tree-node-children`、`.tree-node-toolbar`、`.tree-level-badge` 和 `.tree-indent-guide` 样式；深度通过 CSS 变量控制缩进，超过面板宽度时标题省略，按钮在窄屏自动换行，不改变左侧面板已有滚动条。

- [ ] **Step 6: 运行 UI 测试确认 GREEN**

运行 `node tests/html_paste_gen_ui_test.js`，确认默认折叠、ARIA 层级、缩进/反缩进动作、子条目添加和 `app-logic` 可解析。

### Task 5: 统一网络库、导入导出和回归行为

**Files:**
- Modify: `html/HtmlPasteGen.html`
- Modify: `tests/html_paste_gen_test.js`
- Modify: `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1: 增加 schema v2 网络 JSON 失败用例**

测试 v1 导入会自动补齐 `children`，以及 v2 导入缺少 `children`、超过 4 级、重复嵌套 ID、重复快捷键的 JSON 时返回节点路径错误；测试合法四级 JSON 可被网络库读取、编辑、导出并保持树结构，且导出结果始终为 v2。

- [ ] **Step 2: 统一编辑器网络库处理**

让 `handleJsonImport()`、`importNetworkJson()`、`saveNetworkJson()`、`saveNetworkBundle()` 统一调用 `core.migrateDocument()` → `core.validateDocument()` → `core.normalizeDocument()`；导入成功后清除 `expandedNavNodeIds`，导出 JSON 保留完整 `children` 和 `schemaVersion: 2`。

- [ ] **Step 3: 运行专项回归测试**

依次运行：

```bash
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
node tests/http_handler_html_paste_test.js
node tests/index_navigation_test.js
node tests/simplewebserver_restart_test.js
node tests/html_paste_bundle_api_test.js
```

### Task 6: 手动验证、构建和提交

**Files:**
- Verify: `html/HtmlPasteGen.html`, `tests/html_paste_gen_test.js`, `tests/html_paste_gen_ui_test.js`
- Commit: all feature files and this plan

- [ ] **Step 1: 浏览器手动回归**

在 `HtmlPasteGen.html` 中新建节点并形成 4 级树；验证默认折叠、展开祖先定位、`＋`/`−` 边界禁用、同级排序、添加子条目、复制/删除子树、搜索、批量复制和生成页全部命令显示。

- [ ] **Step 2: 运行最终验证**

运行 `git diff --check` 和 `wsl make -B`；构建输出必须包含 `Build successful: bin/simplewebserver`，允许记录仓库现有 GCC 警告但不得出现错误。

- [ ] **Step 3: 检查差异并提交**

只暂存 `html/HtmlPasteGen.html`、两个测试文件和本计划；不要暂存 `.claude/settings.local.json`、`html/wiki/sqlite_db/*`、`.port`、`.superpowers/`、`nul` 等既有或伴侣文件。提交信息使用：

```bash
git add -- html/HtmlPasteGen.html tests/html_paste_gen_test.js tests/html_paste_gen_ui_test.js docs/superpowers/plans/2026-08-27-editor-tree-navigation.md
git commit -m "feat: add editable tree navigation"
```
