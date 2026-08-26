# 多选复制篮自定义内容实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task with review checkpoints.

**Goal:** 让生成的 HTML 页面在多选复制篮中支持自定义纯文本片段的插入、排序、编辑、删除、临时复制和可选写入文档。

**Architecture:** 在 `HtmlPasteGenCore` 中提供纯函数形式的序列节点操作与文档追加操作；生成页面运行时维护 `batchEntries` 统一序列，同时保留 `copyOrder` 兼容现有命令多选。自定义片段通过弹窗编辑，提交后加入统一序列，可选择追加到当前文档分组；一键复制统一消费命令和自定义节点。

**Tech Stack:** Standalone HTML/CSS/JavaScript、Node `assert`/`vm` 契约测试、现有浏览器剪贴板回退逻辑。

---

### Task 1: 为核心序列和生成页面补充失败测试

**Files:**
- Modify: `tests/html_paste_gen_test.js`
- Modify: `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1: 写核心模型失败断言**

在 `html_paste_gen_test.js` 中增加以下行为断言：

```js
const custom = { kind: 'custom', id: 'custom-1', title: '前置说明', content: '请先确认环境' };
const entries = core.insertBatchEntry(
  [{ kind: 'item', id: batchItems[0].id }, { kind: 'item', id: batchItems[1].id }],
  custom,
  'after',
  batchItems[0].id
);
assert.deepStrictEqual(entries.map(entry => entry.id), [batchItems[0].id, 'custom-1', batchItems[1].id]);
assert.deepStrictEqual(core.moveBatchEntry(entries, 'custom-1', 1).map(entry => entry.id), [batchItems[0].id, batchItems[1].id, 'custom-1']);
assert.deepStrictEqual(core.removeBatchEntry(entries, 'custom-1').map(entry => entry.id), [batchItems[0].id, batchItems[1].id]);
assert.strictEqual(core.validateCustomBatchContent('  ', '正文').valid, true);
assert.strictEqual(core.validateCustomBatchContent('标题', '  \n ').valid, false);
const customBatch = core.composeBatchEntries(batchDoc, entries, 'newline', '');
assert.strictEqual(customBatch.text, `${batchItems[0].content}\n请先确认环境\n${batchItems[1].content}`);
assert.deepStrictEqual(Array.from(customBatch.includedIds), [batchItems[0].id, 'custom-1', batchItems[1].id]);
const appended = core.appendCustomDocumentItem(batchDoc, batchDoc.groups[0].id, '文档片段', '持久化正文');
assert.strictEqual(appended.valid, true);
assert.strictEqual(appended.document.groups[0].items.at(-1).content, '持久化正文');
```

- [ ] **Step 2: 写 UI 模板失败断言**

在 `html_paste_gen_ui_test.js` 增加对 `custom-content-button`、`custom-content-dialog`、`custom-content-title`、`custom-content-text`、`custom-content-position`、`custom-save-to-document`、`custom-document-group`、`custom-document-title`、`custom-submit-button` 的 ID 断言，并检查源码包含 `batchEntries`、`insertBatchEntry`、`composeBatchEntries`、`加入并复制`、`保存到文档`、`function openCustomContentDialog`、`function renderCustomBatchEntry`。

- [ ] **Step 3: 运行测试确认 RED**

运行：

```powershell
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
```

预期：测试因核心函数和自定义弹窗控件尚不存在而失败。

### Task 2: 实现核心序列与文档追加函数

**Files:**
- Modify: `html/HtmlPasteGen.html` `core-logic` 脚本

- [ ] **Step 1: 添加节点规范化和插入函数**

在 `uniqueIds` 附近增加：

```js
function normalizeBatchEntries(entries) { /* 过滤无效节点并按 id 去重 */ }
function insertBatchEntry(entries, entry, position, anchorId) { /* start/after/end 转索引 */ }
function moveBatchEntry(entries, entryId, direction) { /* 统一节点上下移动 */ }
function removeBatchEntry(entries, entryId) { /* 返回删除指定节点后的新数组 */ }
```

函数必须返回新数组，不修改调用者传入的数组；`after` 找不到锚点时使用末尾；空内容自定义节点不得进入序列。

- [ ] **Step 2: 添加统一拼接和校验函数**

实现 `validateCustomBatchContent(title, content)` 和 `composeBatchEntries(value, entries, separatorMode, customSeparator)`。拼接结果返回 `text`、`includedIds`、`includedCount`、`skippedCount`、`missingCount`；命令节点按文档 ID 查找，自定义节点按 `content` 取值，空值跳过。

- [ ] **Step 3: 添加文档追加函数并导出 API**

实现 `appendCustomDocumentItem(value, groupId, title, content)`：克隆文档、校验目标分组和非空标题/正文，使用 `createItem` 生成无快捷键、无链接、非收藏的新条目，返回 `{ valid, document, item, error }`。将新增函数加入 `window.HtmlPasteGenCore` 导出对象。

- [ ] **Step 4: 运行核心测试确认 GREEN**

运行 `node tests/html_paste_gen_test.js`，预期核心模型测试通过。

### Task 3: 接入生成页面的统一复制篮状态

**Files:**
- Modify: `html/HtmlPasteGen.html` 生成页面运行时脚本和复制篮模板

- [ ] **Step 1: 增加统一序列状态和兼容同步**

新增 `let batchEntries = [];`，为命令选择维护 `{ kind: 'item', id }` 节点；保留 `copyOrder` 作为命令 ID 派生列表。调整 `toggleSelection`、`selectVisibleItems`、`toggleGroupSelection`、`moveSelection`、拖动排序、清空、导入和恢复逻辑，使自定义节点在普通命令选择操作中保留，在导入/恢复/清空时清除。

- [ ] **Step 2: 改造批量拼接和空篮判断**

将运行时 `composeBatch` 改为消费 `batchEntries` 并调用 `core.composeBatchEntries`；复制篮显示条件改为“存在命令或有效自定义节点”，一键复制将 `includedIds` 中的命令 ID 传给 `markCopiedCards`，自定义节点只显示序列成功状态。

- [ ] **Step 3: 改造渲染与排序操作**

`renderBatchTray` 按统一序列渲染命令节点和自定义节点；两类节点均提供上移、下移和删除，命令节点继续显示原标题，自定义节点显示“自定义”标记与片段标题。拖动事件使用节点 ID，不能把自定义节点加入 `selectedIds`。

- [ ] **Step 4: 运行现有核心与 UI 测试**

运行：

```powershell
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
```

预期：统一序列测试和既有多选/拖动契约均通过。

### Task 4: 实现自定义内容弹窗与临时加入

**Files:**
- Modify: `html/HtmlPasteGen.html` 生成页面 CSS、复制篮 HTML 模板和运行时脚本

- [ ] **Step 1: 添加弹窗和响应式样式**

在复制篮附近添加 `custom-content-dialog`，包含遮罩、标题、显示标题输入、正文 textarea、位置 select、保存到文档复选框、目标分组 select、文档条目标题输入、“加入复制篮”“加入并复制”“取消”按钮。弹窗使用 `role="dialog"`、`aria-modal="true"`，桌面居中、窄屏底部抽屉。

- [ ] **Step 2: 添加打开、关闭和表单联动**

实现 `openCustomContentDialog(entry)`、`closeCustomContentDialog()`、`updateCustomDocumentFields()`；打开时位置选项根据当前 `batchEntries` 的命令节点生成，关闭时恢复触发按钮焦点，Escape 和遮罩可关闭。勾选保存时显示目标分组与文档标题字段。

- [ ] **Step 3: 实现加入和编辑流程**

提交时调用 `core.validateCustomBatchContent`；根据位置调用 `core.insertBatchEntry`。编辑已有自定义节点时使用原 ID 替换内容，不重复插入；空内容阻止提交并保留输入。保存到文档先调用 `core.appendCustomDocumentItem`，成功后替换运行时文档并保存草稿；失败保留弹窗和复制篮节点。

- [ ] **Step 4: 接入操作按钮和快捷状态**

“＋自定义内容”打开新片段，“编辑”复用同一弹窗；“加入并复制”提交成功后调用 `copyBatch`。自定义节点不参与命令快捷键查找、已选数量和组全选状态。

- [ ] **Step 5: 运行 UI 契约和脚本语法测试**

运行 `node tests/html_paste_gen_ui_test.js` 与 `node tests/html_paste_gen_test.js`，预期全部通过。

### Task 5: 完整验证、代码审查与交付

**Files:**
- Verify: `html/HtmlPasteGen.html`, `tests/html_paste_gen_test.js`, `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1: 运行完整检查**

```powershell
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
node tests/http_handler_html_paste_test.js
node tests/index_navigation_test.js
node tests/simplewebserver_restart_test.js
git diff --check
wsl make
```

- [ ] **Step 2: 做浏览器烟雾测试**

打开 `HtmlPasteGen.html` 生成的页面，选择两个命令，加入自定义片段到开头、命令之后和末尾，确认复制文本顺序；编辑/上下移动/删除片段；勾选“保存到文档”并确认新增条目；验证 Escape 关闭弹窗。只操作临时测试内容，不覆盖用户网络库文件。

- [ ] **Step 3: 代码审查与修复**

请 reviewer 检查统一序列与 `copyOrder` 同步、文档写入边界、XSS/纯文本处理、键盘焦点和移动端布局；Critical/Important 问题修复后重新运行完整检查。

- [ ] **Step 4: 提交并推送**

仅提交功能文件、测试和本规格/计划文档，保留现有无关工作区改动：

```powershell
git commit -m "feat: add custom batch content insertion"
git push origin codex/register-viewer-auto-parse
```
