# HtmlPasteGen 网络库与 HTML 预览实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 为 `HtmlPasteGen.html` 增加服务器 `html/html_paste/` 网络库的 JSON 导入导出和 HTML 预览/打开能力，并保持本地文件能力与现有生成页行为不变。

**Architecture:** 在 `src/http_handler.c` 增加只允许单层 `.json`/`.html` 文件的网络库 API；JSON 读取与保存由 API 负责传输和原子写入，页面继续复用既有规范化/校验逻辑。编辑器新增网络库面板，独立维护清单和预览状态，生成的独立 HTML 不调用服务器 API。

**Tech Stack:** C11/POSIX + Windows-compatible platform helpers, vanilla JavaScript/HTML/CSS, Node assertion tests, existing `make` build.

---

### Task 1: 添加服务器 API 的失败契约测试

**Files:**
- Modify: `tests/http_handler_html_paste_test.js`
- Test target: `src/http_handler.c`

- [ ] **Step 1: Write the failing static contracts**

创建 Node 测试读取 `src/http_handler.c`，断言源码包含 `/api/html-paste/list`、`/api/html-paste/read`、`/api/html-paste/save`，固定目录 `html_paste`，扩展名 `.json`/`.html`，`409` 冲突、`overwrite` 字段、`rename` 原子写入和拒绝 `..`/路径分隔符的安全判断。

- [ ] **Step 2: Run the test to verify it fails**

Run: `node tests/http_handler_html_paste_test.js`

Expected: FAIL because the three routes and handlers do not exist yet.

- [ ] **Step 3: Commit the RED test**

```bash
git add tests/http_handler_html_paste_test.js
git commit -m "test: specify html paste network APIs"
```

### Task 2: 实现 `html_paste` 安全 API

**Files:**
- Modify: `src/http_handler.c`
- Test: `tests/http_handler_html_paste_test.js`

- [ ] **Step 1: Add bounded path and extension helpers**

实现 `html_paste_name_safe(name, extensionMode)`：只接受单层非空文件名，拒绝 `.`、`..`、`/`、`\\`、控制字符、隐藏文件和超长名称；列表模式只接受 `.json`/`.html`，读取/保存模式分别只接受 `.json`。所有路径只由 `WEB_ROOT`, `"/html_paste/"`, 和已校验文件名拼接。

- [ ] **Step 2: Implement list handler**

扫描 `html/html_paste` 普通文件，读取 `stat` 的大小和修改时间，按修改时间倒序写出 JSON `{ok:true,files:[...]}`，文件 type 为 `json` 或 `html`；目录不存在时返回空列表并尝试创建目录。

- [ ] **Step 3: Implement JSON read handler**

从查询串读取 `name`，校验 `.json` 文件名和普通文件，限制 `st_size <= 1 MiB`，读取完整 UTF-8 原文并返回 `application/json` 响应；参数、大小或读取失败返回明确的 4xx/5xx JSON 错误。

- [ ] **Step 4: Implement JSON save handler**

从请求体读取 `name`, `content`, `overwrite`，复用现有 `json_get_str`/`json_get_int` 辅助解析；限制名称、正文不超过 1 MiB。目标存在且 `overwrite` 非真时返回 409；否则写入 `html/html_paste/.<name>.<pid>.tmp`，`fwrite` 成功后 `rename` 到目标，失败清理临时文件并返回 500。

- [ ] **Step 5: Wire GET/POST routing and run the test**

将 GET 路由接入 list/read，POST 路由接入 save，并为网络库保存设置 1 MiB body 上限。运行 `node tests/http_handler_html_paste_test.js`，预期 PASS。

- [ ] **Step 6: Commit the API**

```bash
git add src/http_handler.c tests/http_handler_html_paste_test.js
git commit -m "feat: add html paste network library APIs"
```

### Task 3: 添加编辑器网络库的失败契约测试

**Files:**
- Modify: `tests/html_paste_gen_test.js`
- Modify: `tests/html_paste_gen_ui_test.js`
- Test target: `html/HtmlPasteGen.html`

- [ ] **Step 1: Add core/generated source assertions**

在现有生成页契约中加入网络库 API 常量、`loadNetworkLibrary`, `importNetworkJson`, `saveNetworkJson`, `renderNetworkLibrary`, `previewNetworkHtml` 与错误状态文本断言；确认独立生成 HTML 不包含网络库面板调用。

- [ ] **Step 2: Add UI IDs and interaction assertions**

