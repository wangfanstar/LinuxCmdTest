# NoteWiki 代码架构说明（AGENTS.md）

> 本文件供 AI 助手快速理解 wiki 子系统的架构、数据约定与维护规则，避免重复探索或误改关键逻辑。  
> 源码根目录：`E:/MCP_PROJECT/wfserver/wfwebserver`

---

## 1. 系统概览

NoteWiki 是一个**单页 Markdown wiki**，前后端分离：

- **后端**：C 语言 HTTP 服务器（`src/`），直接操作文件系统和 SQLite，无第三方 web 框架。
- **前端**：纯 HTML + 原生 JavaScript，无打包工具，无 npm 依赖。
- **核心理念**：在浏览器内完成写作、发布、历史查看、权限管理，所有数据本地持久化。

---

## 2. 目录结构

```
html/wiki/
├── notewiki.html        # 主编辑器 SPA（3100+ 行，单文件）
├── notewikiindex.html   # 文章全文搜索索引页（离线可用）
├── sidebar.js           # 已发布 HTML 页面注入：侧边栏 + PDF 导出
├── rich-render.js       # MathJax / Mermaid 懒加载渲染器（全局复用）
├── wiki-auth-admin.html # 权限管理后台（admin 角色专用）
├── md_db/               # Markdown 源文件（按分类子目录组织）
├── sqlite_db/           # SQLite 数据库（auth/audit/history）
│   └── db.config        # 数据库配置（路径等）
├── uploads/             # 上传文件（图片 + 附件，扁平存储）
├── vendor/              # 第三方库
│   ├── mathjax/         # tex-svg-full.js（数学公式）
│   └── mermaid/         # mermaid.min.js（流程图）
├── wiki-index.json      # 文章元数据索引（由服务端生成，供离线搜索）
└── AGENTS.md            # 本文件

src/
├── wiki.c / wiki.h      # 全部 wiki REST API 实现（2000+ 行）
├── auth_db.c / auth_db.h# SQLite 权限体系：登录、审计、历史快照
├── http_handler.c/h     # HTTP 路由分发 + auth 鉴权前置
├── main.c               # TCP 监听 + 线程池初始化
├── threadpool.c/h       # 生产者-消费者线程池
└── log.c/h              # 滚动日志（线程安全）
```

---

## 3. 存储架构

### 3.1 Markdown 源文件（`md_db/`）

- 路径规则：`md_db/[category/]<id>.md`（无分类则在根层）
- **首行必须是 META 注释**（`wiki_scan_md_dir()` 扫描的唯一元数据来源）：

  ```
  <!--META {"id":"note_20240101_120000_abcd","title":"文章标题","category":"分类路径","created":"2024-01-01T12:00:00Z","updated":"2024-01-01T12:00:00Z"} -->
  ```

  字段顺序无关，但 `id` 必须存在且非空。破坏首行格式会导致文章从列表中消失。

- 第二行起为正文 Markdown 内容。

### 3.2 已发布 HTML（`html/wiki/[category/]<id>.html`）

- 由 `wiki_write_html_file()` 从 `.md` 内容生成，是**派生产物，不要手动编辑**。
- 通过 `<script src="/wiki/sidebar.js">` 注入侧边栏、TOC、PDF 导出能力。
- 重建全部 HTML：`POST /api/wiki-rebuild-html` 或前端「全部更新 HTML」按钮。

### 3.3 SQLite（`sqlite_db/`）

- 仅由 `auth_db.c` 管理，`wiki.c` 不直接操作数据库。
- 主要表：`users`（用户）、`audit_log`（操作审计）、`md_history`（文章历史快照）。

### 3.4 上传文件（`uploads/`）

- 扁平存储，文件名经双重处理：
  1. `wiki_upload_safe()`：字符白名单（仅 `[a-zA-Z0-9._-]`）+ 扩展名白名单（图片/文档/代码/压缩/音视频）。
  2. `wiki_upload_unique_name()`：若同名文件已存在，追加 `_1`、`_2` 后缀。
- 客户端在上传前还会调用 `buildSafeUploadFilename()`（`notewiki.html`）对文件名做规范化。

### 3.5 索引文件（`wiki-index.json`）

- 格式与 `/api/wiki-list` 返回值一致，供 `notewikiindex.html` 离线全文搜索使用。
- 由 `GET /api/wiki-refresh-index` 触发重建，或前端「📄 索引」按钮。

---

## 4. 服务端 API（`src/wiki.c`）

### 4.1 文章 CRUD

