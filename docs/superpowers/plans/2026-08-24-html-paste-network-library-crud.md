# HtmlPasteGen 网络库 JSON CRUD Implementation Plan

> For agentic workers: REQUIRED SUB-SKILL: Use superpowers:executing-plans (recommended) to implement this plan task-by-task. Steps use checkbox syntax for tracking.

Goal: 为现有 HtmlPasteGen 网络库增加 JSON 文件的行内新建、读取、编辑覆盖和删除能力，HTML 文件继续只读预览，并通过 GitHub 远程保存提交。

Architecture: 后端在 src/http_handler.c 增加受保护的 DELETE /api/html-paste/delete 路由，复用现有单层 JSON 文件名校验、大小限制、互斥锁和回环/author-admin 授权。前端在 html/HtmlPasteGen.html 扩展网络库状态和行内编辑器，JSON 保存前执行语法与 HtmlPasteGen 文档校验，失败保持草稿与已有清单；HTML 仍只读预览。测试采用 Node 源码契约、核心行为断言和 Linux make 回归。

Tech Stack: C11/POSIX HTTP handler, vanilla HTML/CSS/JavaScript, Node.js assert tests, WSL make, git.

---

### Task 1: 后端 DELETE API 契约与实现

Files:
- Modify: tests/http_handler_html_paste_test.js
- Modify: src/http_handler.c near html_paste network handlers and request routing

- [ ] Step 1: 写 DELETE API 的失败契约测试

