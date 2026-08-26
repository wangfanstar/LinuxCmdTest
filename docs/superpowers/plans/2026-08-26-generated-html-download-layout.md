# 生成 HTML 下载与顶部布局修复实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task with review checkpoints. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在生成 HTML 的固定顶部工具栏中增加本地下载入口，并确保页面正文、导航跳转和编辑区不被工具栏遮挡。

**Architecture:** 复用现有 `exportCurrentHtml()`，只在生成页 `hero-actions` 增加按钮和事件绑定。继续使用 `generated-toolbar-slot` 作为占位，但把工具栏实际高度加上安全间距同步到 CSS 变量，并使用 `scroll-padding-top`/`scroll-margin-top` 处理浏览器滚动与锚点定位。

**Tech Stack:** Standalone HTML/CSS/JavaScript、Node `assert`/`vm` 契约测试、WSL `make`。

---

### Task 1: 添加失败回归测试

**Files:**
- Modify: `tests/html_paste_gen_test.js`
- Modify: `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1: 验证下载按钮契约**

断言生成结果包含 `download-current-html-button`，且生成运行时绑定该按钮并调用 `exportCurrentHtml`。

- [ ] **Step 2: 验证顶部遮挡防护契约**

断言生成模板包含 `--generated-toolbar-height`、`scroll-padding-top`、`scroll-margin-top`，并且工具栏占位高度同步时包含额外安全间距。

- [ ] **Step 3: 运行测试确认 RED**

运行 `node tests/html_paste_gen_test.js` 和 `node tests/html_paste_gen_ui_test.js`，预期因新按钮和滚动防护尚不存在而失败。

### Task 2: 实现下载入口与顶部安全空间

**Files:**
- Modify: `html/HtmlPasteGen.html`

- [ ] **Step 1: 增加固定工具栏下载按钮**

在生成页 `hero-actions` 加入 `download-current-html-button`，绑定到现有 `exportCurrentHtml`，不复制序列化逻辑。

- [ ] **Step 2: 同步工具栏真实高度**

让 `syncGeneratedToolbarSpace()` 将工具栏高度加上 16px 安全间距后写入 `--generated-toolbar-height` 和 `generated-toolbar-slot`，保留 ResizeObserver 与 resize 监听，覆盖按钮换行场景。

- [ ] **Step 3: 添加滚动偏移防遮挡**

在生成页 CSS 中增加 `html { scroll-padding-top: ... }`，并给 `.group-section`、`.copy-card`、`.editor-panel` 设置 `scroll-margin-top`，确保普通锚点与脚本滚动落在工具栏下方。

### Task 3: 验证并交付

**Files:**
- Verify: `html/HtmlPasteGen.html`, `tests/html_paste_gen_test.js`, `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1: 运行全量检查**

运行核心/UI/网络 API/索引/重启测试、`git diff --check` 与 `wsl make`。

- [ ] **Step 2: 更新计划状态并提交**

仅提交功能文件、测试与本计划，保留工作区既有无关改动；提交信息为 `feat: add generated HTML local download`。