| 方法 | 路径 | 实现函数 | 说明 |
|------|------|---------|------|
| GET  | `/api/wiki-list` | `handle_api_wiki_list` | 扫描 md_db，返回所有文章元数据数组 |
| GET  | `/api/wiki-read` | `handle_api_wiki_read` | 返回指定文章的 MD 原文（`?id=`） |
| POST | `/api/wiki-save` | `handle_api_wiki_save` | 保存/发布：写 .md + 生成 .html + 写历史快照 |
| POST | `/api/wiki-delete` | `handle_api_wiki_delete` | 删除文章（md + html） |
| GET  | `/api/wiki-search` | `handle_api_wiki_search` | 全文搜索（遍历 md_db 文件内容） |

### 4.2 分类管理

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/wiki-mkdir` | 新建分类目录 |
| POST | `/api/wiki-rename-cat` | 重命名分类（含移动子目录和 HTML） |
| POST | `/api/wiki-delete-cat` | 递归删除分类及其所有文章 |
| POST | `/api/wiki-rename-article` | 重命名文章（id/title 同步） |
| POST | `/api/wiki-move-article` | 移动文章到其他分类 |

### 4.3 导入导出

| 方法 | 路径 | 说明 |
|------|------|------|
| GET  | `/api/wiki-export-md-zip` | 导出单篇 MD + 附件为 ZIP |
| POST | `/api/wiki-export-pdf` | 服务端生成 PDF 用 HTML 片段（返回完整 HTML 字符串） |
| POST | `/api/wiki-upload` | 上传文件，返回 `{"ok":true,"url":"/wiki/uploads/..."}` |
| POST | `/api/wiki-cleanup-uploads` | 删除所有文章中未引用的上传文件 |

### 4.4 维护

| 方法 | 路径 | 说明 |
|------|------|------|
| GET  | `/api/wiki-refresh-index` | 重建 wiki-index.json |
| POST | `/api/wiki-rebuild-html` | 批量重建全部已发布 HTML |

### 4.5 auth_db.c API（经 `http_handler.c` 路由）

| 路径 | 说明 |
|------|------|
| `/api/wiki-login` / `/api/wiki-logout` | 登录/登出（Cookie `wiki_token`） |
| `/api/wiki-auth-status` | 返回当前登录用户信息 |
| `/api/wiki-users` | 列出所有用户（admin） |
| `/api/wiki-user-save` / `/api/wiki-user-delete` | 用户管理（admin） |
| `/api/wiki-audit-logs` | 操作审计日志（admin） |
| `/api/wiki-md-history` | 文章历史版本列表与内容（`?id=&limit=&offset=`） |
| `/api/wiki-user-article-rank` | 用户文章贡献排名 |

---

## 5. 权限体系（`src/auth_db.c`）

- 角色：`admin`（用户管理 + 全局审计）/ `author`（写文章）/ 未登录（只读）。
- **鉴权前置**：`http_handler.c` 中 `is_wiki_write_api()` 列出所有写操作路径，调用前先验证 Cookie。
- 认证流程：`Cookie: wiki_token=<token>` → `auth_resolve_user_from_headers()` → `auth_user_t`。
- 所有写操作写 `audit_log`；`wiki-save` 同时调用 `auth_md_backup()` 存完整历史快照。
- **新增写操作路由时**，必须同步更新 `is_wiki_write_api()` 函数，否则无需登录即可调用。

---

## 6. 前端架构（`notewiki.html`）

单文件 SPA，所有逻辑内联，无外部 JS 依赖（除 `rich-render.js`）。

### 6.1 核心模块

| 模块/函数 | 说明 |
|-----------|------|
| `S`（全局对象） | 应用状态：`curId`、`articles`、`curCat`、`editIsNew`、编辑草稿等 |
| `init()` | 初始化：检查 auth → 加载文章列表 → 渲染目录树 |
| `refreshAll()` | 重新从服务端拉取文章列表并重建目录树 |
| `openArticle(id)` | 切换到阅读态：fetch read API → 渲染 → 更新 TOC |
| `_doSave(opts)` | 保存/发布核心：验证 → 渲染 MD → fetch save API → 刷新 |
| `md2html(src, syncLines)` | 内置 Markdown 渲染器（GFM 表格/脚注/数学/mermaid 占位符） |
| `scheduleAutoSave()` | 30s 自动保存（仅 dirty 时触发） |
| `histSnapshot()` | Undo/Redo 环形缓冲（50步） |
| `doUploadFile(file, opts)` | 上传：`forceLink=true` → 链接，否则 → 图片嵌入 |
| `exportPdf()` | PDF 导出入口：判断是否走服务端路径 |
| `exportPdfBuildWindow()` | 客户端 iframe 打印方式 |
| `exportPdfViaServer()` | 服务端生成 HTML 后打印 |

### 6.2 Markdown 渲染流程

```
编辑器输入 → onEditorInput() → scheduleEditorRender()
    → md2html()（同步，约 5ms）→ updatePreview()
    → scheduleRichRender()（异步）→ rich-render.js
        → MathJax 渲染数学公式
        → Mermaid 渲染流程图
