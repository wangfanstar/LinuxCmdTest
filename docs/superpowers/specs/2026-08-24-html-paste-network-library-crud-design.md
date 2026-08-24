# HtmlPasteGen 网络库 JSON CRUD 设计

## 目标

在现有 HtmlPasteGen.html 网络库面板上增加 JSON 文件的完整增删改查能力，让用户无需离开页面即可创建、查看、编辑、删除和导入网络库 JSON；HTML 文件继续保持只读列表、预览和打开能力。

## 范围与边界

- JSON 文件：支持新建、列表查询、读取、编辑覆盖、删除、导入当前编辑器。
- HTML 文件：仅支持列表、搜索、类型筛选、沙箱预览和新窗口打开，不开放网络写入。
- 本地 JSON 导入导出能力保持不变。
- 生成的独立 HTML 页面不依赖网络库 API，仍可离线运行。

## 用户交互

网络库面板沿用现有刷新、搜索、类型筛选和预览区，新增：

1. “新建 JSON”按钮：创建一条空白编辑行，默认文件名为 untitled.json，内容填入最小合法 HtmlPasteGen 文档。
2. JSON 行内编辑：展开后显示文件名输入框和 JSON 文本编辑框；提供“校验并保存”“取消编辑”。
3. 已有 JSON 行操作：提供“编辑”“导入”“删除”。编辑保存覆盖原文件，并在覆盖前二次确认；导入继续复用现有导入确认和 schema 校验。
4. 删除操作：显示文件名和不可恢复提示，确认后删除；成功后刷新清单并清除预览，失败时保留原清单和编辑状态。
5. 状态反馈：保存、删除、读取、解析、权限和网络错误通过网络库状态区提示，已有清单不会因刷新失败被清空。

## 数据与状态

前端网络库状态扩展为：

- networkFiles：服务器返回的文件元数据；
- networkQuery / networkType：搜索和类型筛选；
- networkPreviewName：当前 HTML 预览文件；
- networkEditingName：当前编辑中的 JSON 文件名，空值表示新建；
- networkDraft：编辑中的文件名和原始 JSON 文本；
- networkBusy：防止重复保存/删除请求。

保存前分两层校验：

1. JSON 语法校验，拒绝空内容和非法 JSON；
2. 对 HtmlPasteGen 文档复用 normalizeImportedDocument 与 validateImportedDocument，提示 schema、ID、快捷键等错误。

服务器仅保存经过路径和大小限制的 .json 文件；读取或保存失败不替换当前草稿。

## 服务器 API

保留现有接口：

- GET /api/html-paste/list：列出 .json 和 .html；
- GET /api/html-paste/read?name=...：读取单个 JSON；
- POST /api/html-paste/save：使用 {name, content, overwrite} 原子保存 JSON。

新增接口：

- DELETE /api/html-paste/delete?name=...：删除单层 .json 文件。

删除接口要求：

- 复用 html_paste_name_safe(name, 1)，拒绝空名、路径分隔符、..、隐藏文件、非 .json 和非普通文件；
- 使用现有本机回环/author-admin 请求保护；
- 在 g_html_paste_mu 互斥锁内执行 unlink，返回 {ok:true,name}；
- 参数无效返回 400，文件不存在返回 404，权限或文件系统错误返回 403/500；
- 不提供 HTML 删除和任意目录操作。

## 安全与兼容

- 文件名始终通过 encodeURIComponent 放入查询字符串，不直接拼接用户输入路径。
- 删除只允许 JSON，避免通过网络库接口修改可执行 HTML。
- 预览继续使用受限 sandbox iframe。
- 旧 JSON 缺少新字段时沿用既有规范化逻辑，不影响现有文件。
- 非覆盖保存继续默认拒绝同名文件；编辑模式显式发送 overwrite:true 并要求二次确认。

## 测试策略

- C 契约测试：新增 DELETE 路由、参数校验、授权保护、互斥锁和 unlink 调用断言；运行 make。
- 前端 UI 契约测试：断言 CRUD 控件、编辑状态、删除确认、DELETE API、保存校验与失败保留清单逻辑存在。
- 核心行为测试：覆盖空文档新建、JSON 语法错误、schema 错误、同名拒绝、编辑覆盖和删除后刷新。
- 回归测试：node tests/http_handler_html_paste_test.js、node tests/html_paste_gen_test.js、node tests/html_paste_gen_ui_test.js，以及 wsl make。

## 非目标

- 不开放 HTML 新建、编辑或删除。
- 不实现目录层级、批量删除、版本历史或回收站。
- 不改变生成页面的离线运行模型。
