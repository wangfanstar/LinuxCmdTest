# Linux 命令行测试工具

基于 C 语言的轻量级 HTTP 服务器，内置 SSH 批量命令执行与结果比对功能，适用于 Linux 系统巡检、版本验证和回归测试场景。

## 功能概览

| 模块 | 说明 |
|------|------|
| SSH 命令行测试 | 输入 SSH 连接信息，批量执行命令，生成可编辑 HTML 报告 |
| 预期结果比对 | 导入预期结果 JSON，与实际执行输出并列对比，自动判断通过/不符 |
| 命令匹配标记 | 每条命令可单独设置：默认 `=` / 模糊 `~` / 忽略 `!` |
| 配置导入导出 | SSH 连接配置与命令列表可保存为 JSON 文件复用；可选字段 `reportRemark`（整份报告备注） |
| 报告存档与列表 | 执行结果可存档到 `html/report/`；`reports.html` 浏览 HTML/JSON，支持筛选、排序、删除 |
| 域段解析 | `TableParse.html`：Length/Range 解析 hex/dec/bin，嵌套域段与 JSON 配置 |
| 日志查看器 | 实时查看服务器日志，支持级别过滤、关键字搜索、本地文件上传 |

## 快速开始

### 编译（Linux / WSL / macOS）

在 **非 Windows** 终端中（未设置 `OS=Windows_NT`），使用完整 Linux 源码（含 `monitor.c`、`ssh_exec.c`）：

```bash
chmod +x build_linux.sh   # 仅首次
./build_linux.sh          # 或直接: make
```

产出：`bin/simplewebserver`

若误在 MSYS 里带着 `OS=Windows_NT` 环境变量，可先 `unset OS` 再 `make`。

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

**说明（Windows 构建）**：`Makefile` 在 `OS=Windows_NT` 时会链接 `-lws2_32`，并改用 `monitor_win.c`、`ssh_exec_win_stub.c`（无 `/proc` 与 Unix 版 SSH 引擎）。SVN 接口在 Windows 上返回“本构建不支持”；完整监控与 SSH 仍以 Linux 构建为准。

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
- **OpenSSH 客户端**（`ssh` 命令，≥ 6.x；**仅 Linux 运行完整 SSH 引擎时需要**）

无需 `sshpass`，密码认证通过 `SSH_ASKPASS` 机制实现。

## 项目结构

