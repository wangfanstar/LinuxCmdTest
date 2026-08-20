# GT 网页工具

基于 C 语言的轻量级 HTTP 服务器，内置 Wiki 文档管理、报告存档、域段解析等工具。

## 功能概览

| 模块 | 说明 |
|------|------|
| NoteWiki | 分类笔记管理，Markdown 编辑与实时预览，自动生成独立 HTML 文章页 |
| 域段解析 | `TableParse.html`：Length/Range 解析 hex/dec/bin，嵌套域段与 JSON 配置 |
| 寄存器工具 | `register-viewer.html` 可视化解析寄存器描述文件；`register.html` 管理 register 归档目录 |
| CodeChecker 每日网页 | 浏览 CI 每日生成的 CodeChecker 报告（`codechecker_html/` 只读软链接目录），搜索、排序、每页 100 条分页 + iframe 内嵌预览 |
| 网站管理（管理员） | 登录后查看网站日志（分块读取、关键字过滤）与访问 IP 统计（日期范围/路径/IP 过滤、TOP 排行） |
| Wiki 权限与审计（可选） | 基于 SQLite 的登录、角色权限（admin/author/guest）、操作日志、MD 历史备份 |

## 快速开始

### 编译（Linux / WSL / macOS）

```bash
chmod +x build_linux.sh   # 仅首次
./build_linux.sh          # 或直接: make
```

产出：`bin/simplewebserver`

> 如需启用 Wiki 的 SQLite 权限功能，请先安装 sqlite3 开发包后使用：
>
> ```bash
> make SQLITE3=1
> ```

### 编译（Windows，MinGW-w64）

