# HtmlPasteGen 可调宽度工作台 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax and must be executed in order.

**Goal:** 扩大 HtmlPasteGen 桌面工作台及左侧导航，并提供可持久化的鼠标/键盘宽度调整。

**Architecture:** 在现有三栏 `.workspace` 中插入一个无业务副作用的垂直分隔按钮，由 CSS Grid 自定义属性控制左栏宽度。应用逻辑只维护一个经过边界校验的宽度值，统一驱动拖拽、双击重置、键盘调整、ARIA 状态和 localStorage 持久化；窄屏继续使用现有单栏切换逻辑。

**Tech Stack:** Standalone HTML/CSS/JavaScript、Pointer Events、CSS Grid、自定义属性、localStorage、Node.js `assert`/`vm` 契约测试、Codex 浏览器、本地 Linux/WLS 构建。

---

## 文件结构

- Modify: `tests/html_paste_gen_ui_test.js` — 增加工作台宽度、分隔条语义、边界及持久化契约；替换旧三栏 CSS 断言。
- Modify: `html/HtmlPasteGen.html` — 调整 `.app-shell`/`.workspace` 样式和媒体查询，插入可访问分隔条，实现宽度状态、Pointer Events、键盘和 localStorage 控制。
- Create: `docs/superpowers/specs/2026-08-28-html-paste-gen-resizable-layout-design.md` — 已确认的设计规格（已提交）。
- Create: `docs/superpowers/plans/2026-08-28-html-paste-gen-resizable-layout.md` — 本实施计划。

不修改 JSON schema、生成 HTML 模板、网络库 API 或移动端面板数据结构。

### Task 1: 用 UI 契约测试锁定布局和交互接口

**Files:**
- Modify: `tests/html_paste_gen_ui_test.js`，现有 `.workspace` 及布局契约附近。

- [ ] **Step 1: 写出失败测试**

在现有 `assert.match(html, /class="workspace"/)` 附近加入以下断言，并删除旧的固定三栏断言 `grid-template-columns: minmax(170px, .64fr)...`：

```js
assert.match(html, /.app-shell\s*\{[\s\S]*width:\s*min\(1880px,\s*calc\(100%\s*-\s*32px\)\)/);
assert.match(html, /--structure-width:\s*330px/);
assert.match(html, /grid-template-columns:\s*minmax\(260px,\s*var\(--structure-width[^)]*\)\)\s+8px\s+minmax\(560px,\s*1\.7fr\)\s+minmax\(320px,\s*1fr\)/);
assert.match(html, /id=["']workspace-resizer["']/);
assert.match(html, /workspace-resizer[\s\S]*aria-label=["']调整左侧导航宽度["']/);
assert.match(html, /workspace-resizer[\s\S]*aria-valuemin=["']260["']/);
assert.match(html, /workspace-resizer[\s\S]*aria-valuemax=["']520["']/);
assert.match(html, /workspace-resizer[\s\S]*aria-valuenow=["']330["']/);
assert.match(html, /workspace-resizer[\s\S]*aria-orientation=["']vertical["']/);
assert.match(html, /STRUCTURE_WIDTH_KEY/);
assert.match(html, /STRUCTURE_WIDTH_DEFAULT/);
assert.match(html, /function\s+clampStructureWidth\s*\(/);
assert.match(html, /function\s+readStructureWidth\s*\(/);
assert.match(html, /function\s+setStructureWidth\s*\(/);
assert.match(html, /function\s+startWorkspaceResize\s*\(/);
assert.match(html, /setPointerCapture/);
assert.match(html, /pointermove/);
assert.match(html, /pointerup/);
assert.match(html, /pointercancel/);
assert.match(html, /dblclick/);
assert.match(html, /ArrowLeft|ArrowRight/);
assert.match(html, /event\.key\s*===\s*['"]Home['"]/);
assert.match(html, /event\.key\s*===\s*['"]End['"]/);
assert.match(html, /localStorage\.getItem\(STRUCTURE_WIDTH_KEY/);
assert.match(html, /localStorage\.setItem\(STRUCTURE_WIDTH_KEY/);
assert.match(html, /@media\s*\(max-width:\s*960px\)[\s\S]*workspace-resizer[\s\S]*display:\s*none/);
```