```

### 6.3 目录树

- 使用分类路径字符串（如 `"技术/前端"`）作为键，`S.articles` 为扁平数组。
- `renderTree()` → `catSection()` / `catSectionTree()` 按分类递归渲染。
- 折叠状态存 `localStorage`（键 `wiki-cats-collapsed`）。

### 6.4 键盘快捷键（`onEditorKey()`）

| 快捷键 | 操作 |
|--------|------|
| Ctrl+S | 保存 |
| Ctrl+P | 发布 |
| Ctrl+Z / Ctrl+Y | Undo / Redo |
| Ctrl+Alt+T | 插入表格 |
| Ctrl+Alt+M | 插入数学公式 |
| Ctrl+Alt+I | 插入图片 |
| Ctrl+Alt+A | 上传附件 |

---

## 7. sidebar.js（已发布页面注入）

已发布 HTML（`/wiki/[cat/]<id>.html`）末尾引入 `<script src="/wiki/sidebar.js">`，注入：

- **左侧文章目录**：从 `/api/wiki-list` 动态加载，支持分类折叠。
- **右侧本文 TOC**：扫描 `#article-body` 内的 h1–h6，支持折叠。
- **PDF 导出**：覆盖 `window.exportPdf`，逻辑与 `notewiki.html` 的 `exportPdfBuildWindow` 保持一致。
- **`buildStablePdfBookmarkBody()`**：为 PDF 书签在 body 头部注入不可见的 shadow 标题元素。

**同步规则**：`sidebar.js` 的 `exportPdf` CSS、`buildPdfToc()` 和 boot 脚本必须与 `notewiki.html` 的 `exportPdfBuildWindow` 保持同步。**改其中一处必须同步修改另一处**。

---

## 8. PDF 导出架构

两条路径，入口均为 `exportPdf()`：

```
exportPdf()
    ├── exportPdfViaServer()   # 服务端路径（/api/wiki-export-pdf）
    │     服务端生成完整 HTML → 客户端 iframe 打印
    └── exportPdfBuildWindow() # 客户端路径
          在前端构建完整 HTML → iframe srcdoc → window.print()
```

**页码**：使用 CSS `@page { @bottom-center { content: counter(page) " / " counter(pages) } }`，不再使用 JS 注入（Chrome 90+ 原生支持）。

**PDF 目录（TOC）**：
- `buildPdfToc()` 在 boot 脚本中运行，扫描 `#pdf-article-body` 的 h1–h4。
- 最深层级（`maxLv`）的条目加 `deepest` 类，CSS 设置为 `#b54708`（琥珀色）。
- 其他层级：`#555`（灰色），每级缩进 10px。
- 打印时 `.pdf-layout` 切换为 `display:block`，目录置于正文上方。

---

## 9. rich-render.js

全局复用的懒加载渲染器，`notewiki.html`、`sidebar.js`、已发布 HTML 三处均使用。

```javascript
var rr = createRichRenderer({
    mathjaxSrc: '/wiki/vendor/mathjax/tex-svg-full.js',
    mermaidSrc: '/wiki/vendor/mermaid/mermaid.min.js',
    mermaidTheme: 'dark'  // or 'default'
});
rr.ensure(callback);  // 懒加载库（同一页面只加载一次）
rr.render(rootElement); // 渲染 root 内的数学和流程图
```

修改此文件需兼顾**编辑预览**、**已发布页面**、**PDF 导出**三个场景。

---

## 10. 文章 ID 规范

- 字符集：`[a-zA-Z0-9_-]`（服务端严格校验）。
- 自动生成格式：`note_YYYYMMDD_HHMMSS_XXXX`（`wiki_gen_id()`，后四位为线程ID低16位）。
- 手动指定时须满足字符集要求，否则 `wiki-save` 返回 400。

---

## 11. 关键内部函数（`wiki.c`）

