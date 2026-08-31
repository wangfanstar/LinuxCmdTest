# Register Viewer 描述叠加层（Sidecar Description Overlay）设计

## 背景与问题

`html/register-viewer.html` 解析 XML/JSON 寄存器文件后，会展示每个寄存器的 `shortDesc` / `fullDesc`，以及每个位域的 `desc`。但源寄存器文件中有大量描述为空：

- 例：`REGFILE_CEPCS0_0_FEC_CONTROL`（indirect，地址 `0x100`）的字段
  `FEC_BYPASS_CORRECTION_ENABLE` 的 `description` 为空字符串。

这些描述可以在 4 本 PDF 手册中找到：

- `CEFEC_reference_guide_v1.1.pdf`
- `CEMAC_reference_guide_v1.7.pdf`
- `CEPCS_reference_guide_v1.82.pdf`
- `CESOCX16_WRAP_reference_guide_v1.0.pdf`

仓库已存在抽取工具 `tools/build_register_descriptions.py`，能把上述手册的地址表/字段表抽成
`kind=register/field` 的目录（含 `name` / `address` / `startBit` / `endBit` / `text`）。
当前它只支持 `--inspect`（写缓存后退出），尚未配置发布。

## 目标

在不修改任何原始寄存器文件（`d10_trunk_registers_13285.json` 等）的前提下：

1. 为无描述的寄存器/位域生成一份**描述叠加层**（JSON）。
2. `register-viewer.html` 解析原始寄存器文件时，若发现某寄存器/位域描述为空，但有相匹配的
   叠加层描述，则加载叠加层描述作为兜底。
3. 该方案通用、可维护、易复现，不依赖具体文件名/服务器路径。

## 核心思路

**关键洞察**：把「PDF 手册 ↔ 寄存器文件」之间不精确的跨源匹配，放到线下的生成工具中一次完成。
交给浏览器的是按寄存器内容精确索引的查表数据。运行时只做字符串查找，不携带任何模糊匹配逻辑，
从而健壮、快速、易维护。

叠加层只「补空」，不覆盖：

- 原始文件永远不改动。
- 只有当描述为空时才用叠加层填充；若原始已有描述，原始值为准（叠加层是兜底）。

## 术语

- **原始寄存器文件**：被 `register-viewer.html` 解析、展示寄存器的 XML/JSON（如
  `d10_trunk_registers_13285.json`）。
- **叠加层（overlay）**：新生成的 JSON 描述文件，仅用于填补空描述。
- **寄存器键 / 字段键**：由寄存器文件内容算出的精确索引字符串。

## 方案（推荐 A）

### 目录与文件

叠加层与被说明的寄存器文件放在**同一文件夹**，名为 `<寄存器文件名>.descriptions.json`（同名后缀，
便于维护）。例如：

```
html/register/latest/d10_trunk_registers_13285.json
html/register/latest/d10_trunk_registers_13285.descriptions.json
```

默认文件名由 `--publish` 依 `--registers` 生成：`<寄存器文件名去后缀>.descriptions.json`。
可用 `--output-dir` / `--output` 覆盖。`build_register_descriptions.py` 默认即输出到
寄存器文件所在目录。

该文件是叠加层，不是寄存器：前端用 `isDescFileName` 区分，**不会**进入 `pendingFiles` 队列，
也不会出现在 `netScan` 的寄存器列表。

### 叠加层 Schema

```json
{
  "version": 1,
  "generatedAt": "2026-08-31T00:00:00Z",
  "sourceRegisterFile": "d10_trunk_registers_13285.json",
  "sourceManuals": [
    "CEPCS_reference_guide_v1.82.pdf",
    "CEFEC_reference_guide_v1.1.pdf"
  ],
  "registers": {
    "<regKey>": {
      "shortDesc": "……",
      "fullDesc": "……",
      "fields": { "<fieldKey>": "描述文本" },
      "source": { "manual": "CEPCS_reference_guide_v1.82.pdf", "pdfPage": 68 }
    }
  }
}
```

