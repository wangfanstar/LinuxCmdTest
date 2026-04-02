# 测试套件说明

本目录包含 `simplewebserver` 的自动化测试，涵盖单元测试和 HTTP 集成测试，支持本地运行和 CI 流水线（GitHub Actions）。

---

## 目录结构

```
tests/
├── framework.h           # 轻量测试框架（无外部依赖）
├── test_log.c            # src/log.c 单元测试
├── test_threadpool.c     # src/threadpool.c 单元测试
├── test_helpers.c        # src/http_handler.c 内部函数单元测试
├── test_http_api.sh      # HTTP 集成测试（curl）
└── run_tests.sh          # 主控脚本
```

---

## 快速开始

所有命令在项目根目录执行。

### 编译服务器并运行全量测试

```bash
make test
```

这会依次执行：编译服务器 → 编译测试二进制 → 运行单元测试 → 启动服务器并运行集成测试。

### 仅运行单元测试（无需启动服务器）

```bash
make test-unit
```

### 仅运行集成测试

```bash
make test-integration
```

### 只编译测试二进制（不运行）

```bash
make test-build
```

---

## 测试文件说明

### `test_log.c` — 日志模块单元测试

测试 `src/log.c` 的公开接口。

| 用例 | 验证内容 |
|------|---------|
| `test_init_creates_directory` | `log_init()` 自动创建日志目录 |
| `test_init_creates_log_file` | 初始化后生成 `server_0.log` |
| `test_write_info_appears_in_file` | 写入内容确实落盘 |
| `test_write_all_levels` | DEBUG / INFO / WARN / ERROR 四级均可写入 |
| `test_write_grows_file` | 多次写入文件持续增大 |
| `test_close_is_idempotent` | `log_close()` 重复调用不崩溃 |
| `test_reinit_after_close` | 关闭后可以重新初始化 |
| `test_log_format_includes_timestamp` | 每行包含时间戳字段 |
| `test_concurrent_writes` | 4 线程并发写入不产生竞态（数据量校验） |

编译命令：

```bash
gcc -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -Isrc -Itests \
    tests/test_log.c src/log.c -lpthread -o tests/test_log
```

---

### `test_threadpool.c` — 线程池单元测试

测试 `src/threadpool.c` 的公开接口。文件内包含 `handle_client()` 的 stub 实现（使用 `pthread_cond` 计数，等待任务完成），无需真实 HTTP 连接。

| 用例 | 验证内容 |
|------|---------|
| `test_create_basic` | 正常创建，返回非 NULL |
| `test_create_single_thread` | 单线程 + 队列长度 1 的极端参数 |
| `test_create_many_threads` | 8 线程 + 64 队列 |
| `test_submit_one_task` | 提交 1 个任务，确认执行 |
| `test_submit_many_tasks_all_run` | 提交 30 个任务，全部执行 |
| `test_submit_returns_0_on_success` | 成功提交返回 0 |
| `test_destroy_waits_for_in_flight_tasks` | destroy 等待所有运行中任务完成 |
| `test_submit_negative1_after_shutdown` | shutdown 后 destroy 不崩溃 |
| `test_single_thread_queue_full_blocks_then_drains` | 队列满时 submit 阻塞，任务执行后解除 |

编译命令：

```bash
gcc -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -Isrc -Itests \
    tests/test_threadpool.c src/threadpool.c src/log.c -lpthread \
    -o tests/test_threadpool
```

---

### `test_helpers.c` — HTTP Handler 内部函数单元测试

测试 `src/http_handler.c` 中的纯逻辑函数：JSON 解析助手和动态字符串缓冲区。

**编译策略**：通过 `#include "../src/http_handler.c"` 将源文件直接并入同一编译单元，使 `static` 函数可被测试直接调用。`log_write()` 和 `ssh_exec` 系列函数以 stub 形式就地定义，**不修改任何源文件**，不需要链接 `http_handler.c` 或 `log.c`。

