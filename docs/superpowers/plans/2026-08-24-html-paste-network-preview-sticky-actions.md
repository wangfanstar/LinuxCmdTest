# HtmlPasteGen 网络库预览与固定操作 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended; inline execution is also valid). Steps use checkbox syntax for tracking.

**Goal:** 修复网络库 HTML 打开空白、默认启用覆盖保存，让网络库和生成 HTML 操作在长页面滚动时保持可见，并让生成页面默认展开所有命令。

**Architecture:** 修改 html/HtmlPasteGen.html 的编辑器与 generatedShell。网络库 header 提升为 app-shell 直属 sticky toolbar；生成页将 hero 说明与操作搜索区拆开，操作区成为直属 sticky toolbar；生成运行时初始化全部条目为展开状态。Node 合约测试覆盖结构与关键行为，WSL make 覆盖 Linux 编译。

**Tech Stack:** 原生 HTML/CSS/JavaScript、Node.js assert/vm、C11 simplewebserver、WSL make。

---

### Task 1: 先写回归测试并确认 RED

**Files:** tests/html_paste_gen_ui_test.js、tests/html_paste_gen_test.js

- [ ] 在 UI 测试中加入 network-library-toolbar ID，并加入：

    assert.match(html, /id=["']network-library-toolbar["']/);
    assert.match(html, /id=["']network-overwrite-checkbox["'][^>]*checked/);
    assert.match(html, /window\.open\(\s*networkFileUrl\(/);
    assert.doesNotMatch(html, /window\.open\(\s*['"]about:blank['"]/);
    assert.match(html, /network-preview[\s\S]*addEventListener\(['"]error['"]/);
    assert.match(html, /class=["']generated-toolbar["']/);

- [ ] 在生成 HTML 测试中加入：

    assert.match(generated, /const expandedIds = new Set\(model\.groups\.flatMap/);
    assert.match(generated, /generated-toolbar[\s\S]*position:\s*sticky/);

- [ ] 替换旧的 about:blank 结构断言，运行：

    node tests/html_paste_gen_ui_test.js
    node tests/html_paste_gen_test.js

预期因生产代码尚未更新而失败；随后提交测试：

    git add tests/html_paste_gen_ui_test.js tests/html_paste_gen_test.js
    git commit -m "test: specify sticky network and generated toolbars"

### Task 2: 网络库预览、打开、覆盖和 sticky 工具栏

**File:** html/HtmlPasteGen.html

- [ ] 将网络库标题/按钮 header 从 network-library-panel 外移为直属 section network-library-toolbar，保留原有按钮 ID；network-overwrite-checkbox 增加 checked。
- [ ] 添加：

    .network-library-toolbar {
      position: sticky;
      z-index: 38;
      top: 0;
      margin-top: 14px;
      border: 1px solid var(--line);
      border-radius: var(--radius-lg);
      box-shadow: 0 10px 28px rgba(31, 51, 80, .12);
      background: color-mix(in srgb, var(--surface) 96%, transparent);
      backdrop-filter: blur(14px);
    }

  在 620px 以下保持按钮换行并减少上下边距。
- [ ] 将 openNetworkHtml 替换为：

    function openNetworkHtml(name) {
      const file = state.networkFiles.find(entry => entry.name === name);
      if (!file || file.type !== 'html') return;
      const popup = window.open(networkFileUrl(file.name), '_blank', 'noopener,noreferrer');
      if (!popup) announce('浏览器阻止了新窗口，请允许弹窗后重试。', 'warning');
    }

- [ ] 将 previewNetworkHtml 改为先 fetch networkFileUrl(file.name)，响应非 2xx 时写入错误状态；成功后再赋值 iframe src。给 network-preview 绑定 load/error 事件，分别写入成功/失败状态。
- [ ] 运行：

    node tests/html_paste_gen_ui_test.js
    node tests/html_paste_gen_test.js
    node tests/http_handler_html_paste_test.js

### Task 3: 生成 HTML 默认完整命令和固定顶部操作区

**File:** html/HtmlPasteGen.html

- [ ] 在 generatedShell 中关闭 hero 后新增直属 generated-toolbar，将原有 hero-actions、search-row、copy-status 移入；补充：

    .generated-toolbar {
      position: sticky;
      z-index: 40;
      top: 0;
      margin-top: 12px;
      padding: 12px 0;
      border-bottom: 1px solid var(--line);
      background: color-mix(in srgb, var(--bg) 94%, transparent);
      backdrop-filter: blur(14px);
    }

- [ ] 将生成运行时集合初始化为：

    const expandedIds = new Set(model.groups.flatMap(group => group.items.map(item => item.id)));
    let revealAll = false;

  JSON 导入和恢复原始内容后清空并按新 model 重建 expandedIds；保留单条 toggleItemExpanded 和全局内容显隐。
- [ ] 运行：

    node tests/html_paste_gen_test.js
    node tests/html_paste_gen_ui_test.js

### Task 4: 浏览器回归、编译和交付

**Files:** 仅验证 html/HtmlPasteGen.html、tests/html_paste_gen_ui_test.js、tests/html_paste_gen_test.js、tests/http_handler_html_paste_test.js。

- [ ] wsl make 退出码为 0。
- [ ] 浏览器确认网络库 sticky toolbar 在下翻编辑区时仍可见；覆盖默认勾选且同名导出保留确认；HTML iframe 可见，打开按钮直接进入目标 URL。
- [ ] 浏览器生成长命令 HTML，确认初始完整显示，生成页顶部 toolbar 下翻仍可见，单条收起/全局显隐仍有效。
- [ ] 最终运行：

    git diff --check
    node tests/html_paste_gen_ui_test.js
    node tests/html_paste_gen_test.js
    node tests/http_handler_html_paste_test.js
    wsl make

  只提交 html/HtmlPasteGen.html 与测试文件；保留 .claude/settings.local.json、删除的 sqlite 文件和 nul 等既有无关变更。
