# Register Viewer 描述叠加层实现计划（Description Overlay Implementation Plan）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不修改任何原始寄存器文件的前提下，为 `register-viewer.html` 增加「描述叠加层」：由
`tools/build_register_descriptions.py` 从 4 本 PDF 手册生成一份按内容键索引的叠加层 JSON；前端解析
寄存器时，凡描述为空且叠加层有对应项的，用叠加层描述填充并标注来源。参考设计文档：
`docs/superpowers/specs/2026-08-31-register-viewer-description-overlay-design.md`。

**Architecture:** 叠加层放 `html/register/descriptions/`（不进入寄存器队列）；前端仿
`autoLoadLatestRegisters` 增加 `autoLoadRegisterDescriptions()` 发现并合并叠加层；解析与渲染阶段
统一走 `regKeyOf`/`fieldKeyOf` 两个精确键做查补。生成器在 `build_register_descriptions.py` 中新增
`--publish` 与 `--coverage-report`，把 PDF 目录与寄存器文件的空描述项做一次离线匹配。

**Tech Stack:** 静态 HTML/CSS/JavaScript（前端）、Python3 + pdfplumber（生成器）、Bash 静态契约测试、
现有 C 服务器构建（`make`，无需改 C 代码）。

**依赖环境：** 生成器需要 `pdfplumber`。前端契约测试与 `make` 不依赖它；未安装时可先生成/缓存
叠加层再做前端验证。

---

### Task 1: 添加失败的前端静态契约测试

**Files:**
- Create: `tests/register_viewer_description_overlay_test.sh`
- Read: `html/register-viewer.html`

- [ ] **Step 1: 写入测试**

创建 `tests/register_viewer_description_overlay_test.sh`，仿现有
`register_viewer_latest_autoload_test.sh`：

```bash
#!/usr/bin/env bash
set -euo pipefail

FILE="html/register-viewer.html"

require_text() {
  local needle="$1"
  if ! grep -Fq "$needle" "$FILE"; then
    printf 'Missing expected text: %s\n' "$needle" >&2
    exit 1
  fi
}

require_regex() {
  local regex="$1"
  if ! grep -Eq "$regex" "$FILE"; then
    printf 'Missing expected pattern: %s\n' "$regex" >&2
    exit 1
  fi
}

require_regex 'var descriptionOverlay'
require_regex 'function regKeyOf\(r\)[[:space:]]*\{'
require_regex 'function fieldKeyOf\(regKey[[:space:]]*, f\)[[:space:]]*\{'
require_regex 'function isDescriptionPath\(relPath\)[[:space:]]*\{'
require_regex 'function mergeDescriptionOverlay\(patch\)[[:space:]]*\{'
require_regex 'function autoLoadRegisterDescriptions\(\)[[:space:]]*\{'
require_regex 'function applyOverlayToRegister\(reg\)[[:space:]]*\{'
require_text 'paths.filter(isDescriptionPath)'
require_text 'descriptionOverlay'
require_text '_descSource ='
require_text 'applyOverlayToRegister(reg)'
require_text 'autoLoadRegisterDescriptions();'
```

- [ ] **Step 2: 运行以确认失败**

Run: `bash tests/register_viewer_description_overlay_test.sh`

Expected: FAIL（页面尚无 `descriptionOverlay` / 相关函数）。

---

### Task 2: 实现前端键函数与描述叠加层合并

**Files:**
- Modify: `html/register-viewer.html`（在 `parseXML` / `parseImportedJSON` 之前的工具函数区）

- [ ] **Step 1: 添加全局状态、键函数、路径过滤与合并**

在 `var allRegisters = [];` 附近的全局变量区加入：

```javascript
var descriptionOverlay = {};
var descriptionOverlayLoaded = false;

function regKeyOf(r) {
  return [r.entryType || 'reg', r.blockName || '', r.subName || '', r.regName || ''].join('\x1f');
}

function fieldKeyOf(regKey, f) {
  return [regKey, f.name || '', f.startBit, f.endBit].join('\x1f');
}

function isDescriptionPath(relPath) {
  var p = String(relPath || '').replace(/\\/g, '/');
  var parts = p.split('/');
  if (parts.shift() !== 'descriptions' || !parts.length) return false;
  return /\.json$/i.test(parts[parts.length - 1]);
}

function mergeDescriptionOverlay(patch) {
  if (!patch || typeof patch !== 'object' || !patch.registers) return;
  var regKeys = Object.keys(patch.registers);
  for (var i = 0; i < regKeys.length; i++) {
    var k = regKeys[i];
    var cur = descriptionOverlay[k] || { shortDesc: '', fullDesc: '', fields: {}, source: null };
    var src = patch.registers[k];
    if (cur.source === null && src.source) cur.source = src.source;
    if (src.shortDesc && !cur.shortDesc) cur.shortDesc = src.shortDesc;
    if (src.fullDesc  && !cur.fullDesc)  cur.fullDesc  = src.fullDesc;
    if (src.fields) {
      var fk = Object.keys(src.fields);
      for (var j = 0; j < fk.length; j++) {
        if (!cur.fields[fk[j]]) cur.fields[fk[j]] = src.fields[fk[j]];
      }
    }
    descriptionOverlay[k] = cur;
  }
  descriptionOverlayLoaded = true;
  applyOverlayToAllRegisters();
}
```