#### json_get_str（9 个用例）

| 用例 | 验证内容 |
|------|---------|
| `test_json_get_str_basic` | 提取第一个字符串字段 |
| `test_json_get_str_second_key` | 提取非首字段 |
| `test_json_get_str_missing_key_returns_neg1` | 缺失 key 返回 -1 |
| `test_json_get_str_empty_value` | 空字符串值 `""` |
| `test_json_get_str_escaped_backslash` | `\\` → `\` |
| `test_json_get_str_escaped_newline` | `\n` 转义 |
| `test_json_get_str_escaped_tab` | `\t` 转义 |
| `test_json_get_str_spaces_around_colon` | 冒号两侧有空格 |
| `test_json_get_str_truncates_to_len` | 超出 len 时截断并保证 NUL |

#### json_get_int（6 个用例）

| 用例 | 验证内容 |
|------|---------|
| `test_json_get_int_basic` | 提取整数 |
| `test_json_get_int_large` | 大整数（65535） |
| `test_json_get_int_zero` | 值为 0 |
| `test_json_get_int_missing_key_returns_default` | 缺失 key 返回 def |
| `test_json_get_int_string_value_returns_default` | 值是字符串时返回 def |
| `test_json_get_int_multiple_keys` | 同 JSON 中提取多个 int 字段 |

#### json_get_str_array（6 个用例）

| 用例 | 验证内容 |
|------|---------|
| `test_str_array_single_element` | 单元素数组 |
| `test_str_array_multiple_elements` | 多元素，按序赋值 |
| `test_str_array_empty_array` | 空数组返回 0 |
| `test_str_array_missing_key` | 缺失 key 返回 0 |
| `test_str_array_respects_max_count` | max_count 限制 |
| `test_str_array_escaped_element` | 元素内含转义引号 |

#### json_api_get_pass（5 个用例）

| 用例 | 验证内容 |
|------|---------|
| `test_pass_basic` | 提取 `pass` 字段 |
| `test_pass_null_gives_empty` | `null` 值 → 空字符串 |
| `test_pass_missing_gives_empty` | 无 pass 字段 → 空字符串 |
| `test_pass_uses_root_key_only` | 嵌套对象中的 `pass` 不被匹配（安全） |
| `test_password_alias` | `password` 别名 |

#### strbuf_t（9 个用例）

| 用例 | 验证内容 |
|------|---------|
| `test_sb_append_basic` | 基本追加 |
| `test_sb_append_grows` | 1000 次追加触发多次 realloc |
| `test_sb_appendf_format` | printf 格式化 |
| `test_sb_appendf_long_string` | 超 4096 字节走堆分配分支 |
| `test_sb_json_str_plain` | 普通字符串 → `"hello"` |
| `test_sb_json_str_quotes` | 内嵌引号 → `\"` |
| `test_sb_json_str_backslash` | 反斜杠 → `\\` |
| `test_sb_json_str_newline` | `\n` → `\n`（JSON） |
| `test_sb_json_str_control_char` | ASCII < 0x20 → `\u00xx` |

编译命令：

```bash
gcc -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -Isrc -Itests \
    -Wno-unused-function \
    tests/test_helpers.c -lpthread -o tests/test_helpers
```

---

### `test_http_api.sh` — HTTP 集成测试

脚本自动完成：启动服务器（后台）→ 等待就绪 → 执行 curl 测试 → 退出时杀掉服务器（`trap` 保证清理）。

**环境变量**

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `TEST_PORT` | `18881` | 服务器监听端口（避免与 8881 冲突） |
| `SERVER_BIN` | `./bin/simpleserver` | 服务器二进制路径 |

**覆盖的端点**

