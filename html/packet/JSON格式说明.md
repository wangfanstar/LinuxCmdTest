硬件报文表项通用 JSON Schema 说明
此 JSON 结构定义了一种用于描述底层硬件报文表项（如 C/C++ 中的 struct 嵌套 union 结构）的通用数据模型。AI 或解析脚本应关注其两段式架构：基础结构（必选）与动态分支（可选）。

1. 顶层结构 (Root Object)
顶层是一个 JSON Object，包含表项的全局信息及分层数据：

packet_name (String): 报文表项的全局名称标识（例如 "CPU2CPP", "LOOPBACK"）。

base_header (Array): 必选。包含所有固定不变的字段定义。无论条件如何变化，这些字段必定存在，相当于 C 语言中的基础 struct。

conditional_header (Object): 可选。如果报文头部存在基于某个控制字段进行空间复用（Multiplexing）的情况，则存在此对象；如果报文是纯线性的（如 LOOPBACK），则此字段不出现。

2. 基础块定义：base_header
这是一个数组，按顺序描述了硬件报文头中的固定“长字 (Word)”。

word (Integer): 长字索引/偏移量（例如 0, 1, 2，通常每个字代表 32-bit 或 64-bit 空间）。

fields (Array): 该长字内包含的具体字段列表，通常按高位到低位的顺序排列。

bit_range (String): 字段在当前长字中占据的比特位范围（例如 "31~16"，单比特为 "7"，全部占用可能为 "全量" 或 "31~0"）。

name (String): 字段的程序变量名、宏定义或缩写。

description (String): 字段的功能描述、合法值或特殊动作注释。

to / from (String): 数据流向标识。

出方向报文（如下发硬件）通常使用 to，表示映射到哪个目标模块或寄存器。

入方向报文（如上送 CPU）通常使用 from，表示数据来源于哪个硬件流水线节点。

如果为 /，表示暂无映射、系统保留或默认配置。多来源时可能用 | 或逗号分隔。

3. 动态复用块定义：conditional_header
这是一个对象，行为类似于编程语言中的 switch-case 语句或 C 语言中的 union 联合体。

dependent_on_field (String): 关键依赖键。指明当前分支逻辑依赖于 base_header 中的哪个具体字段的值（例如依赖 "Message Type"）。

word (Integer): 发生内存复用的起始长字索引（例如 7 或 8）。

variants (Array): 所有的条件分支列表（即所有的 case）。

condition_value (String): 触发此分支的匹配条件（可能是具体的十六进制值如 "0x1"，也可能是业务类型名称如 "MAC 地址学习"）。

condition_desc (String): 此分支代表的业务含义。

fields (Array): 当条件命中时，当前复用长字（Word）的具体字段拆分方式。其内部数据结构与 base_header 中的 fields 完全一致（包含 bit_range, name, description, to/from）。

给自动化解析系统（AI / 脚本）的执行建议
模式识别 (Schema Validation): 解析前先检查顶层是否包含 conditional_header。如果没有，按纯线性结构体生成代码；如果有，准备生成包含 union 的复杂结构体。

方向适配 (Direction Handling): 检查 fields 内部使用的是 to 还是 from。这通常决定了生成代码是赋值操作（Setter/Tx）还是读取操作（Getter/Rx）。

依赖图链接 (Dependency Linking): 如果存在 conditional_header，必须回溯解析树，在 base_header 中找到 dependent_on_field 所指向的字段，并在生成序列化/反序列化（Pack/Unpack）逻辑时，将该字段作为 switch(field_value) 的判断条件。

---

## 文件架构关系（PacketGen离线相关）

当前离线相关文件放置在 `packet` 目录，关系如下：

- `PacketGen.html`  
  主页面与主逻辑入口。运行时会动态挂载 manifest，并注册 service worker。

- `packet/manifest.webmanifest`  
  Web App Manifest 文件，描述应用名称、主题色、启动页与作用域。

- `packet/sw.js`  
  Service Worker 缓存策略与离线回退逻辑。

- `packet/*.json`（如 `CPU2CPP_HDR.json`、`CPP2CPU_HDR.json`、`LOOPBACK.json`）  
  自定义报文头定义文件，被页面和离线缓存共同使用。

### 关系说明

1. 页面加载（HTTP/HTTPS）时，`PacketGen.html` 动态引用 `packet/manifest.webmanifest`。  
2. 页面随后注册 `packet/sw.js`，由 SW 负责缓存 `PacketGen.html` 与 `packet/*.json`。  
3. 解析/生成逻辑使用 `packet/*.json` 的 schema 结构驱动字段显示、编码与解码。

---

## 工作原理（离线能力）

`packet/sw.js` 使用 Cache API：

- `install` 阶段预缓存核心资源（页面、manifest、JSON定义）。  
- `activate` 阶段清理旧版本缓存。  
- `fetch` 阶段优先命中缓存，未命中再走网络；网络失败时回退到 `PacketGen.html`。

这样可在已缓存后支持断网访问与离线解析/构包。

---

## 使用说明

### 1) 推荐启动方式

不要直接双击 `file://` 打开。请用本地 HTTP 服务启动，例如：

```bash
npx http-server "E:\MCP_PROJECT\PacketGen" -p 8000
```

然后浏览器访问：

`http://localhost:8000/PacketGen.html`

### 2) 首次离线准备

1. 联网打开一次页面。  
2. 等页面与资源加载完成（SW完成安装与缓存）。  
3. 之后断网仍可打开并使用已缓存的模板与定义。

### 3) 更新资源后的建议

当 `packet/*.json` 或页面逻辑更新后，建议：

1. 刷新页面（可使用强制刷新）。  
2. 重新打开一次页面以完成新版本缓存。  
3. 若出现旧缓存内容，关闭标签页后重开页面。