- [ ] **Step 2: 运行测试并确认 RED**

运行：

```bash
node tests/html_paste_gen_ui_test.js
```

预期：失败在第一个缺失的工作台宽度/分隔条契约，而不是语法错误；现有测试文件仍可被 Node 读取。

- [ ] **Step 3: 提交失败测试**

```bash
git add tests/html_paste_gen_ui_test.js
git commit -m "test: specify resizable HtmlPasteGen layout"
```

### Task 2: 实现桌面宽度、可拖拽分隔条和持久化状态

**Files:**
- Modify: `html/HtmlPasteGen.html:36-70` — `.app-shell` 和 `.workspace` 样式。
- Modify: `html/HtmlPasteGen.html:624-642` — 桌面窄屏媒体查询。
- Modify: `html/HtmlPasteGen.html:809-850` — 工作台 DOM 中插入分隔条。
- Modify: `html/HtmlPasteGen.html` 的 `app-logic` 初始化区域和事件绑定。

- [ ] **Step 1: 扩大工作台并加入四列网格**

将 `.app-shell` 宽度改为：

```css
.app-shell {
  width: min(1880px, calc(100% - 32px));
  margin: 0 auto;
  padding: 22px 0 44px;
}
```

将 `.workspace` 改为带宽度变量和分隔列的网格：

```css
.workspace {
  --structure-width: 330px;
  display: grid;
  grid-template-columns: minmax(260px, var(--structure-width)) 8px minmax(560px, 1.7fr) minmax(320px, 1fr);
  gap: 14px 10px;
  align-items: start;
  margin-top: 14px;
}

.workspace.is-resizing,
.workspace.is-resizing * { user-select: none; }

.workspace-resizer {
  align-self: stretch;
  min-height: 420px;
  width: 8px;
  padding: 0;
  border: 0;
  border-radius: 99px;
  background: transparent;
  cursor: col-resize;
  touch-action: none;
}

.workspace-resizer::before {
  display: block;
  width: 3px;
  height: 100%;
  margin: 0 auto;
  border-radius: inherit;
  background: var(--line-strong);
  content: "";
  transition: background .14s ease, width .14s ease;
}

.workspace-resizer:hover::before,
.workspace-resizer:focus-visible::before,
.workspace.is-resizing .workspace-resizer::before {
  width: 5px;
  background: var(--primary);
}

.workspace-resizer:focus-visible {
  outline: 3px solid rgba(79, 70, 229, .22);
  outline-offset: 2px;
}
```

- [ ] **Step 2: 保持中等宽度和窄屏安全下限**

在现有 `@media (max-width: 1180px)` 中，将 workspace 覆盖改为：

```css
.workspace {
  grid-template-columns: minmax(240px, min(var(--structure-width), 32vw)) 8px minmax(0, 1.55fr) minmax(280px, .95fr);
}
```

在现有 `@media (max-width: 960px)` 的 block 布局中加入：

```css
.workspace-resizer { display: none; }
```

保留现有 900px 面板切换和 620px 页面边距规则；不要让桌面宽度变量造成移动端横向滚动。

- [ ] **Step 3: 插入可访问分隔条**

在结构导航 `<aside>` 结束、编辑 `<section id="editor-panel">` 开始之前加入：

```html
<button id="workspace-resizer"
        class="workspace-resizer"
        type="button"
        aria-label="调整左侧导航宽度"
        title="拖动调整左侧导航宽度；双击恢复默认"
        aria-orientation="vertical"
        aria-valuemin="260"
        aria-valuemax="520"
        aria-valuenow="330"></button>
```

分隔条不设置 `data-action`，避免被现有编辑器事件委托当作内容操作；所有行为由专用初始化函数绑定。

- [ ] **Step 4: 写入宽度状态和边界工具**

在 `app-logic` 的 `DRAFT_KEY` 常量附近加入：

