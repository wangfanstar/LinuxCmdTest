# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
make              # 编译（输出 bin/simplewebserver）
make run          # 编译后以默认端口 8881 运行
make clean        # 清除 obj/ 和 bin/
make debug        # 带 -g -O0 的调试构建
make memcheck     # Valgrind 内存检查（需先 make debug）
```

自定义启动：

```bash
./bin/simplewebserver -p 9000 -t 8 -q 256 -l /tmp/logs
```

## 架构概览

四个模块，职责分明，依赖方向单向：

```
main.c
  ├─ threadpool.c/h   线程池（循环队列 + mutex/cond）
  ├─ http_handler.c/h HTTP 解析与文件服务
  └─ log.c/h          滚动日志（线程安全）
```

**main.c**：创建 TCP socket → 初始化线程池 → accept 循环 → 将 `client_task_t`（含 fd 和客户端地址）提交到线程池。收到 SIGINT/SIGTERM 后优雅关闭。

**threadpool.c**：生产者-消费者模型。固定大小循环队列；`not_empty` / `not_full` 两个条件变量控制阻塞；`shutdown` 标志让所有工作线程在队列排空后退出。

**http_handler.c**：GET 提供静态文件（`html/<path>`）与只读 JSON API；POST 处理 `/api/ssh-exec*`、`/api/save-report`、`/api/save-config`、`/api/delete-report` 等。解析请求行后去掉 path 中的 `?` / `#` 后缀再匹配路由与静态路径；需查询串的 GET（如 `list-ssh-configs`、`procs`、`port`）使用保留查询的副本解析。报告列表见 `GET /api/reports`，存档目录 `html/report/`（`http_handler.c` 内扫描与校验）。

**log.c**：全局互斥锁保护文件句柄；单文件写满 100 MB 后自动切换到下一序号（`server_N.log`）；超过 10 个文件时将最旧的删除后整体前移序号（rotate_files）。使用 `_IONBF` 关闭用户空间缓冲。

## 关键常量位置

| 常量 | 文件 | 说明 |
|------|------|------|
| `WEB_ROOT` | `http_handler.h` | 静态文件根目录，默认 `"html"` |
| `LOG_MAX_SIZE` | `log.h` | 单日志文件上限，默认 100 MB |
| `LOG_MAX_FILES` | `log.h` | 最多保留日志文件数，默认 10 |
| `DEFAULT_PORT` | `main.c` | 默认监听端口 8881 |
| `MAX_THREADS` | `main.c` | 线程数上限 64 |

## 平台说明

- 目标平台：Linux / macOS（或 WSL）；`_mkdir` 有条件编译分支支持 Windows。
- 编译标志：`-D_GNU_SOURCE`（启用 `strdup` 等扩展）、`-std=c11`、`-lpthread`。

## 编码要求
1、当前代码是在win下平台开发维护，实际代码要运行在linux平台编译运行，本地优先用WSL环境编译验证。

## 环境

- **Shell**: Git Bash / WSL | **OS**: Windows 11 / WSL2 | **Runtime**: Node.js
- wsl sudo的密码是 wangf
- **ROOT**: `E:/MCP_PROJECT/wfserver/EMU_TEST_WEB/`
- **HTML_DIR**: `E:/MCP_PROJECT/wfserver/EMU_TEST_WEB/html/`
- ssh连接测试环境，远程主机IP:49.233.175.250  用户名：ubuntu  密码： W1_brysj

```
EMU_TEST_WEB/
├── CLAUDE.md
├── src/
└── html/         ← 工作目录 + Web根目录
    ├── *.html/*.js
    └── report/
```

**WSL路径映射**: `E:/` → `/mnt/e/`

---

## 兼容性

- **Firefox**: 68.10.0esr (64-bit) for RHEL
- **禁止**: ?. (可选链, FF74+)/?? (空值合并, FF72+)
- **可用**: async/await (FF52+)/fetch (FF39+)/箭头函数 (FF22+)/ES6模块/CSS Grid

---

## 禁止行为

| ❌ 禁止 | ✅ 替换 |
|--------|--------|
| CMD命令 (`cd /d`/`del`/`copy`/`move`/`dir`/`%VAR%`) | Git Bash命令 (`cd`/`rm`/`cp`/`mv`/`ls`/`$VAR`) |
| 反斜杠路径 `E:\path` | 正斜杠 `E:/path` (Bash) 或 `/mnt/e/path` (WSL) |
| `node -e "多行代码"` | 写`.js`文件再执行（见模板） |
| 裸`readFileSync('./file')` | `path.join(__dirname, 'file')` |
| `replace('</body>', ...)` | `safeInject`多候选模式 |
| heredoc内含反斜杠正则 | `split/join`或`JSON.stringify` |
| Update工具改>3行 | Node.js脚本 |
| HTML注入前不扫描 | `scanInjectPoints`先行 |
| Node测试调用浏览器函数 | 重新定义Node版函数 |