| 分类 | 测试点 |
|------|--------|
| 静态文件 | `GET /` 返回 200；`/nonexist.html` 返回 404；路径穿越 `/../` 返回 400 |
| client-info | 返回 200、`ok:true`、包含 `ip` 字段 |
| reports | 返回 200、`ok:true`、包含 `files` 字段 |
| list-all-configs | 返回 200、`ok:true` |
| cancel | `POST /api/cancel` 返回 `ok:true` |
| 请求体校验 | 空 body → 400（ssh-exec / ssh-exec-stream / ssh-exec-one / save-report / save-config / delete-report） |
| kill pid 校验 | pid=0 / pid=-1 → `ok:false` |
| Content-Type | JSON 端点返回 `application/json`；HTML 文件返回 `text/html` |
| CORS | JSON 响应包含 `Access-Control-Allow-Origin` |

手动运行：

```bash
# 默认端口 18881
bash tests/test_http_api.sh

# 指定端口
TEST_PORT=19000 bash tests/test_http_api.sh
```

---

## 内存检查（Valgrind）

为单元测试构建 debug 版并用 Valgrind 检查：

```bash
make test-build-debug

valgrind --leak-check=full --show-leak-kinds=all \
         --track-origins=yes --error-exitcode=1 \
         tests/test_log_dbg

valgrind --leak-check=full --show-leak-kinds=all \
         --track-origins=yes --error-exitcode=1 \
         tests/test_threadpool_dbg

valgrind --leak-check=full --show-leak-kinds=all \
         --track-origins=yes --error-exitcode=1 \
         tests/test_helpers_dbg
```

---

## CI / GitHub Actions

推送到 `master` 或创建 PR 时自动触发（`.github/workflows/ci.yml`）：

```
1. ubuntu-latest 环境安装 gcc / make / curl / valgrind
2. make all              ← 编译服务器
3. make test-build       ← 编译测试二进制
4. make test-unit        ← 运行单元测试
5. make test-integration ← 启动服务器 + curl 集成测试
6. Valgrind 检查三个单元测试二进制（debug 版）
7. 失败时上传服务器日志为 artifact
```

---

## 添加新测试

### 添加单元测试用例

在对应的 `test_*.c` 文件末尾添加函数，并在 `main()` 中用 `RUN_TEST()` 注册：

```c
static void test_my_new_case(void)
{
    char out[64];
    int r = json_get_str("{\"key\":\"val\"}", "key", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0);
    TEST_ASSERT_STR_EQ(out, "val");
}

// 在 main() 中：
RUN_TEST(test_my_new_case);
```

### 添加集成测试断言

在 `test_http_api.sh` 中调用现有的辅助函数：

```bash
# 检查 HTTP 状态码
check_status "描述" "${BASE}/api/my-endpoint" "200"

# 检查响应体包含字符串
check_body_contains "描述" "${BASE}/api/my-endpoint" '"key":"value"'

# POST 请求检查状态码
check_post_status "描述" "${BASE}/api/my-endpoint" '{"x":1}' "200"

# POST 请求检查响应体
check_post_body "描述" "${BASE}/api/my-endpoint" '{"x":1}' '"ok":true'
```

### 添加新的测试文件

1. 创建 `tests/test_mymodule.c`（参照 `test_log.c` 结构）
2. 在 `Makefile` 中添加编译目标：
   ```makefile
   $(TEST_DIR)/test_mymodule: $(TEST_DIR)/test_mymodule.c $(SRC_DIR)/mymodule.c
       $(CC) $(TCFLAGS) -o $@ $^ $(TLDFLAGS)
   ```
3. 将变量名加入 `test-unit` 目标的循环列表

---

## 依赖要求

| 工具 | 用途 | 最低版本 |
|------|------|---------|
| gcc | 编译 C 测试 | 7.0（支持 `-std=c11`） |
| make | 构建 | 3.81 |
| curl | 集成测试 HTTP 请求 | 7.x |
| bash | 集成测试脚本 | 4.0 |
| valgrind | 内存检查（可选） | 3.14 |