| 函数 | 作用 |
|------|------|
| `wiki_gen_id()` | 生成唯一文章 ID |
| `wiki_md_find()` | 在 md_db 中递归查找 `<id>.md` |
| `wiki_md_write_path()` | 计算 .md 文件写入路径（含分类子目录） |
| `wiki_write_html_file()` | 从标题/分类/HTML body 生成完整已发布 HTML |
| `wiki_rewrite_html()` | 更新已发布 HTML 的元数据（重命名/移动后调用） |
| `wiki_scan_md_dir()` | 遍历 md_db 构建文章列表 JSON |
| `wiki_rename_replace()` | 原子写文件（tmp → rename） |
| `wiki_upload_safe()` | 文件名安全校验（字符白名单 + 扩展名白名单） |
| `wiki_upload_unique_name()` | 上传冲突处理（追加 `_1`/`_2`） |
| `wiki_write_related_attachments()` | 扫描 HTML body 中非图片 uploads 链接，追加「相关附件」section |
| `wiki_collect_dirs()` | 遍历 wiki 根目录收集分类路径列表 |
| `register_subdir_safe()` | 分类路径安全校验（防 `..` 穿越） |

---

## 12. 维护注意事项

1. **MD 首行 META 不可破坏**：`<!--META {...} -->` 是文章列表的唯一来源，改变格式会导致文章"消失"。

2. **HTML 是派生产物**：已发布的 `.html` 文件由服务端生成，不要手动编辑。修改文章样式应改 `wiki_write_html_file()`。

3. **sidebar.js 与 notewiki.html PDF 逻辑必须同步**：两者各自有独立的 `exportPdf` 实现，改一处必须改另一处（CSS、TOC、boot 脚本）。

4. **新增写操作路由必须更新 `is_wiki_write_api()`**（`http_handler.c` 第 31 行起），否则未登录用户可直接调用。

5. **sqlite_db 只由 `auth_db.c` 管理**：`wiki.c` 不应直接操作数据库文件，历史/审计相关功能通过 `auth_md_backup()` / `auth_audit()` 接口调用。

6. **并发安全**：文件写入通过 `wiki_rename_replace()` 保证原子性，不要绕过此函数直接用 `fopen(..., "w")`。

7. **字符串缓冲区上限**：id 128B、title 512B、cat 512B。扩展字段长度时需同步修改 C 端声明和 JS 端校验逻辑。

8. **扩展名白名单**：上传允许的扩展名在 `wiki_upload_safe()` 的 `exts[]` 数组中维护，新增文件类型在此添加。

9. **分类路径分隔符**：统一用 `/`（正斜杠），前后端约定一致，`register_subdir_safe()` 会拒绝包含 `\`、`..` 的路径。

10. **rich-render.js 不要内联**：三处场景都依赖 `/wiki/rich-render.js` 的 URL，不要把它的内容内联进 notewiki.html 或 sidebar.js。

---

## 13. 典型数据流示意

### 保存/发布文章

```
前端 _doSave()
  → md2html() 生成 HTML
  → POST /api/wiki-save {id, title, category, content, html, published}
      → wiki_save (wiki.c)
          → 校验 id/cat
          → 写 md_db/[cat/]<id>.md（首行 META + 正文）
          → wiki_write_html_file() → html/wiki/[cat/]<id>.html
          → auth_md_backup() → sqlite md_history 快照
          → auth_audit() → sqlite audit_log
      → 返回 {ok, id, updated}
  → refreshAll() 刷新文章列表
```

### 上传附件

```
前端 doUploadFile(file, {forceLink: true})
  → buildSafeUploadFilename() 规范化文件名
  → 插入占位符 [附件上传中...](uploading)
  → POST /api/wiki-upload（body=文件内容，X-Wiki-Filename 头）
      → wiki_upload (wiki.c)
          → wiki_upload_safe() 校验文件名
          → wiki_upload_unique_name() 处理冲突
          → 写入 html/wiki/uploads/<name>
      → 返回 {ok, url}
  → 替换占位符为 [filename](url)
```

### PDF 导出（客户端路径）

```
前端 exportPdfBuildWindow(title, cat, updated, bodyHtml)
  → 构建完整 HTML（含 @page CSS + TOC 占位 + body）
  → 创建隐藏 iframe，设置 srcdoc
  → iframe 内 boot 脚本：
      → rich-render.js 渲染数学/流程图（如有）
      → buildPdfToc() 扫描标题，填充 #pdf-toc-list
      → window.__pdfReady = 1
  → 主窗口检测 __pdfReady → cw.print()
  → afterprint 事件 → 清理 iframe
```