```
.
├── src/
│   ├── main.c              # 入口：参数解析、socket、accept 循环
│   ├── http_handler.c/h    # HTTP 请求调度（路由分发）、静态文件服务
│   ├── http_utils.c/h      # 共享工具：strbuf、JSON 解析/构建、HTTP 响应、URL 解码
│   ├── report_api.c/h      # 报告与配置存档管理（/api/save-report、/api/reports 等）
│   ├── register_api.c/h    # 注册表文件管理（/api/save-register-file 等）
│   ├── wiki.c/h            # Wiki 引擎（Markdown→HTML、搜索、上传、CRUD）
│   ├── svn_api.c/h         # SVN 日志查询（/api/svn-log）
│   ├── ssh_api.c/h         # SSH 命令执行（单条 / 批量 / SSE 流式）
│   ├── monitor.c/h         # 系统监控（CPU、内存、进程、在线用户统计）
│   ├── ssh_exec.c/h        # SSH 底层执行引擎（fork/pipe/execvpe）
│   ├── threadpool.c/h      # 线程池（生产者-消费者，循环队列）
│   └── log.c/h             # 滚动日志（线程安全，最多 10 × 100 MB）
├── html/
│   ├── index.html           # 工具导航首页
│   ├── linux_cmd_test.html  # SSH 命令行测试主页面
│   ├── reports.html         # 已存档报告列表（调用 /api/reports）
│   ├── TableParse.html      # 域段解析工具
│   ├── logviewer.html       # 日志查看器
│   └── wiki/               # Wiki 阅读 / 编辑页面
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
| `http_handler` | 路由调度：解析请求行，按路径分发到各 API 模块；提供静态文件服务 |
| `http_utils` | 跨模块共享工具：动态字符串缓冲（`strbuf_t`）、JSON 读写、HTTP 响应发送、URL 解码、目录创建 |
| `report_api` | 报告与 SSH 配置文件的存取、列表扫描、删除 |
| `register_api` | 注册表 JSON/XML 文件的上传、重命名、删除及目录管理 |
| `wiki` | Markdown 文章的读写、HTML 渲染、全文搜索、分类/重命名/移动、图片上传 |
| `svn_api` | 调用系统 `svn log --xml`，透传 XML 结果给前端 |
| `ssh_api` | 调用 `ssh_exec` 引擎，支持单条、批量、SSE 流式三种执行模式 |
| `monitor` | 读取 `/proc/stat`、`/proc/meminfo`、`/proc/[pid]/stat` 等，输出 CPU / 内存 / 进程 / 在线用户 JSON |
| `ssh_exec` | SSH 底层：`fork`+`pipe`+`execvpe` 驱动 OpenSSH 子进程，PTY 模式支持交互式设备 |
| `threadpool` | 固定线程池，循环队列，`not_empty`/`not_full` 两个条件变量控制背压 |
| `log` | 线程安全滚动日志：单文件 100 MB 切换，超限时整体前移并删最旧 |

## SSH 命令行测试

### 基本流程

1. 填写 SSH **主机 / IP**、**端口**、**用户名**、**密码**
2. 添加要执行的命令（支持预设模板、文件导入）
3. 点击 **⚡ 测试连接** 验证连接可用性
4. 点击 **▶ 执行** 批量执行，右侧生成报告
5. 点击 **↓ 下载** 将报告保存为独立 HTML 文件  
6. 执行报告**终端窗口底部**可填写**报告备注**（与每条命令下的备注不同）；会写入下载/存档 HTML、导出 JSON、服务器配置 JSON 及浏览器本地恢复状态

### 报告存档目录与接口

存档默认写入 **`html/report/<SSH用户名>/<服务器年月 YYYYMM>/`**（兼容旧路径 `html/report/<YYYYMM>/`）。

| 接口 | 方法 | 说明 |
|------|------|------|
| `/api/reports` | GET | JSON：`groups[]` 含 `legacy`、`user`、`ym`、`files[]`（`name`、`kind` 为 html 或 json、`mtime`、`size`） |
| `/api/delete-report` | POST | JSON：`legacy`、`ym`、`name`；非 legacy 时必填 `user`；删除上述目录下对应文件 |

前端 **`reports.html`**：按类型筛选（全部 / 仅 HTML / 仅 JSON）、组内按文件名或修改时间排序、打开链接与删除。

### 命令匹配标记

在命令左侧点击标记按钮循环切换：

| 标记 | 含义 |
|------|------|
| `=` 默认 | 跟随全局精确 / 模糊匹配开关 |
| `~` 模糊 | 此命令始终忽略空白差异 |
| `!` 忽略 | 跳过此命令的比对，固定显示「跳过」 |

### 预期结果比对

1. 执行命令后点击工具栏 **导出预期**，将当前输出保存为 JSON 基线
2. 下次执行后点击 **导入预期**，自动进行并列比对
3. **对比视图** 按钮切换比对视图与单结果视图
4. **模糊匹配 / 精确匹配** 按钮控制全局匹配模式

### 命令文件格式（.txt）

```
# 注释行（以 # 开头）会被跳过
uname -a
~ df -h          # ~ 前缀：模糊匹配
! free -h        # ! 前缀：忽略比对
```

### SSH 配置文件格式（.json）

```json
{
  "version": 1,
  "host": "192.168.1.100",
  "port": 22,
  "user": "root",
  "pass": "password",
  "commands": [
    { "cmd": "uname -a", "marker": "" },
    { "cmd": "df -h",    "marker": "fuzzy" },
    { "cmd": "free -h",  "marker": "ignore" }
  ]
}
```

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

解析请求行后，**路径中 `?` 与 `#` 之后会被截断**再用于静态文件路径与多数 API 匹配（例如 `/api/reports`、`.html` 页面），避免带缓存参数时 404。  
仍依赖查询串的接口（如 `/api/list-ssh-configs?user=`、`/api/procs?`、`/api/port?`）在服务端使用**保留查询的完整路径**解析参数。

## 平台

- **Linux**（RHEL / CentOS / Ubuntu 等）：主要目标；编译需 `_GNU_SOURCE`；运行完整 SSH/监控需 `openssh-client`，服务端需开启密码认证。
- **Windows**：可用 **MinGW-w64（WinLibs）** 编译并运行 HTTP/Wiki/报告等；监控与 SSH 使用存根实现，行为见上文「编译（Windows）」说明。

## 许可证

MIT