依赖：**GCC（C11）**、**mingw32-make**、**pthread**（WinLibs 自带）。推荐使用 [winget](https://learn.microsoft.com/windows/package-manager/winget/) 安装 WinLibs：

```powershell
winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT --accept-source-agreements --accept-package-agreements
```

安装后**新开一个终端**（或注销重登），使 `PATH` 生效，然后在项目根目录执行：

```powershell
.\build_win.ps1
```

或 **cmd**：

```bat
build_mingw.bat
```

产出：`bin/simplewebserver.exe`。

**说明（Windows 构建）**：`Makefile` 在 `OS=Windows_NT` 时会链接 `-lws2_32`。SVN 接口在 Windows 上返回"本构建不支持"。

> 如需启用 Wiki 的 SQLite 权限功能，请先安装 sqlite3 开发头文件和库后使用：
>
> ```powershell
> mingw32-make SQLITE3=1
> ```

### 一行命令对照

| 环境 | 命令 |
|------|------|
| Linux / WSL | `./build_linux.sh` 或 `make` |
| Windows PowerShell | `.\build_win.ps1` |
| Windows cmd | `build_mingw.bat` |

### 启动 / 停止

```bash
./simplewebserver.sh start          # 默认端口 8881
./simplewebserver.sh start -p 9000  # 自定义端口
./simplewebserver.sh stop
./simplewebserver.sh status
./simplewebserver.sh restart
./simplewebserver.sh build          # 仅编译，不启动
```

启动后浏览器访问 `http://<host>:8881`。

### 依赖

- **GCC**（支持 C11）
- **pthread**（Linux 为系统库；Windows 用 MinGW POSIX 线程）

### SQLite 安装（启用 Wiki 权限/审计时）

#### Windows（MSYS2 / MinGW-w64）

```bash
pacman -Syu
# 关闭并重新打开终端后
pacman -Su
pacman -S mingw-w64-x86_64-sqlite3
```

验证（MSYS2 终端内）：

```bash
sqlite3 --version
ls "$MINGW_PREFIX/include/sqlite3.h"
```

若你在 **PowerShell / cmd** 里使用 WinLibs 的 `gcc`，默认 **没有** `sqlite3.h`。任选其一：

1. 打开 **「MSYS2 MinGW 64-bit」** 终端（不是 MSYS2），在项目目录执行 `mingw32-make SQLITE3=1`（会自动使用 `$MINGW_PREFIX/include`）。
2. 仍在 PowerShell，但已安装 MSYS2 与 `mingw-w64-x86_64-sqlite3` 时，手动指定前缀（路径按你本机安装调整）：
   ```powershell
   mingw32-make SQLITE3=1 SQLITE3_PREFIX=C:/msys64/mingw64
   ```
3. 或使用自定义包含目录与库目录：
   ```powershell
   mingw32-make SQLITE3=1 SQLITE3_CFLAGS=-IC:/msys64/mingw64/include SQLITE3_LDFLAGS=-LC:/msys64/mingw64/lib
   ```

#### Linux

- Ubuntu / Debian

```bash
sudo apt update
sudo apt install -y sqlite3 libsqlite3-dev
```

- CentOS 7

```bash
sudo yum install -y epel-release
sudo yum install -y sqlite sqlite-devel
```

安装后启用编译：

```bash
make SQLITE3=1
```

若 SQLite 自行安装在用户目录（例如头文件为 `~/local/sqlite3/include/sqlite3.h`，库在 `~/local/sqlite3/lib/`），Makefile 会**自动检测**该路径，无需额外参数。也可手动指定前缀：

```bash
make SQLITE3=1 SQLITE3_PREFIX="$HOME/local/sqlite3"
```

或分别指定包含目录与库目录：

```bash
make SQLITE3=1 SQLITE3_INCLUDE="$HOME/local/sqlite3/include" SQLITE3_LIBDIR="$HOME/local/sqlite3/lib"
```

## 项目结构

```
.
├── src/
│   ├── main.c              # 入口：参数解析、socket、accept 循环
│   ├── http_handler.c/h    # HTTP 请求调度（路由分发）、静态文件服务、报告存档接口
│   ├── http_utils.c/h      # 共享工具：strbuf、JSON 解析/构建、HTTP 响应、URL 解码
│   ├── register_api.c/h    # 注册表文件管理（/api/save-register-file 等）
│   ├── wiki.c/h            # Wiki 引擎（Markdown→HTML、搜索、上传、CRUD）
│   ├── auth_db.c/h         # Wiki 登录/权限/会话/审计/MD 历史（SQLite，可选启用）
│   ├── admin_api.c/h       # 管理员 API：日志文件列表/分块读取、IP 访问统计
│   ├── svn_api.c/h         # SVN 日志查询（/api/svn-log）
│   ├── threadpool.c/h      # 线程池（生产者-消费者，循环队列）
│   ├── platform.c/h        # 平台兼容层
│   └── log.c/h             # 滚动日志（线程安全，最多 10 × 100 MB）
├── html/
│   ├── index.html           # 工具导航首页
│   ├── TableParse.html      # 域段解析工具
│   ├── register-viewer.html # 寄存器查看器（上传 XML/JSON 可视化解析）
│   ├── register.html        # register 归档目录文件管理
│   ├── svntools.html        # SVN 日志查询
│   ├── codechecker.html     # CodeChecker 每日报告浏览（搜索/排序/分页/内嵌预览）
│   ├── codechecker_html/    # CI 生成的 CodeChecker 报告目录（只读软链接，不入库）
│   ├── admin.html           # 网站管理（登录 + IP 统计 + 日志浏览，管理员）
│   ├── packet/              # 报文生成与解析模板
│   └── wiki/               # Wiki 阅读 / 编辑页面
│       ├── notewiki.html
│       └── wiki-auth-admin.html   # Wiki 账号权限与日志查询页（管理员）
├── simplewebserver.sh      # 管理脚本（start/stop/restart/status/build）
├── build_linux.sh          # Linux/WSL：调用 make（非 Windows 目标）
├── build_win.ps1           # Windows PowerShell：设置 OS 与 PATH 后 mingw32-make
├── build_mingw.bat         # Windows cmd：同上
├── Makefile
└── README.md
```

### 模块职责

| 模块 | 职责 |
|------|------|
| `http_handler` | 路由调度：解析请求行，按路径分发到各 API 模块；提供静态文件服务；`/api/gt-sdk-doc` 跳转与 `/api/codechecker-list` 报告列表 |
| `http_utils` | 跨模块共享工具：动态字符串缓冲（`strbuf_t`）、JSON 读写、HTTP 响应发送、URL 解码、目录创建 |
| `register_api` | 注册表 JSON/XML 文件的上传、重命名、删除及目录管理 |
| `wiki` | Markdown 文章的读写、HTML 渲染、全文搜索、分类/重命名/移动、图片上传 |
| `auth_db` | Wiki 认证与权限：用户、会话、审计日志、MD 历史备份、偏好设置持久化（SQLite） |
| `admin_api` | 管理员 API：日志文件列表、按偏移分块读取日志、按日期/路径/IP 聚合访问统计（需管理员会话） |
| `svn_api` | 调用系统 `svn log --xml`，透传 XML 结果给前端 |
| `threadpool` | 固定线程池，循环队列，`not_empty`/`not_full` 两个条件变量控制背压 |
| `log` | 线程安全滚动日志：单文件 100 MB 切换，超限时整体前移并删最旧 |
| `platform` | 平台兼容层：目录创建、时间、网络初始化等 |

## 编译选项

```bash
make debug      # 带 -g -O0 调试符号
make memcheck   # Valgrind 内存检查
make clean      # 清除构建产物
```

自定义启动参数：

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-p <port>` | 监听端口 | `8881` |
| `-t <threads>` | 工作线程数 | CPU 核数 × 1.5 |
| `-q <size>` | 任务队列长度 | `128` |
| `-l <dir>` | 日志目录 | `logs/` |

## HTTP 路由说明

解析请求行后，**路径中 `?` 与 `#` 之后会被截断**再用于静态文件路径与多数 API 匹配（例如 `/api/codechecker-list`、`.html` 页面），避免带缓存参数时 404；需查询串的 GET（如 `/api/admin-ip-stats`）使用保留查询的副本（`path_qs`）解析。

### Wiki 接口（`SQLITE3=1` 启用时）

内容读写：`/api/wiki-list`、`/api/wiki-read`、`/api/wiki-search`、`/api/wiki-save`、`/api/wiki-delete`、`/api/wiki-rename-article`、`/api/wiki-rename-cat`、`/api/wiki-delete-cat`、`/api/wiki-move-article`、`/api/wiki-mkdir`、`/api/wiki-upload`、`/api/wiki-export-pdf`、`/api/wiki-export-md-zip`、`/api/wiki-rebuild-html`、`/api/wiki-refresh-index`、`/api/wiki-cleanup-uploads`、`/api/wiki-restore-version`

回收站：`/api/wiki-trash-list`、`/api/wiki-trash-restore`、`/api/wiki-trash-empty`

认证与账号（管理员接口见各接口说明）：

| 接口 | 方法 | 说明 |
|------|------|------|
| `/api/wiki-login` | POST | 登录，写入会话 Cookie（`WIKI_SESS`） |
| `/api/wiki-logout` | POST | 退出登录并清理会话 |
| `/api/wiki-auth-status` | GET | 查询当前登录状态 |
| `/api/wiki-users` | GET | 用户列表（管理员） |
| `/api/wiki-user-save` | POST | 新增/修改用户（管理员） |
| `/api/wiki-user-delete` | POST | 删除用户（管理员） |
| `/api/wiki-audit-logs` | GET | 查询操作审计日志（管理员） |
| `/api/wiki-md-history` | GET | 查询文章历史备份（作者/管理员） |
| `/api/wiki-user-article-rank` | GET | 用户文章贡献统计 |
| `/api/wiki-notewiki-prefs` | GET/POST | Wiki 页面偏好设置（排序、布局等，需登录） |

### CodeChecker 报告接口

| 接口 | 方法 | 说明 |
|------|------|------|
| `/api/codechecker-list` | GET | 扫描 `html/codechecker_html/`（CI 只读软链接目录），返回 `files[]`（名称/大小/修改时间，按时间倒序），`index.html` 单独放 `index` 字段 |

前端 **`codechecker.html`**：置顶 index 入口卡片、文件名实时搜索、时间/名称排序、每页 100 条分页、右侧 iframe 内嵌预览（打开自动加载最新报告）。

### 管理接口（`SQLITE3=1` 启用时，需管理员会话）

| 接口 | 方法 | 说明 |
|------|------|------|
| `/api/admin-log-files` | GET | 列出日志目录内 `server_N.log`（名称/大小/修改时间，旧→新） |
| `/api/admin-log-read?file=&offset=&limit=` | GET | 按字节偏移读取日志块（自动对齐行首），`file` 严格校验 `server_N.log` |
| `/api/admin-ip-stats?file=&from=&to=&path=&ip=&top=` | GET | IP 访问聚合统计：`from`/`to` 为 `YYYY-MM-DD[ HH:MM[:SS]]` 前缀匹配，`path`/`ip` 为大小写不敏感关键字过滤，`top` 上限 500 |
| `/api/admin-ip-host?ip=` | GET | 单个 IP 反向解析主机名（`getnameinfo` 走 /etc/hosts + DNS），内存缓存：成功 24h、失败 10min |
| `/api/admin-ip-logs?ip=&file=&limit=` | GET | 检索包含指定 IP 的日志行（`file` 省略=全部文件，按时间顺序返回最后 `limit` 条，默认 500、上限 2000，`truncated` 标记是否有截断） |

前端 **`admin.html`**：复用 Wiki 账号登录（仅 `admin` 角色可进）；「IP 统计」标签页支持文件/日期时间范围/路径/IP 过滤与 TOP N，结果表点击表头按访问次数/最近访问/IP 数值序排序（再点切换升降序），主机名列自动解析前 20 行（并发 6、结果缓存，排序不丢失），可点「解析全部主机名」解析所有行；点击 IP 行在右侧面板就地预览该 IP 的日志（跟随统计的文件选择，可关闭）；「日志浏览」标签页支持加载更多、跳到尾部、关键字过滤。

默认账号：`Admin / 123456`（首次初始化 SQLite 库时自动创建，配置见 `html/wiki/sqlite_db/db.config`）。

## 平台

- **Linux**（RHEL / CentOS / Ubuntu 等）：主要目标；编译需 `_GNU_SOURCE`。
- **Windows**：可用 **MinGW-w64（WinLibs）** 编译并运行 HTTP/Wiki 等；SVN 接口返回"本构建不支持"。

## 许可证

MIT
