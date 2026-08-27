# HtmlPasteGen 编辑器子条目导航实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (recommended) to implement this plan task-by-task with review checkpoints. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 HtmlPasteGen 编辑器左侧分组导航增加默认折叠的子条目列表，并支持一键定位到具体编辑条目。

**Architecture:** 在 `app-logic` 中使用瞬时 `expandedNavGroupIds` 集合保存展开状态，`renderGroupList()` 为每个分组渲染展开按钮和子条目按钮；通过现有 `handleAction` 事件委托处理切换与定位，`selectAndFocus()` 负责打开编辑区并聚焦条目标题。CSS 将子列表放在分组行下方并提供层级缩进，文档 JSON 不增加字段。

**Tech Stack:** Standalone HTML/CSS/JavaScript、Node `assert`/`vm` 契约测试、WSL `make`。

---

### Task 1: 添加失败 UI 契约测试

**Files:**
- Modify: `tests/html_paste_gen_ui_test.js`

- [x] **Step 1: 写入子条目导航契约断言**

在现有 UI 契约断言后加入以下检查，锁定可访问的 DOM/动作契约：

```js
assert.match(html, /group-item-list/);
assert.match(html, /group-item-link/);
assert.match(html, /toggle-group-items/);
assert.match(html, /select-item/);
assert.match(html, /expandedNavGroupIds/);
assert.match(html, /aria-expanded/);
assert.match(html, /aria-controls/);
assert.match(html, /默认折叠/);
```

- [x] **Step 2: 运行测试确认 RED**

运行 `node tests/html_paste_gen_ui_test.js`，预期因 HtmlPasteGen.html 尚未包含上述子导航契约而失败；确认失败来自缺少功能字符串而不是测试语法错误。

### Task 2: 实现编辑器子条目导航

**Files:**
- Modify: `html/HtmlPasteGen.html`

- [x] **Step 1: 增加瞬时展开状态与重置辅助函数**

在 `expandedPreviewIds` 附近增加 `const expandedNavGroupIds = new Set();` 和 `resetGroupNavExpansion()`，后者清空集合；在文档替换、导入、清空和恢复示例的路径调用它，保证默认折叠。

- [x] **Step 2: 扩展左侧导航渲染**

将 `renderGroupList()` 的分组行改为“展开按钮 + 分组选择 + 原有操作”三列，并追加 `group-item-list`。仅当 `expandedNavGroupIds.has(group.id)` 时移除 `hidden`，每个条目渲染 `select-item` 按钮、序号和标题；空分组显示“暂无子条目”。按钮设置 `aria-expanded`、`aria-controls`、`aria-label` 和 `title`。

- [x] **Step 3: 接入事件委托和条目定位**

在 `handleAction()` 中优先处理 `toggle-group-items` 和 `select-item`。切换动作更新集合并重绘；选择动作将分组加入展开集合，再调用 `selectAndFocus(groupId, itemId)`。在 `selectAndFocus()` 中对带 `itemId` 的定位同样确保分组展开。

- [x] **Step 4: 增加层级导航样式**

调整 `.group-row` 为三列网格，新增 `.group-toggle`、`.group-item-list`、`.group-item-link`、`.group-item-number` 和 `.group-item-name` 样式；子列表跨列、带左侧层级线、标题超长省略，并在 `[hidden]` 时完全隐藏。

### Task 3: 验证并提交

**Files:**
- Verify: `html/HtmlPasteGen.html`, `tests/html_paste_gen_ui_test.js`
- Commit: `docs/superpowers/specs/2026-08-27-editor-sidebar-child-navigation-design.md`, `docs/superpowers/plans/2026-08-27-editor-sidebar-child-navigation.md`, `html/HtmlPasteGen.html`, `tests/html_paste_gen_ui_test.js`

- [x] **Step 1: 运行专项与全量验证**

依次运行 `node tests/html_paste_gen_ui_test.js`、`node tests/html_paste_gen_test.js`、`node tests/http_handler_html_paste_test.js`、`node tests/index_navigation_test.js`、`node tests/simplewebserver_restart_test.js`、`node tests/html_paste_bundle_api_test.js`、`git diff --check` 和 `wsl make`，全部通过后再交付。

- [x] **Step 2: 检查差异并提交**

确认 `git diff --stat` 和 `git diff --check` 只包含本次文档、UI 和测试文件；不要暂存 `.claude/settings.local.json`、`html/wiki/sqlite_db/*`、`.port`、`nul` 等工作区既有改动。提交信息使用 `feat: add editor child navigation`。
