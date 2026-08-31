# Register Viewer latest 自动加载设计

## 目标

`html/register-viewer.html` 打开后，默认扫描并加载页面对应的 `./register/latest/` 目录中的全部 XML/JSON 文件；用户仍可继续使用现有的手动文件选择和网络加载入口。

## 方案

复用现有 `GET /api/list-register-files` 接口递归获取 `register/` 下的文件列表，在前端仅保留路径前缀为 `latest/` 的 XML/JSON 文件。对每个文件以相对页面的 `register/latest/...` URL 获取内容，转换为浏览器 `File` 对象后按类型批量加入现有 `pendingFiles` 队列，由既有缓存和解析流程处理。

自动加载在 `initFileInputs()` 完成后启动；请求失败或没有匹配文件时不阻塞页面，使用现有提示机制反馈。手动添加、重复文件判断、缓存、解析和展示逻辑保持不变。

## 验收

- 打开页面后，`register/latest/` 中的所有 `.xml` 和 `.json` 文件自动出现在左侧文件列表并完成解析。
- `register/` 根目录及其它子目录文件不会被默认加载。
- 自动加载失败时页面仍可手动加载文件，并显示明确提示。
- 现有静态检查通过，C 服务器构建不受影响。