- [ ] **Step 2: 添加描述回填辅助函数**

加入（供各解析点与渲染统一调用）：

```javascript
function applyOverlayToRegister(reg) {
  var rk = regKeyOf(reg);
  var patch = descriptionOverlay[rk];
  if (!patch) return false;
  var touched = false;
  if (!reg.shortDesc && patch.shortDesc) { reg.shortDesc = patch.shortDesc; touched = true; }
  if (!reg.fullDesc  && patch.fullDesc)  { reg.fullDesc  = patch.fullDesc;  touched = true; }
  for (var i = 0; i < reg.fields.length; i++) {
    var f = reg.fields[i];
    if (f.desc) continue;
    var fk = fieldKeyOf(rk, f);
    if (patch.fields && patch.fields[fk]) {
      f.desc = patch.fields[fk];
      f._descSource = 'manual';
      touched = true;
    }
  }
  if (touched) reg._descSource = 'manual';
  return touched;
}

function applyOverlayToAllRegisters() {
  for (var i = 0; i < allRegisters.length; i++) applyOverlayToRegister(allRegisters[i]);
  if (descriptionOverlayLoaded) refreshDataView();
}
```

- [ ] **Step 3: 在解析入口调用回填**

- `parseRegElement(...)`：在 `return { ... };` 之前，先把构造好的对象存到局部变量，
  `applyOverlayToRegister(obj)`，再 `return obj`。
- `parseImportedJSON(...)`：在 `result.push({...})` 之前同样回填。
- SRAM 解析（`parseXML` 内）：同样回填。

各解析点统一本地构造对象（或先 push 再回填）均可，以不重复代码为原则。

- [ ] **Step 4: 运行契约测试**

Run: `bash tests/register_viewer_description_overlay_test.sh`

Expected: 通过（任务1 的新函数与调用均已就位）。

---

### Task 3: 实现叠加层自动发现与加载

**Files:**
- Modify: `html/register-viewer.html`（在 `autoLoadLatestRegisters` 之后、`initFileInputs();` 调用附近）

- [ ] **Step 1: 添加自动加载函数**

```javascript
function autoLoadRegisterDescriptions() {
  if (location.protocol === 'file:') {
    showToast('本地打开无法自动加载描述叠加层，请由服务器服务或使用“加载文件夹”选择 descriptions/');
    return;
  }
  fetch('/api/list-register-files')
    .then(function(resp) {
      if (!resp.ok) throw new Error('HTTP ' + resp.status);
      return resp.json();
    })
    .then(function(data) {
      if (!data || !data.ok || !Array.isArray(data.files))
        throw new Error('接口返回异常');
      var paths = data.files.filter(isDescriptionPath);
      if (!paths.length) return;
      var errors = [];
      var requests = paths.map(function(relPath) {
        var normalized = String(relPath).replace(/\\/g, '/');
        var urlPath = normalized.split('/').map(encodeURIComponent).join('/');
        return fetch('./register/' + urlPath)
          .then(function(resp) {
            if (!resp.ok) throw new Error('HTTP ' + resp.status);
            return resp.json();
          })
          .then(function(patch) {
            mergeDescriptionOverlay(patch);
          })
          .catch(function(err) {
            errors.push(normalized + ': ' + err.message);
          });
      });
      return Promise.all(requests).then(function() {
        if (errors.length) showToast('描述叠加层加载完成，失败 ' + errors.length + ' 个');
      });
    })
    .catch(function(err) {
      showToast('描述叠加层加载失败：' + err.message);
    });
}
```

- [ ] **Step 2: 启动时调用**

在 `autoLoadLatestRegisters();` 之后追加 `autoLoadRegisterDescriptions();`。

- [ ] **Step 3: 排除叠加层出现在的网络加载列表**

在网络加载弹窗文件来源于 `netScan()` 生成的 `_netFileList`：在 `_netFileList = data.files.map(...)`
前，先 `data.files = data.files.filter(function(p){ return !isDescriptionPath(p); });`
使 `descriptions/` 不列入手动网络加载（仍是寄存器文件被拒绝/混淆的风险来源）。