```js
const STRUCTURE_WIDTH_KEY = 'html-paste-gen:structure-width:v1';
const STRUCTURE_WIDTH_DEFAULT = 330;
const STRUCTURE_WIDTH_MIN = 260;
const STRUCTURE_WIDTH_MAX = 520;
const STRUCTURE_WIDTH_STEP = 16;
const STRUCTURE_WIDTH_LARGE_STEP = 80;
let structureResizeActive = false;

function clampStructureWidth(value) {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) return STRUCTURE_WIDTH_DEFAULT;
  return Math.min(STRUCTURE_WIDTH_MAX, Math.max(STRUCTURE_WIDTH_MIN, Math.round(numeric)));
}

function readStructureWidth() {
  try {
    return clampStructureWidth(localStorage.getItem(STRUCTURE_WIDTH_KEY));
  } catch (error) {
    return STRUCTURE_WIDTH_DEFAULT;
  }
}

function saveStructureWidth(value) {
  try {
    localStorage.setItem(STRUCTURE_WIDTH_KEY, String(clampStructureWidth(value)));
  } catch (error) {
    // 布局偏好不可保存时仍保留本次会话的视觉调整。
  }
}

function setStructureWidth(value, message) {
  const workspace = document.querySelector('.workspace');
  const resizer = byId('workspace-resizer');
  if (!workspace || !resizer) return STRUCTURE_WIDTH_DEFAULT;
  const width = clampStructureWidth(value);
  workspace.style.setProperty('--structure-width', width + 'px');
  resizer.setAttribute('aria-valuenow', String(width));
  if (message) announce(message, 'info');
  return width;
}

function persistStructureWidth(value, message) {
  const width = setStructureWidth(value, message);
  saveStructureWidth(width);
  return width;
}
```

`readStructureWidth` 必须把 `null`、非数字、Infinity 和越界值统一收敛到合法宽度；任何 localStorage 异常都不能阻止页面初始化。

- [ ] **Step 5: 实现鼠标拖拽、双击和键盘控制**

在 `setStructureWidth` 后加入以下控制器，并确保 Pointer Events 不可用时只禁用拖动、保留已保存宽度及键盘/双击操作：

```js
function updateStructureWidthFromPointer(event) {
  if (!structureResizeActive) return;
  const workspace = document.querySelector('.workspace');
  if (!workspace) return;
  const rect = workspace.getBoundingClientRect();
  setStructureWidth(event.clientX - rect.left);
}

function finishStructureResize(event) {
  if (!structureResizeActive) return;
  structureResizeActive = false;
  const workspace = document.querySelector('.workspace');
  const resizer = byId('workspace-resizer');
  workspace?.classList.remove('is-resizing');
  if (resizer && resizer.hasPointerCapture?.(event.pointerId)) resizer.releasePointerCapture(event.pointerId);
  const width = Number(resizer?.getAttribute('aria-valuenow')) || STRUCTURE_WIDTH_DEFAULT;
  saveStructureWidth(width);
}

function startWorkspaceResize(event) {
  if (typeof window.PointerEvent === 'undefined') return;
  const workspace = document.querySelector('.workspace');
  const resizer = byId('workspace-resizer');
  if (!workspace || !resizer) return;
  event.preventDefault();
  structureResizeActive = true;
  workspace.classList.add('is-resizing');
  resizer.setPointerCapture?.(event.pointerId);
  updateStructureWidthFromPointer(event);
}

function initializeStructureResizer() {
  const workspace = document.querySelector('.workspace');
  const resizer = byId('workspace-resizer');
  if (!workspace || !resizer) return;
  setStructureWidth(readStructureWidth());
  resizer.addEventListener('pointerdown', startWorkspaceResize);
  resizer.addEventListener('pointermove', updateStructureWidthFromPointer);
  resizer.addEventListener('pointerup', finishStructureResize);
  resizer.addEventListener('pointercancel', finishStructureResize);
  resizer.addEventListener('dblclick', () => {
    persistStructureWidth(STRUCTURE_WIDTH_DEFAULT, '左侧导航已恢复默认宽度。');
  });
  resizer.addEventListener('keydown', event => {
    const current = Number(resizer.getAttribute('aria-valuenow')) || STRUCTURE_WIDTH_DEFAULT;
    const step = event.shiftKey ? STRUCTURE_WIDTH_LARGE_STEP : STRUCTURE_WIDTH_STEP;
    let next = current;
    if (event.key === 'ArrowLeft') next = current - step;
    else if (event.key === 'ArrowRight') next = current + step;
    else if (event.key === 'Home') next = STRUCTURE_WIDTH_MIN;
    else if (event.key === 'End') next = STRUCTURE_WIDTH_MAX;
    else return;
    event.preventDefault();
    persistStructureWidth(next, `左侧导航宽度：${clampStructureWidth(next)}px。`);
  });
}
```