字段说明：
- `version`：叠加层格式版本，便于前端缓存失效与兼容判断。
- `generatedAt`：生成时间（仅提示用途）。
- `sourceRegisterFile`：生成时扫描的寄存器文件名（仅供人工核对）。
- `sourceManuals`：本次生成使用到的手册清单。
- `registers`：**键 → 补丁**。键为寄存器键，值为可为空字符串的 `shortDesc` / `fullDesc`
  与 `fields` 映射。
- `source`：可选。补充描述来源（手册 + PDF 页码），用于前端 tooltip 与 QA。

### 匹配键（生成器与前端必须完全一致）

键由寄存器文件内容拼接，不包含任何服务器路径或文件名。用不可见分隔符 `\x1F` 拼接以防歧义。

- **寄存器键**：
  ```
  entryType + "\x1F" + blockName + "\x1F" + subName + "\x1F" + regName
  ```
  （刻意**不含地址**，这样有 `copies` 的寄存器共用一份描述，天然正确。）

- **字段键**：
  ```
  <regKey> + "\x1F" + fieldName + "\x1F" + startBit + "\x1F" + endBit
  ```

前端在 `parseRegElement` / `parseImportedJSON` / SRAM 解析处均能算出这两个键（对应的 `entryType`、
`blockName`、`subName`、`regName`、`fieldName`、`startBit`、`endBit` 都来自寄存器对象本身）。

> 注意：键格式需在生成器（Python）与前端（JS）间保持单一事实来源。建议在前端提供
> `regKeyOf(reg)` / `fieldKeyOf(reg, field)` 两个函数，生成器用等价的 Python 函数，二者
> 的拼接顺序/分隔符写入本文档并定期用黄金样例交叉校验。

### 生成（扩展 `tools/build_register_descriptions.py`）

- 步骤 A：抽取 4 本手册目录（现有逻辑，`--inspect`）。
- 步骤 B：加载目标寄存器 JSON，遍历所有寄存器/字段；对描述为空的项算出 `regKey` / `fieldKey`。
- 步骤 C：与目录匹配：
  - 寄存器：按「地址 ∈ `addresses[]`」且「名称一致」匹配，落到具体 IP/域。
  - 字段：在匹配到的寄存器的 `registerTitle` 下按「字段名 + `startBit`/`endBit`」匹配。
- 步骤 D：产出叠加层 JSON。新增 `--publish` 模式（当前 `--inspect` 后即退出）。
- 新增 `--coverage-report`：统计空缺被填补数、未匹配数，未匹配项写入报告供人工复核。

匹配采用**宽松 + 可复核**策略：能精确匹配的写入；无法确定的先不进叠加层，由覆盖率报告提示
人工确认（必要时在工具中增加手工映射表）。
另有「全名汇总字段兜底」：当寄存器级描述文本含 `[N] 名称` / `[Hi:Lo] 名称` 行时，按其与
字段名归一化比对，把汇总文本拆成逐字段描述。

### 运行时（前端已实现）

- 自动加载：`autoLoadRegisterDescriptions()` 拉取 `/api/list-register-files`，过滤
  `isDescFileName`（`*.descriptions.json`），逐个 `fetch('./register/' + urlPath)`，
  解析后合并进 `descriptionOverlay`，并写入 `descFiles` 列表（可单独移除）。
- 默认同名后缀：`foo.json` ⇄ `foo.descriptions.json`，同目录即可自动识别加载；`autoLoadLatestRegisters`
  / `netScan` / 文件夹加载均把描述文件与寄存器文件分开，不会混入寄存器队列。
- 手动增删：
  - 「＋ 描述」= `pickDescFiles()` 选择任意描述 JSON 加载。
  - 「×」= `removeDescFile(id)`；「移除描述」= `removeAllDescFiles()`。
  - 移除时 `rebuildDescOverlay()` 由剩余项重放叠加，保证回退干净。