在 tests/http_handler_html_paste_test.js 现有网络库断言后加入：

    assert.match(source, /handle_api_html_paste_delete\s*\(/);
    assert.match(source, /\/api\/html-paste\/delete/);
    assert.match(source, /html_paste_file_path\s*\([^\n]+1\)/);
    assert.match(source, /unlink\s*\(/);
    assert.match(source, /DELETE/);
    assert.match(source, /JSON file not found/);

Run:

    node tests/http_handler_html_paste_test.js

Expected: FAIL because the DELETE handler, route, and unlink call do not exist.

- [ ] Step 2: 实现删除处理函数

在 handle_api_html_paste_read 或 handle_api_html_paste_save 附近增加：

    static void handle_api_html_paste_delete(http_sock_t client_fd, const char *path_qs)

函数按以下顺序执行：

1. 用 query_param_get(path_qs, "name", name, sizeof(name)) 读取文件名，并用 html_paste_file_path(filepath, sizeof(filepath), name, 1) 强制只允许 JSON。
2. stat(filepath) 检查普通文件；参数无效返回 400，文件不存在或非普通文件返回 404。
3. 获取 HTML_PASTE_LOCK()，再次 stat 防止竞态；调用 unlink(filepath)，释放锁。
4. unlink 成功返回 200 和 {"ok":true,"name":"..."}，失败按 errno 返回 403 或 500 的 JSON 错误。
5. 响应中的 name 使用现有 sb_json_str 或等价 JSON 转义，不能把原始名称直接拼成 JSON。

- [ ] Step 3: 接入 DELETE 路由与授权

在 handle_client_request 的 POST 分支之后、GET 方法检查之前增加 DELETE 分支。仅对 /api/html-paste/delete 处理：

    if (strcasecmp(method, "DELETE") == 0) {
        if (strcmp(path, "/api/html-paste/delete") != 0) {
            send_response(client_fd, 404, "Not Found", "<h1>404 Not Found</h1>");
            goto done;
        }
        if (html_paste_request_allowed(client_ip, req_buf, client_fd) != 0) goto done;
        handle_api_html_paste_delete(client_fd, path_qs);
        goto done;
    }

保持现有 GET/POST 路由、回环免登录和 author/admin 保护不变。

- [ ] Step 4: 运行后端契约测试与 Linux 编译

Run:

    node tests/http_handler_html_paste_test.js
    wsl make

Expected: both commands exit 0; test prints html paste network API contracts passed.

- [ ] Step 5: 提交后端变更

    git add src/http_handler.c tests/http_handler_html_paste_test.js
    git commit -m "feat: add html paste network JSON delete API"

### Task 2: 前端 CRUD 失败契约测试

Files:
- Modify: tests/html_paste_gen_ui_test.js
- Modify: tests/html_paste_gen_test.js

- [ ] Step 1: 增加 UI 控件与函数断言

在 tests/html_paste_gen_ui_test.js 现有网络库 ID 列表增加：

    network-new-json-button
    network-network-editor
    network-draft-name
    network-draft-content
    network-draft-save
    network-draft-cancel

并加入以下源码契约：

    assert.match(html, /function\s+startNetworkJsonCreate\s*\(/);
    assert.match(html, /function\s+editNetworkJson\s*\(/);
    assert.match(html, /function\s+saveNetworkDraft\s*\(/);
    assert.match(html, /function\s+deleteNetworkJson\s*\(/);
    assert.match(html, /\/api\/html-paste\/delete\?name=/);
    assert.match(html, /JSON\.parse/);
    assert.match(html, /校验并保存/);
    assert.match(html, /删除网络库文件/);
    assert.match(html, /当前编辑草稿未改变/);

- [ ] Step 2: 增加核心 JSON 草稿校验的失败断言

在 tests/html_paste_gen_test.js 的 HtmlPasteGen core 测试末尾增加：

    const invalidDraft = core.cloneDocument(searchDoc);
    invalidDraft.groups[0].items[0].shortcut = 'Ctrl+';
    assert.strictEqual(core.validateDocument(invalidDraft).valid, false);
    assert.strictEqual(JSON.parse(JSON.stringify(searchDoc)).meta.title, searchDoc.meta.title);

同时断言生成页仍不包含 network-library-panel，确保网络库 CRUD 只存在于编辑器壳，不泄漏到独立生成 HTML。

- [ ] Step 3: 运行测试确认 RED

Run:

    node tests/html_paste_gen_test.js
    node tests/html_paste_gen_ui_test.js

Expected: UI test FAIL because CRUD IDs/functions are not yet present; core test continues to pass existing assertions.

- [ ] Step 4: 提交 RED 契约

    git add tests/html_paste_gen_test.js tests/html_paste_gen_ui_test.js
    git commit -m "test: specify network library JSON CRUD UI"

### Task 3: 网络库行内编辑器与状态管理

Files:
- Modify: html/HtmlPasteGen.html near network CSS, network panel markup, and app-logic network functions
- Test: tests/html_paste_gen_ui_test.js

- [ ] Step 1: 添加状态和编辑器面板

在 state.networkPreviewName 后增加：

    networkEditingName: '',
    networkDraft: null,
    networkBusy: false

在网络库 header 的“刷新网络库”按钮旁增加 network-new-json-button；在 network-content 前增加隐藏的 network-network-editor，包含：

    input#network-draft-name
    textarea#network-draft-content
    button#network-draft-save，文案“校验并保存”
    button#network-draft-cancel，文案“取消编辑”

沿用现有 field/textarea/button CSS，编辑器在窄屏下与列表纵向排列；保存中禁用编辑器按钮。

- [ ] Step 2: 添加草稿与文件名工具

在 networkFileMatches 附近增加：

    function networkJsonFilename(value) {
      const name = String(value || '').trim();
      if (!name || name[0] === '.' || name.includes('/') || name.includes('\\') || name.includes('..')) return '';
      return /\.json$/i.test(name) ? name : name + '.json';
    }

    function createNetworkDraft(name, content) {
      return { name: name || 'quick-copy.json', content: content || JSON.stringify(createBlankDocument(), null, 2) };
    }

    function clearNetworkDraft() {
      state.networkEditingName = '';
      state.networkDraft = null;
      renderNetworkLibrary();
    }

    function startNetworkJsonCreate() {
      state.networkEditingName = '';
      state.networkDraft = createNetworkDraft('quick-copy.json');
      renderNetworkLibrary();
      requestAnimationFrame(() => byId('network-draft-name').focus());
    }

保持文件名输入只接受单层 .json；网络请求仍通过 encodeURIComponent 编码 name。

- [ ] Step 3: 实现读取已有 JSON 进入编辑状态

实现 async editNetworkJson(name)：

1. 从 state.networkFiles 找到 type=json 的文件。
2. fetch /api/html-paste/read?name=encodeURIComponent(file.name)，以 text 读取并检查 response.ok。
3. JSON.parse 原文；失败仅提示网络库错误，不修改已有草稿。
4. 设置 state.networkEditingName=file.name、state.networkDraft={name:file.name,content:JSON.stringify(parsed,null,2)}，renderNetworkLibrary()。

编辑读取失败时使用 setNetworkStatus 和 announce，保留当前列表。

- [ ] Step 4: 实现行内保存

实现 async saveNetworkDraft()：

1. 从两个编辑控件读取名称和正文，调用 networkJsonFilename；无效名称立即提示。
2. JSON.parse 正文；语法错误提示“JSON 格式错误”，保留草稿。
3. 调用 core.validateDocument(parsed)；错误时显示前 3 条错误，不发请求。
4. 若 state.networkEditingName 非空且名称改变，先确认旧文件不会自动删除；保存新文件后由用户手动删除旧文件。
5. 编辑已有文件时发送 overwrite:true 并二次确认；新建发送 overwrite:false。
6. POST /api/html-paste/save，处理 409、413、403 和其他错误；成功后 await loadNetworkLibrary()，清空草稿并提示保存名称。

保存前不要替换 state.document；任何失败都保留网络草稿、当前编辑内容和清单。

- [ ] Step 5: 接入列表行编辑/删除操作

在 renderNetworkLibrary 的 JSON 分支增加：

    编辑 -> editNetworkJson(file.name)
    导入 -> importNetworkJson(file.name)
    删除 -> deleteNetworkJson(file.name)

当 state.networkDraft 不为空时，先渲染 network-network-editor；编辑器字段从 state.networkDraft 读取，并绑定 input 事件更新草稿，不要每次输入都触发网络请求。

- [ ] Step 6: 运行 UI 测试确认 GREEN

Run:

    node tests/html_paste_gen_ui_test.js

Expected: HtmlPasteGen UI contract tests passed.

### Task 4: 删除交互、导入兼容与回归

Files:
- Modify: html/HtmlPasteGen.html
- Modify: tests/html_paste_gen_test.js
- Modify: tests/html_paste_gen_ui_test.js

- [ ] Step 1: 实现删除确认与 DELETE 请求

实现 async deleteNetworkJson(name)：

1. 仅允许 state.networkFiles 中 type=json 的文件。
2. confirm('确认删除网络库文件“'+name+'”？此操作不可恢复。')；取消立即返回。
3. 设置 state.networkBusy=true，发送 DELETE /api/html-paste/delete?name=encodeURIComponent(name)。
4. response.ok 且 payload.ok 后清除同名 selected/editing 状态，await loadNetworkLibrary()，提示成功。
5. 失败时恢复 networkBusy，保留列表/草稿并提示“删除失败，当前编辑草稿未改变”。

按钮绑定：

    byId('network-new-json-button').addEventListener('click', startNetworkJsonCreate);
    byId('network-draft-save').addEventListener('click', saveNetworkDraft);
    byId('network-draft-cancel').addEventListener('click', clearNetworkDraft);

- [ ] Step 2: 确保导入与 CRUD 状态隔离

导入 JSON 前若 networkDraft 不为空，提示用户取消或继续；确认继续时先 clearNetworkDraft，再沿用现有 importNetworkJson 的 core.validateDocument、normalizeDocument、替换当前编辑内容流程。网络库 JSON 编辑绝不直接写入 state.document，直到用户点击“导入”。

- [ ] Step 3: 增加回归契约

在 UI 测试中断言：

    assert.match(html, /method:\s*['"]DELETE['"]/);
    assert.match(html, /encodeURIComponent\(name\)/);
    assert.match(html, /networkBusy/);
    assert.match(html, /response\.status === 409/);
    assert.match(html, /state\.networkFiles/);

在 core 测试中断言 buildGeneratedHtml(searchDoc) 不包含 /api/html-paste/delete 和 network-draft-content。

- [ ] Step 4: 运行完整测试

Run:

    node --check tests/html_paste_gen_test.js
    node --check tests/html_paste_gen_ui_test.js
    node tests/http_handler_html_paste_test.js
    node tests/html_paste_gen_test.js
    node tests/html_paste_gen_ui_test.js
    wsl make
    git diff --check

Expected: all Node tests print their passed message, wsl make exits 0, and git diff --check prints no errors.

- [ ] Step 5: 提交前检查并提交前端变更

    git add html/HtmlPasteGen.html tests/html_paste_gen_test.js tests/html_paste_gen_ui_test.js
    git commit -m "feat: add network library JSON CRUD UI"

### Task 5: 集成验证、提交 GitHub 与交付

Files:
- No fixture files committed; use temporary files only under html/html_paste during verification.

- [ ] Step 1: 创建临时 JSON/HTML fixture并验证 API

在 html/html_paste 下创建临时 JSON 与 HTML 文件，启动 make run 或现有本地服务，验证：

    GET /api/html-paste/list
    GET /api/html-paste/read?name=<fixture>.json
    POST /api/html-paste/save with overwrite false returns 409 for duplicate
    POST /api/html-paste/save with overwrite true updates content
    DELETE /api/html-paste/delete?name=<fixture>.json removes only JSON

同时确认 HTML 仍可预览，DELETE HTML 返回 400，非法名称返回 400，远程非授权请求仍被拒绝。

- [ ] Step 2: 清理 fixture 并复跑回归

删除所有临时 fixture，确认 git status 只保留本轮提交及既有用户未提交文件；重新运行 Task 4 的 Node 测试、wsl make 和 git diff --check。

- [ ] Step 3: 检查提交内容与远程

    git log --oneline -6
    git show --check --stat HEAD
    git status --short
    git remote -v

确认本轮提交不包含 .claude/settings.local.json、html/wiki/sqlite_db/db.config、html/wiki/sqlite_db/pending_logs.jsonl 或 nul。

- [ ] Step 4: 推送当前分支到 GitHub

    git push -u origin codex/register-viewer-auto-parse

Expected: origin 返回分支创建或更新成功；不执行 git reset --hard、git checkout -- 或删除用户既有未提交文件。

- [ ] Step 5: 输出交付信息

交付 HtmlPasteGen.html、src/http_handler.c、测试文件和设计/计划文档路径，说明 JSON CRUD 范围、HTML 只读边界、测试结果、提交 SHA 和 GitHub 分支。