`pointermove` 只更新 CSS 和 ARIA，不在每个像素移动时写 localStorage；结束事件统一持久化最终值。分隔条初始化调用放在现有 DOM 事件绑定完成、首次 `renderAll()` 之前或之后均可，但必须在首屏可见前执行一次。

- [ ] **Step 6: 运行 UI 测试确认 GREEN**

运行：

```bash
node tests/html_paste_gen_ui_test.js
```

预期：`HtmlPasteGen UI contract tests passed`，退出码 0。

- [ ] **Step 7: 提交布局实现**

```bash
git add html/HtmlPasteGen.html tests/html_paste_gen_ui_test.js
git commit -m "feat: add resizable HtmlPasteGen workspace"
```

### Task 3: 回归、真实页面验收和交付

**Files:**
- Verify: `html/HtmlPasteGen.html`
- Verify: all existing test files under `tests/`
- Do not stage unrelated existing worktree changes (`.claude/settings.local.json`, `html/wiki/sqlite_db/pending_logs.jsonl`, `.port`, `.superpowers/`, `nul`).

- [ ] **Step 1: 运行完整自动化回归**

并行或依次运行：

```bash
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
node tests/http_handler_html_paste_test.js
node tests/index_navigation_test.js
node tests/simplewebserver_restart_test.js
node tests/html_paste_bundle_api_test.js
git diff --check HEAD~2..HEAD
wsl make -B
```

预期：六项 Node 测试退出码均为 0，`git diff --check` 无输出，WSL 以 `Build successful: bin/simplewebserver` 结束；现有 C/Wiki 警告若出现，记录为既有警告，不引入 HTML/JavaScript 相关编译问题。

- [ ] **Step 2: 重启服务并做 HTTP 健康检查**

运行：

```bash
wsl ./simplewebserver.sh restart -p 8881
```

随后确认 `http://127.0.0.1:8881/HtmlPasteGen.html` 返回 HTTP 200，响应包含 `workspace-resizer`、`--structure-width` 和 `调整左侧导航宽度`。

- [ ] **Step 3: 真实浏览器验收桌面交互**

在浏览器打开 `http://127.0.0.1:8881/HtmlPasteGen.html`，只操作布局偏好，不修改内容数据：

1. 确认工作台整体更宽，左栏默认约 330px，分隔条有清晰焦点样式。
2. 用鼠标拖到较窄和较宽位置，确认左栏在 260–520px 内变化，编辑区和预览区仍可见。
3. 双击分隔条，确认宽度恢复默认。
4. 聚焦分隔条，使用左右方向键、Shift+方向键、Home、End，确认 `aria-valuenow` 和视觉宽度同步。
5. 刷新页面，确认最后一次宽度仍保留；确认分组、条目内容和展开状态未被改变。
6. 在长目录中滚动左栏，确认只滚动左栏，不遮挡编辑区。
7. 缩小窗口到 960px 以下，确认分隔条隐藏、页面恢复单栏且无横向滚动。
8. 读取控制台，确认无新增 error/warning。

- [ ] **Step 4: 复核范围并推送当前分支**

运行：

```bash
git status --short
git log --oneline -5
git diff --check HEAD~2..HEAD
git push -u origin codex/register-viewer-auto-parse
gh pr view 1 --repo wangfanstar/LinuxCmdTest --json number,url,state,headRefName,commits
```

预期：功能提交已推送到现有分支和 PR #1；无关工作区变更仍保持原状，未被 stage 或 commit。