---

## 脚本模板

```js
'use strict';
const path = require('path');
const fs = require('fs');
const ROOT = path.join(__dirname, '..');
const HTML_DIR = __dirname;
console.log('[ENV] ROOT:', ROOT, '| HTML_DIR:', HTML_DIR, '| Node:', process.version);

function safeRead(p) {
  if (!fs.existsSync(p)) { console.error('[ENOENT]', p); process.exit(1); }
  return fs.readFileSync(p, 'utf8').replace(/\r\n/g, '\n');
}
function safeWrite(p, c) {
  const d = path.dirname(p); if (!fs.existsSync(d)) fs.mkdirSync(d, { recursive: true });
  fs.writeFileSync(p, c, 'utf8'); console.log('[OK]', p);
}
function safeInject(c, patterns, inject, pos = 'before') {
  for (const p of patterns) {
    const i = c.indexOf(p);
    if (i !== -1) return c.slice(0, pos === 'before' ? i : i + p.length) + inject + c.slice(pos === 'before' ? i : i + p.length);
  }
  console.error('[INJECT FAILED]', patterns); process.exit(1);
}
function scanInjectPoints(p) {
  const c = fs.readFileSync(p, 'utf8').replace(/\r\n/g, '\n');
  ['</head>','</body>','<body','<script','</script>'].forEach(t => {
    const i = c.toLowerCase().indexOf(t.toLowerCase());
    console.log(i !== -1 ? `[${t}] pos:${i}` : `[${t}] MISSING`);
  });
}

// 业务逻辑
try {
  const p = path.join(HTML_DIR, 'target.html');
  scanInjectPoints(p);
  let c = safeRead(p);
  c = safeInject(c, ['</body>', '</BODY>', '  </body>'], '\n<!-- inject -->\n');
  safeWrite(p, c);
} catch(e) { console.error('[ERROR]', e.message); process.exit(1); }
```

**执行**: `cd "E:/MCP_PROJECT/wfserver/EMU_TEST_WEB/html" && node _tmp_task.js 2>&1 && rm -f _tmp_task.js`

---

## 转义规则

```js
// 方案A: split/join（推荐）
code.split('\\').join('\\\\').split("'").join("\\'").split('\n').join('\\n');

// 方案B: JSON.stringify（自动转义）
JSON.stringify(content);

// 方案C: Base64
Buffer.from(content).toString('base64');
```

---

## Update工具限制

**禁止Update**：old_string>3行 | 含转义字符 | HTML拼接代码 | 报错`String to replace not found`

**报错时**: 脚本`content.indexOf(keyword)`定位 → `JSON.stringify(content.slice(...))`输出 → 构建safeInject patterns → **禁止重试Update**

---

## 嵌套HTML `<script>` 转义

**唯一正确写法**:
```js
'<scr'+'ipt>'       // 开标签拆散
'<\/scr'+'ipt>'     // 闭标签: \/输出/, 父页安全
```

**验证3项**:
```js
// 1. 源码无裸</script>
if (fnCode.indexOf('</script>') !== -1) throw 'FAIL:父页截断';
// 2. 输出含真实</script>
if (output.indexOf('</script>') === -1) throw 'FAIL:无法闭合';
// 3. 内嵌JS语法正确
new Function(output.match(/<script>([\s\S]*?)<\/script>/)?.[1] || '');
```

---

## WSL使用说明

```bash
# 路径映射
E:/MCP_PROJECT/wfserver/EMU_TEST_WEB → /mnt/e/MCP_PROJECT/wfserver/EMU_TEST_WEB

# 进入项目
cd /mnt/e/MCP_PROJECT/wfserver/EMU_TEST_WEB/html

# 执行脚本
node your_script.js

# Node.js需在WSL内安装: sudo apt install nodejs npm
```

---

## Checklist

- [ ] 路径正斜杠(Bash)或`/mnt/`(WSL)
- [ ] 无CMD命令(`del`/`copy`/`dir`)
- [ ] 无`node -e`多行 → 写文件执行
- [ ] 脚本含`ROOT`/`HTML_DIR`初始化和工具函数
- [ ] 文件操作经`safeRead`/`safeWrite`
- [ ] HTML注入先`scanInjectPoints`后`safeInject`多候选
- [ ] 无裸`</script>`→ 用`'<\/scr'+'ipt>'`
- [ ] Update受限 → 用Node脚本
- [ ] 临时文件`rm -f`清理