- [ ] **Step 4: 运行契约测试**

Run: `bash tests/register_viewer_description_overlay_test.sh`

Expected: 通过。

---

### Task 4: 渲染标注 + 缓存失效

**Files:**
- Modify: `html/register-viewer.html`

- [ ] **Step 1: 渲染标注「手册来源」**

在 `buildCard` 的字段行与寄存器描述处，对 `_descSource === 'manual'` 的元素附加徽标：

- 字段描述：`descDisplay` 若 `f._descSource === 'manual'`，在 `hl(f.desc, q)` 前加
  `<span class="badge badge-default">📖手册</span>&nbsp;`。
- 寄存器 `fullDescBlock` / `col-desc`：若 `r._descSource === 'manual'`，在 `reg-meta` 的「来源」处
  额外追加 `<span><strong>描述:</strong> 来自手册</span>`（tooltip 可展示 `_overlaySource`）。
- 可选：让 `applyOverlayToRegister` 记录 `reg._overlaySource = patch.source`，供 tooltip 展示手册+页码。

- [ ] **Step 2: 缓存失效**

- 把 `CACHE_VER` 从 `'v1'` 升到 `'v2'`，避免旧缓存不含叠加层描述。
- 将 `descriptionOverlay` 的 `version`（取 `descriptionOverlayVersion` 变量，初始由加载到的叠加层
  `patch.version` 更新）并入 `cacheKeyFor()`，叠加层重生成且版本变化时触发重新解析。

```javascript
var descriptionOverlayVersion = null;
// 在 mergeDescriptionOverlay(patch) 内：if (patch.version) descriptionOverlayVersion = patch.version;
// cacheKeyFor(file) 追加 '|' + (descriptionOverlayVersion || '')
```

- [ ] **Step 3: 运行全部静态契约测试**

Run: `bash tests/register_viewer_description_overlay_test.sh; bash tests/register_viewer_selection_test.sh; bash tests/register_viewer_latest_autoload_test.sh`

Expected: 三条全部退出 0。

---

### Task 5: 扩展生成器并首次生成叠加层

**Files:**
- Modify: `tools/build_register_descriptions.py`

- [ ] **Step 1: 增加 `--publish` 与 `--coverage-report`**

- 新增可选参数 `--publish`（默认仅干跑/打印统计，不写叠加层）。
- 新增 `--coverage-report <path>`：输出覆盖报告（填补数/未匹配列表）为 JSON。
- 保留现有 `--inspect` 行为不变。

- [ ] **Step 2: 键函数（Python 侧与 JS 完全一致）**

```python
SEP = "\x1f"

def reg_key_of(r):
    return SEP.join([str(r.get("entryType") or "reg"),
                     str(r.get("blockName") or ""),
                     str(r.get("subName") or ""),
                     str(r.get("regName") or "")])

def field_key_of(reg_key, f):
    return SEP.join([reg_key,
                     str(f.get("name") or ""),
                     str(f.get("startBit") or ""),
                     str(f.get("endBit") or "")])
```

> 必须与 Task 2/3 的 JS `regKeyOf`/`fieldKeyOf` 的拼接顺序、分隔符保持一致；加单测用黄金样例
> 交叉校验（见 Task 6）。

- [ ] **Step 3: 扫描寄存器文件、收集空描述项**

加载目标寄存器文件（默认 `html/register/latest/d10_trunk_registers_13285.json`），遍历所有
`entryType`，对 `shortDesc` / `fullDesc` 为空者记下 `reg_key_of`，对每个字段 `desc` 为空者记下
`field_key_of`。允许 `--registers <path>` 覆盖。

- [ ] **Step 4: 离线匹配 PDF 目录**

- 规格：`build_catalogs(docs)` 输出按 IP 分类的 `register`/`field` 目录。
- 匹配算法：
  1. 建索引：`{"<normalized_name>": [ {bits, text, registerTitle, ip, page} ]}`（字段）、
     `{"<normalized_name>": [ {addresses, text, ip, page} ]}`（寄存器）。
  2. 对每个空寄存器：优先用「地址 ∈ `addresses[]`」匹配（先解除 IP 前缀命名差异），否则用
     规范化名称匹配；命中则写 `shortDesc`/`fullDesc`（优先 `fullDesc` 用长文本）。
  3. 对每个空字段：在已定位寄存器的 `registerTitle` 域内按「字段名 + `startBit`/`endBit`」匹配；
     匹配不到再全局同名同 bits 匹配。命中写 `desc`。
  4. 未命中的项进出（不打断），写入覆盖报告。