断言编辑器包含 `network-library-panel`, `network-refresh-button`, `network-search`, `network-type-filter`, `network-file-list`, `network-preview`, `network-import-json-button`, `network-export-json-button`, `network-overwrite-checkbox`，以及“覆盖保存”“预览”“新窗口打开”等文案。

- [ ] **Step 3: Run tests to verify RED**

Run: `node tests/html_paste_gen_test.js; node tests/html_paste_gen_ui_test.js`

Expected: FAIL because the network library UI and functions do not exist.

- [ ] **Step 4: Commit the RED tests**

```bash
git add tests/html_paste_gen_test.js tests/html_paste_gen_ui_test.js
git commit -m "test: specify html paste network library UI"
```

### Task 4: 实现编辑器网络库面板与交互

**Files:**
- Modify: `html/HtmlPasteGen.html`
- Test: `tests/html_paste_gen_test.js`, `tests/html_paste_gen_ui_test.js`

- [ ] **Step 1: Add panel markup and responsive styles**

在编辑器工具区增加网络库按钮和面板：刷新、搜索、类型筛选、文件清单、预览 iframe、错误/空状态；沿用现有 CSS 变量，窄屏时面板纵向排列，预览 iframe 使用 `sandbox="allow-scripts allow-forms allow-modals"`。

- [ ] **Step 2: Add network state and rendering helpers**

增加 `networkFiles`, `networkQuery`, `networkType`, `networkPreviewName` 状态；实现 `formatNetworkFileMeta`, `networkFileMatches`, `renderNetworkLibrary`, `setNetworkStatus`。清单刷新失败不清空已有 `networkFiles`。

- [ ] **Step 3: Implement list and HTML preview/open**

`loadNetworkLibrary()` 请求 `/api/html-paste/list`，校验响应后渲染；HTML 文件使用 `encodeURIComponent(file.name)` 生成 `/html_paste/<name>`，预览写入 iframe `src`，新窗口使用 `window.open`，禁止把原始文件名直接拼接到 URL。

- [ ] **Step 4: Implement network JSON import**

`importNetworkJson(name)` 请求 `/api/html-paste/read?name=...`，解析后复用现有 `normalizeImportedDocument` 和 `validateImportedDocument(normalized, parsed)`；确认替换前清空选择/复制篮/可见性状态，失败时保留全部当前状态。

- [ ] **Step 5: Implement network JSON export with overwrite confirmation**

`saveNetworkJson()` 以当前文档生成 JSON，先检查覆盖复选框；未勾选时直接调用 save API，409 时提示同名并要求勾选覆盖；勾选时弹出明确二次确认，再发送 `overwrite:true`。成功后刷新清单并提示文件名。

- [ ] **Step 6: Bind events without affecting generatedShell**

仅在编辑器页面绑定网络库控件；`generatedShell` 保持本地 JSON 控件和独立运行，不引用 `/api/html-paste/*`。运行两项 Node 测试并预期 PASS。

- [ ] **Step 7: Commit the UI**

```bash
git add html/HtmlPasteGen.html tests/html_paste_gen_test.js tests/html_paste_gen_ui_test.js
git commit -m "feat: add html paste network library UI"
```

### Task 5: 集成验证与清理

**Files:**
- Modify temporarily only: `html/html_paste/network-test.json`, `html/html_paste/network-test.html`

- [ ] **Step 1: Run static checks and Linux build**

```bash
node --check tests/html_paste_gen_test.js
node --check tests/html_paste_gen_ui_test.js
node tests/http_handler_html_paste_test.js
node tests/html_paste_gen_test.js
node tests/html_paste_gen_ui_test.js
wsl make
git diff --check
```

- [ ] **Step 2: Verify API and browser behavior**

启动本地服务器，使用临时 JSON/HTML fixture 验证 list、read、save、409 冲突、overwrite、导入替换、HTML iframe 预览和新窗口 URL；检查非法文件名返回 4xx，并确认刷新失败时已有清单仍在。

- [ ] **Step 3: Delete fixtures and rerun checks**

用 `apply_patch` 删除两个临时 fixture，确认 `git status` 只剩既有用户未提交文件，再运行 `git diff --check` 和核心测试。

- [ ] **Step 4: Commit verification fixes if needed**

若集成验证发现问题，先补失败测试再修复；所有测试通过后保留清晰提交历史并输出最终 `html/HtmlPasteGen.html` 与服务器改动位置。
