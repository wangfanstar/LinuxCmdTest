# HtmlPasteGen 网络库与 HTML 预览设计

## 目标

让 `HtmlPasteGen.html` 在保留本地 JSON 导入导出的同时，直接管理服务器同目录下的 `html/html_paste/` 网络库：列出并导入/导出 JSON 文件，列出 HTML 文件并预览或打开，减少在文件管理器与浏览器之间切换。

## 交互方案

编辑器顶部保留本地文件操作，并新增“网络库”面板：

- “刷新”重新读取网络库清单；搜索框按文件名过滤；可按 JSON/HTML 类型筛选。
- JSON 文件提供“导入”。导入前显示分组和条目数量并确认；确认后替换当前编辑内容。解析、版本、字段、ID、快捷键校验失败时不改变当前模型。
- 编辑器提供“导出到网络库”。默认同名文件直接拒绝；用户选择“覆盖保存”后仍需二次确认，成功后刷新清单。
- HTML 文件显示文件名、大小和更新时间，提供“预览”和“新窗口打开”。预览和新窗口都使用受限 sandbox iframe；文件名经过 URL 编码，不拼接用户可控路径。
- 网络请求失败时保留当前编辑内容并在状态栏显示可读错误；刷新失败不清空已有清单。

## 服务器 API

仅暴露 `WEB_ROOT/html_paste/` 下的普通文件，所有 API 都拒绝空文件名、隐藏文件、路径分隔符、`..`、非目标扩展名和非普通文件。读写大小限制为 1 MiB，避免网络库接口被用于大文件写入。

- `GET /api/html-paste/list`：返回按修改时间倒序的 `{ ok, files: [{ name, type, size, mtime }] }`，只收录 `.json` 与 `.html`。
- `GET /api/html-paste/read?name=...`：仅允许 `.json`，返回 JSON 文件原文及 `application/json` 类型，页面端继续做 schema 和字段校验。
- `POST /api/html-paste/save`：JSON 请求体 `{ name, content, overwrite }`；仅允许 `.json`，默认同名返回 `409`，`overwrite:true` 才覆盖；写入采用临时文件再原子落盘，非覆盖模式使用原子创建避免并发请求绕过冲突保护。

静态文件服务继续提供 `/html_paste/<name>.html`，并由现有 `..` 路径检查和新增文件名校验共同保证安全。网络库 API 在本机回环请求中免登录；来自其他主机的列表、读取和保存请求需要现有 author/admin 会话。接口不开放删除、任意目录写入或 HTML 写入，降低本地服务暴露在局域网时的风险。

## 数据流与边界

网络 JSON 导入沿用生成页的 `normalizeImportedDocument`/`validateImportedDocument` 逻辑，网络 API 只负责传输原文；导出内容沿用现有 `exportCurrentJson`，移除运行时主题偏好后序列化。网络库操作不会写入浏览器 localStorage，避免服务器文件和单浏览器缓存互相覆盖。

HTML 预览区默认使用 `sandbox="allow-scripts allow-forms allow-modals"`，不授予顶层导航权限；“新窗口打开”先创建 `about:blank` 容器，再在其中使用同样的 sandbox iframe，避免网络库 HTML 直接获得主页面同源权限。列表为空时显示目录为空提示。

## 测试策略

- C 端静态契约测试：确认新增路由、扩展名和路径安全检查、冲突状态码与临时文件写入逻辑存在，并通过 `make` 编译。
- HtmlPasteGen 核心/UI 契约测试：确认网络库控件、API 调用、导入确认、覆盖确认、HTML 预览 URL 编码与刷新保留清单逻辑。
- 浏览器验证：在本地 HTTP 服务中放入临时 JSON/HTML fixture，验证列表、导入替换、同名拒绝、覆盖确认、HTML 预览和新窗口 URL；测试后删除 fixture。

## 非目标

本次不实现网络库删除、重命名、目录层级或独立生成 HTML 页面访问服务器 API；网络库 API 复用现有 author/admin 会话做远程访问控制，独立页面继续只支持本地 JSON 导入导出。