- 命名归一化（`compact`/去下划线/转小写）用于缓解 `REGFILE_CEPCS0_0_FEC_CONTROL` 与手册
  `FEC_CONTROL` 这类差异；同时保留原始名，便于人工核对。

- [ ] **Step 5: 产出叠加层与覆盖报告**

- `--publish` 时写 `html/register/descriptions/register_descriptions.json`，
  含 `version`、`generatedAt`、`sourceRegisterFile`、`sourceManuals`、`registers`（键 → 补丁）。
- 打印统计：`filled/empty` 寄存器数与字段数。
- `--coverage-report` 输出未匹配列表。

- [ ] **Step 6: 运行生成器（需 pdfplumber；缺失则跳过）**

Run:
```bash
python tools/build_register_descriptions.py --publish --coverage-report tmp/desc-coverage.json
```

Expected（若已安装 pdfplumber）：生成叠加层；记录 `filled`/`empty` 统计；未匹配项进入报告便于复核。
若未安装，则记录依赖缺失，交由具备 pdfplumber 的环境执行。

---

### Task 6: 生成器键一致性测试

**Files:**
- Create: `tests/register_description_keys_test.py`
- Read: `tools/build_register_descriptions.py`, `html/register-viewer.html`

- [ ] **Step 1: 写键一致性单测（不依赖 pdfplumber）**

用 Python 直接读取 `register-viewer.html` 中 `regKeyOf`/`fieldKeyOf` 的 `\x1f` 拼接顺序，或重放
`build_register_descriptions.reg_key_of/field_key_of`，对固定样例断言字符串与前端预期一致。该测试
不 import pdfplumber，进可覆盖；主要保障 JS/Python 两套拼键规则不失一致。

- [ ] **Step 2: 运行**

Run: `python tests/register_description_keys_test.py`

Expected: 通过。此测试不依赖外部 PDF/网络/服务器。

---

### Task 7: 验证整体构建与回归

**Files:**
- Read: `Makefile`, `git status --short`

- [ ] **Step 1: 运行全部静态测试**

Run:
```bash
bash tests/register_viewer_description_overlay_test.sh; \
bash tests/register_viewer_selection_test.sh; \
bash tests/register_viewer_latest_autoload_test.sh
```

Expected: 三条均退出 0。

- [ ] **Step 2: 校验空白与构建**

Run: `git diff --check; make`

Expected: 无空白错误；C 构建成功产出 `bin/simplewebserver`（叠加层为静态 JSON，不使用/改 C 接口）。

- [ ] **Step 3: 手工验收**

由服务器服务后打开 `register-viewer.html`：
- 加载 `latest/` 寄存器文件 → 展开 `REGFILE_CEPCS0_0_FEC_CONTROL`，字段
  `FEC_BYPASS_CORRECTION_ENABLE` 显示叠加层描述 + 📖手册 徽标。
- 未匹配项仍为空；已有描述项不被覆盖。
- `descriptions/` 不出现在「网络加载」列表。

- [ ] **Step 4: 核对变更范围**

Run: `git status --short`

Expected: 仅 `html/register-viewer.html`、`tools/build_register_descriptions.py`、
新增 `html/register/descriptions/register_descriptions.json`（由生成器产出）、
`tests/register_viewer_description_overlay_test.sh`、`tests/register_description_keys_test.py`；
既有用户改动与非本次目标文件保持不变。

---

### Task 8: 提交合并

- [ ] **Step 1: 分段提交**

按逻辑分段提交（先前端契约测试与实现，再生成器与数据，再文档）。提交信息风格对齐现有 commit
（如 `feat: ...`）。仅当用户明确要求提交时执行，否则停留在工作区由用户审阅。

---

## 关键风险与处理

- **键兼容性**：JS 与 Python 拼接规则必须一致（Task 6 单测兜底）。
- **PDF 名称差异**（如 `REGFILE_*` 前缀 vs 手册 `FEC_CONTROL`）：用归一化 + 地址定位缓解；
  无法确定的写入覆盖报告由人工确认，不强行猜。
- **缓存旧数据**：升 `CACHE_VER` 到 v2，并把叠加层 `version` 并入 `cacheKeyFor`。
- **叠加层被当作寄存器**：`isDescriptionPath` 只认 `descriptions/` 前缀 + `.json`，并在
  `netScan` 中排除，避免污染寄存器队列/统计/导出。
- **本地 `file:` 打开**：无法拉取叠加层，跳过并提示；不影响寄存器自身解析。
- **生成器依赖 pdfplumber**：仅影响生成一步；前端契约测试与 `make` 不依赖它。