- 解析钩子：`parseRegElement` / `parseImportedJSON` / SRAM 构建完 `shortDesc` / `fullDesc` /
  `fields[]` 之后，若为空则查 `descriptionOverlay` 填充，并标 `_descSource = 'manual'`。
- 渲染：`buildCard` 对 `_descSource === 'manual'` 的描述加「📖手册」徽标；`reg-meta` 显示来源手册。
- 帮助：侧栏「📘 使用说明」折叠面板说明命名约定与用法。
- 排除项：`netScan()` 列表与 `netModalLoad` 过滤掉描述文件；`saveSelectedToRegister()` /
  `exportAll()` 只处理寄存器。
- 缓存：`CACHE_VER` 升到 `v2`，并把叠加层 `version` 拼入 `cacheKeyFor()`。
- 本地打开（`file:` 协议）无法拉取叠加层，跳过并 toast 提示（与 `latest/` 自动加载一致）。
  **由服务器服务时**，因为键是内容键，任何加载方式的寄存器文件都能被正确补全。

## 备选方案对比

| 方案 | 说明 | 取舍 |
|---|---|---|
| **A（推荐）全局 `descriptions/` 目录 + 内容键** | 一份独立叠加层，按内容键查表 | 运行时无跨源匹配；不绑定路径；任意加载方式都能补；生成一次、diff 友好 |
| B 每文件伴随 `<basename>.descriptions.json` | 放同目录、随寄存器队列加载 | 也不错，但按文件名配对；本地拖拽无服务器路径时难配对；多文件/拆分维护麻烦 |
| C 前端解析 PDF（pdf.js） | 浏览器直接读手册 | 800k 寄存器的文件 + 整本 PDF 扫描，慢且脆；否决 |
| D 内嵌进原始寄存器文件 | 改原始文件 | 与「不改原文件」目标冲突；否决 |

推荐 **A**，可兼容 B（若目录里存在伴随文件也一并合并）。

## 优势

- 原始大文件零改动，重生成可自动化、可 diff。
- 浏览器只做字符串查表，无模糊匹配逻辑。
- "基础 + 叠加" 模式：原始数据永远是空值来源，叠加层只兜底，风险低。
- 一条命令重生成 + 覆盖率报告做 QA。
- 复用现有 `list-register-files` 与静态文件服务，**无需改动任何 C 代码**，`make` 不受影响。

## 验收

- 静态契约测试：新增 `tests/register_viewer_description_overlay_test.sh`，仿现有
  `register_viewer_*_test.sh`，grep 新增函数与关键调用如
  `autoLoadRegisterDescriptions`、`regKeyOf`、`fieldKeyOf`、`descriptionOverlay`、
  `_descSource`、`descriptions/`。
- 手工：加载 trunk 文件 → 展开 `REGFILE_CEPCS0_0_FEC_CONTROL`，字段
  `FEC_BYPASS_CORRECTION_ENABLE` 显示手册描述 + 📖 徽标；未匹配项仍为空；已有描述项不被覆盖。
- `bash tests/register_viewer_selection_test.sh` 与
  `bash tests/register_viewer_latest_autoload_test.sh` 仍通过。
- `make` 通过；`descriptions/` 由现有静态文件处理器直接供给。

## 边界与约束

- **只补空**：叠加层值不覆盖原始已有描述；原始值为准。
- **多叠加层**：按文件名排序合并，冲突时后者胜（极少发生）。
- **缓存**：叠加层重新生成必须触发前端重新解析（并入版本号/升 CACHE_VER）。
- **file:// 下**：无法拉取叠加层，跳过并提示；不影响寄存器本身解析。
- **Win/Linux**：键分隔使用 `\x1F`，不依赖路径分隔符，跨平台稳定。
