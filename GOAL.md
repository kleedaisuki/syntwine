# Syntwine｜核心 API 标准与实现方案

## 1. 规范目标与不可变更的技术决策

本系统是一套 **C++23 原生实现、pybind11 暴露、Python 3.15+ 使用的类型化文档数据流 API**。Python 提供自然的声明式接口、Typing 与用户控制流；**源节点、表达式图、动态求值器、原生算子、Document Stream、预算校验和遥测引擎均由 C++23 实现**。只有调用 `generate()` / `write()` 才执行完整构建。

```
Python 3.15+ : Typed API, user control-flow, custom callbacks
                  │ pybind11
C++23           : Source / Typed Fragments / Immutable Graph
                  │
                  ▼
       Dynamic EvaluationSession (native)
                  │
            Format Operations
                  │
         Document Part Composition
                  │
       Document Post Stream (squish, pipe, ...)
                  │
        Serialize → Validate → Hard Budget
                  │
             Generated Artifact
                  └─ Native end-to-end telemetry (OFF by default)
```

规范关键词：**MUST**（强制）、**SHOULD**（常规要求）、**MAY**（可选）。

1. 所有公开内容类型 **MUST** 继承 `Source`，不公开独立的 `File` 类型。
2. `Literal` **MUST** 是 `bytes`；`Text` **MUST** 是 UTF-8 `str`；XML、TOML、JSON、MD 各自具有原生操作与显式类型重解释。
3. 所有中间 API **MUST** 惰性；构图不执行读文件、解析、用户回调或内容生成。
4. `Document` **MUST** 先求值所有被选择的 Parts，完成序列化与有序拼接，再执行自己的整篇 Post Stream。
5. 图 **MUST** 支持运行时新增依赖：`Source.defer`、`bind`、嵌套 `Document` 及 Source 作为参数均属于同一 Session 的动态图。
6. Python `if` / `for` / 函数承担控制流；不引入独立 Slot/Conditional DSL。
7. 用户算子 **MUST** 可通过 `pipe` 与 `ops.mount` 在 Python 挂载；实际执行由 C++23 求值器调度，在回调节点跨 pybind11 调用 Python。
8. 最终字符、字节、Token 硬预算 **MUST** 精确校验；无隐式截断与静默删减。
9. **C++23 + pybind11 + Python 3.15+ 是产品正式技术栈，不是未来优化项**。编译、绑定、测试、分发均以此为准，不设计“先 Python、后 C++”路线。
10. **端到端遥测为核心功能，默认关闭**；开启/配置/局部覆盖均由 Python API 完成，C++ 内核直接埋点并记录跨 Python 回调的节点。

## 2. 值语义与继承结构

```
Source[T]                         # 一切可惰性求值的内容节点
├── Literal : Source[bytes]       # 二进制字符串
├── Text    : Source[str]         # UTF-8 字符串
├── MD      : Source[MarkdownValue]
├── TOML    : Source[TOMLValue]
├── JSON    : Source[JSONValue]
├── XML     : Source[XMLValue]
└── Document: Source[bytes]       # 有序组合 + 整篇后处理
```

**`Source[T]` 是一个带输出类型的惰性节点。** `T` 指求值结果的数据模型，不是源文件的后缀名。`Source` 可以是文件、计算、组合、动态绑定或另一个 `Document`。

| 类型 | 求值后的值 | 编码 / 格式保证 |
| --- | --- | --- |
| `Source[bytes]` | 原始字节 | 无格式承诺 |
| `Literal` | `bytes` | 保留每一个字节，不做隐式转义 |
| `Text` | `str` | 必须可编码为合法 UTF-8；严格解码 |
| `MD` | Markdown 表示 | 原始源文本保留，结构性操作按 Markdown 语义解释 |
| `TOML` | TOML 数据与源表示 | 解析有效；结构操作保留可保留的注释与布局 |
| `JSON` | JSON 数据与源表示 | 按 JSON 语法校验；显式序列化 |
| `XML` | XML 文档或片段 | XML 语法、树结构和属性操作 |
| `Document` | 拼接并后处理后的 `bytes` | 可以表示纯文本或二进制产物；`format` 决定检查和处理策略 |

**转换规则**：任何 `Source` 都具有 `as_literal()`、`as_text()`、`as_md()`、`as_toml()`、`as_json()`、`as_xml()`。重解释步骤定义为：

```
source (typed value) → serialize to bytes → decode / parse as target type
```

所以 `TOML.as_json()` 是把 TOML 序列化后的文本当 JSON 语法重新解析，**不是**把 TOML 数据结构自动转成等价 JSON 对象。真正的语义数据转换采用 `to_json()` 等独立方法。跨格式重新解释是**允许的显式操作**，解析失败则抛错；不默认推荐在业务逻辑里反复绕格式转换。

`Literal(b"<x/>")` 只是 4 字节的字面数据。需要 XML 解析时须写 `.as_xml()`；需要构造 XML 元素则由 `XML.element(...)` 完成，不在 `Literal` 上提供隐含 XML 包装。

## 3. 核心 API（对外契约）

### 3.1 构造 Source

```python
from syntwine import Source, Literal, Text, MD, TOML, JSON, XML, Document

raw = Source("prompts/base.md")                 # 默认：文件路径，惰性读取 bytes
raw2 = Source.path("assets/prompt.bin")         # 显式路径工厂，与上式等价
blob = Literal(b"\x00\xFFhello")                # 真实 bytes
message = Text("Hello, 世界")                    # UTF-8 文本
markdown = MD("# Instructions\n\nHello!")       # 源文内联
json_doc = JSON.from_value({"enabled": True})   # 结构化常量
toml_doc = TOML.from_value({"agent": {"mode": "safe"}})
xml_doc = XML.element("note", Text("hello"))    # 明确构造 XML 节点

computed = Source.defer(lambda ctx: Text(ctx["project_name"]))
```

`Source(path)` 的字符串参数永远解释为路径，而不解释为内容。内联文本必须用 `Text` / `MD` 等；内联 bytes 必须用 `Literal`。路径读取与 `Source.defer` 中的回调都延迟到构建期。

### 3.2 Source 通用操作

| API | 行为 | 求值时间 |
| --- | --- | --- |
| `.as_literal() -> Literal` | 序列化为 bytes；无格式解析 | generate |
| `.as_text(encoding="utf-8") -> Text` | 按指定编码严格解码 | generate |
| `.as_md() / .as_toml() / .as_json() / .as_xml()` | 重新解释并校验 | generate |
| `.pipe(op, *args, **kwargs)` | 单节点类型化操作 | generate |
| `.map(fn)` | 将一个求值结果映射为另一个普通值 | generate |
| `.bind(fn)` | 函数返回新 Source，运行时发现下游依赖 | generate |
| `.zip(other)` | 汇合两个依赖，产生值对 | generate |
| `.combine(*others, fn=...)` | 多源 join 并求值函数 | generate |
| `.require(predicate, message=...)` | 对求值结果施加断言 | generate |
| `.tap(fn)` | 在求值时观察值，有副作用 | generate |
| `.generate(...)` | 终止求值，返回 `Generated[T]` | 立即 |
| `.write(path, ...)` | 终止求值、完成验证后写文件 | 立即 |

`Source.defer` 和 `bind` 是动态图机制，而不是重新实现一门条件语言。在构造时已知的条件继续使用 Python `if` 和 `for`。

### 3.3 Literal — 二进制原生操作

```python
payload = (
    Literal(b"\x00header\xff")
    .slice(1, 7)
    .concat(Literal(b"\nend"))
    .replace_bytes(b"head", b"HEAD")
)

encoded = payload.base64()             # Text
hex_text = payload.hex()               # Text
decoded = Literal(b"hello").as_text()  # Text, 默认 UTF-8 strict
digest = payload.sha256()              # Text（hex）
```

规范方法：`slice`、`concat`、`replace_bytes`、`starts_with`、`ends_with`、`base64`、`hex`、`sha256`、`as_text`。不包含 `wrap_xml` 或任何隐式 XML 语义。错误的 UTF-8 必须在转换时抛出 `DecodeError`，不可静默替换。

### 3.4 Text — UTF-8 字符串原生操作

```python
name = Text("klee")
rule = (
    Text("  Hello, {who}!  ")
    .format(who=name)
    .strip()
    .replace("Hello", "Welcome")
    .indent(2)
)

bytes_value = rule.as_literal()  # 在 generate 时 UTF-8 编码
```

规范方法：`concat`、`join`、`strip`、`lstrip`、`rstrip`、`replace`、`split`、`slice`、`indent`、`dedent`、`normalize_newlines`、`format`、`encode`。`format` 接收普通值或 Source；Source 参数作为依赖求值后替换，不需要 Slot 对象。

### 3.5 MD — Markdown 原生操作

```python
rules = (
    Source("rules.md").as_md()
    .section("Coding")
    .shift_headings(1)
    .append(MD("## Additional rules\n\nUse tests."))
)

code = Source("config.toml").as_toml().codeblock("toml") # MD
```

规范方法：`section(heading, occurrence=0)`、`sections()`、`headings()`、`shift_headings(levels)`、`insert_before`、`insert_after`、`append`、`prepend`、`replace_section`、`remove_section`、`codeblock(language)`、`links()`、`render() -> Text`。

Markdown 源文本与代码围栏中的原始内容必须保持格式；涉及章节/标题时解析 Markdown 结构。不进行全局“随便合并空白”的字符串优化。

### 3.6 JSON — 结构化原生操作

```python
config = (
    Source("config.json").as_json()
    .get("features")
    .set("enabled", True)
    .merge(JSON.from_value({"limit": 12}), conflict="right")
)

picked = Source("data.json").as_json().select("$.items[*].name")
pretty = config.pretty(indent=2)  # JSON
as_text = pretty.as_text()        # 序列化后的 JSON 字符串
```

规范方法：`get`、`set`、`remove`、`merge`、`select`、`filter`、`append_array`、`sort_keys`、`pretty`、`minify`、`to_toml`。`merge` 显式规定冲突优先级；数组默认**整体替换**，不做无声明的智能合并。`get` 返回相应的类型化值或 Source，而不是把 JSON 节点随意当成文本。

### 3.7 TOML — 配置原生操作

```python
config = (
    Source("base.toml").as_toml()
    .merge(Source("override.toml").as_toml(), conflict="right")
    .table("agent")
    .set("max_steps", 24)
    .remove("deprecated")
)

text = config.as_text()
```

规范方法：`table`、`get`、`set`、`remove`、`rename`、`merge`、`keys`、`arrays`、`to_json`、`as_text`。`table("agent")` 返回表视图，修改该表生成一个新的、带相同配置根语境的 TOML 表达式；在文档根输出时可使用 `.root()` 明确返回完整文档。

**TOML 不存在原生二进制值。** `TOML.set(key, Literal/Source[bytes])` 的标准绑定规则是把 bytes 按 UTF-8 `strict` 解码为 TOML String；非法 UTF-8 报 `EncodingError`。如需保存任意二进制，请显式 `.base64()` 或 `.hex()` 再传入。该语义使“Doc.squish().as_literal() 注入 TOML”可以直接构造动态依赖，又不会悄悄丢字节。

### 3.8 XML — XML 原生操作

```python
tree = (
    Source("fragment.xml").as_xml()
    .xpath("//instruction")
    .set_attr("priority", "high")
    .wrap("section")
)

element = XML.element("source", Text("<text is escaped>"))
compact = tree.minify_whitespace(policy="markup_only")
```

规范方法：`xpath`、`find`、`children`、`text`、`set_text`、`set_attr`、`remove_attr`、`append`、`prepend`、`replace`、`remove`、`wrap`、`unwrap`、`minify_whitespace`、`serialize`。`XML.element` 负责正确转义文本，不通过 `Literal` 偷渡 XML 语义。

## 4. Document = 可整体流处理的复合 Source

### 4.1 数据模型

`Document` 是 `Source[bytes]`，内部包含两个有序维度：

- **Parts**：有序子源/Fragment 序列，每个子项可以是另一个 Document。
- **Post Pipeline**：整个 Document 拼接后的操作序列，按声明顺序执行。

```
[Part 1] → parse/transform ┐
[Part 2] → parse/transform ├─ Serialize + Join ── Post[0] ── Post[1] ── ... ── Output
[Part 3] → parse/transform ┘                      squish     replace
```

**执行屏障（Barrier）**：所有需要参与当前输出的 Part 都完成必要的求值、序列化和有序拼接后，**才允许**任何 Document 级 `squish` / `replace` / `pipe` 执行。此规则保证跨 Fragment 边界的文本压缩和统一格式化是可能的。

Document 是**不可变值对象**，每次 `append` / `squish` 都返回新 Document，允许共享旧版本。`doc += frag` 定义为 `doc = doc + frag`（Python 重绑定），不是原地修改旧对象。

### 4.2 组合操作

| API | 语义 |
| --- | --- |
| `Document(format="text", separator="\n\n")` | 构造空 Document，文本默认采用空行分隔 |
| `.append(*parts) / .extend(iterable)` | 尾部加入片段 |
| `.prepend(*parts)` | 头部加入片段 |
| `.insert(index, part)` | 指定序号插入 |
| `.remove(index) / .replace_part(index, part)` | 操作 Parts，不触碰源内容 |
| `.slice_parts(start, stop)` | 选取部分 Parts |
| `.concat(other, separator=...)` | 组合另一个 Document |
| `.join(separator=...)` | 指定拼接策略 |
| `.map_parts(fn) / .filter_parts(pred)` | 拼接前逐片段处理或筛选 |
|   `• / +=` | `append` 的便捷形式 |

对 `format="raw"` 默认分隔符为 `b""`；对 `format="text"` 或 `format="md"` 默认为两个换行。所有片段最后均以字节序列拼接；Text 必须经过 UTF-8 编码，结构化类型先使用其确定的序列化结果。

### 4.3 Document 整篇 Stream 操作

| API | 作用阶段 |
| --- | --- |
| `.squish(mode="auto")` | 拼接完成后执行格式感知压缩 |
| `.replace(old, new)` | 整篇替换，允许匹配跨片段边界 |
| `.strip() / .indent(n) / .dedent()` | 整篇空白/缩进变换 |
| `.normalize_newlines(style="lf")` | 整篇换行标准化 |
| `.wrap(prefix, suffix)` | 整篇包装 |
| `.ensure_prefix(prefix) / .ensure_suffix(suffix)` | 整篇幂等边界处理 |
| `.pipe(op, ...)` | 整篇自定义操作 |
| `.require(predicate, message)` | 整篇断言 |
| `.budget(Budget(...))` | 声明生成约束；在全部 Post 执行后校验 |
| `.as_literal() / .as_text() / .as_md()` | 建立对最终 Document 输出的类型化依赖 |

**关键规则**：即使写成 `doc.squish().append(extra)`，`append` 加入的仍是 Parts；`squish` 仍对最终全部 Parts 生效。所有结构编辑先完成，随后统一执行 Post Pipeline。改变 Post 之间的顺序会改变结果，例如 `replace(...).squish()` 与 `squish().replace(...)` 不保证等价。

`squish(mode="auto")` 根据 `format` 选择明确的格式策略。`mode="markdown"` 保留代码围栏、列表和块级结构；`mode="xml"` 遵守 XML 的文本节点与空白规则；`mode="text"` 使用文档指定的文本空白策略。压缩不能以“更少 Token”为由破坏格式或任意截断。对于没有足够信息判断空白可删除的情形，`squish` 保留原文。可另以显式策略进行激进压缩。

### 4.4 全文后处理示例

```python
from syntwine import Document, Source, Text, Budget

doc = (
    Document(format="md")
    .append(Text("# Instructions"))
    .append(Source("rules/core.md").as_md())
    .append(Source("rules/csharp.md").as_md())
    .squish(mode="markdown")       # 全部 Part 拼完以后运行
    .normalize_newlines("lf")
    .ensure_suffix("\n")
    .budget(Budget(characters=30_000, tokens=8_000))
)

artifact = doc.generate(tokenizer="o200k_base")
print(artifact.text)
```

## 5. 动态依赖图（核心执行语义）

### 5.1 形式模型

令 `V` 为 Source 和 Operation 节点集合，`E_t` 为在时刻 `t` 已发现的依赖边，`C` 为本次构建上下文。

```
G_t = (V_t, E_t)
E_t ⊆ E_(t+1)
Eval(node, C) -> Value | Error
```

构建开始时只有显式引用形成的静态边。`Source.defer`、`bind` 等节点在求值时允许**返回新的 Source**，从而向当前执行图注册新的节点与依赖边。求值器不要求在开始前枚举全部依赖。

- `pipe(fn)`：`fn` 输入已经求值的值并返回**普通值**；属于普通变换。
- `map(fn)`：同上，可改变结果类型。
- `bind(fn)`：`fn` 输入已经求值的值并返回 `Source[U]`；运行时将新的 Source 注册为依赖并继续求值。
- `Source.defer(fn)`：求值时调用 Python 工厂，返回值或 Source；可根据上下文动态选择文件和表达式。
- `zip / combine`：多源汇合；不要求手写占位符或插槽。
- `TOML.set / Text.format / XML.set_text`：参数如果是 Source，在调用时**只记录依赖**，生成时再求值并插入。

### 5.2 动态文件选择

```python
from syntwine import Source

rule = (
    Source("manifest.json").as_json()
    .get_text("entry")                    # Source[str]
    .bind(lambda filename:
          Source(f"prompts/{filename}").as_md())
)

result = rule.generate()
```

只有求值 `manifest.json` 后才能知道需要读取哪个 Markdown 文件。这是运行时解析图，而不是提前展开所有潜在分支。

### 5.3 文档嵌入 TOML：跨源、跨格式的动态图

```python
from syntwine import Source, Text, Document, TOML, Budget

instructions = (
    Document(format="md")
    .append(Source("prompts/base.md").as_md())
    .append(Source("prompts/security.md").as_md())
    .append(Text("## Project\n\nUse unit tests."))
    .squish(mode="markdown")
    .ensure_suffix("\n")
)

# Literal 是 bytes；此处是依赖边，不是提前调用 generate。
binary_prompt = instructions.as_literal()

config = (
    Source("settings.toml").as_toml()
    .set("agent.instructions", binary_prompt)
    .set("agent.enabled", True)
)

result = config.generate(budget=Budget(bytes=100_000))
print(result.text)
```

求值顺序：先生成 `instructions` 所需各文件与 Markdown Fragment → 整体 `squish` → `as_literal` 获得 UTF-8 bytes → TOML `set` 在键值边界以 UTF-8 strict 解释为 String → 生成合法 TOML → 对最终 TOML 产物实施预算检查。用户不必在中间手工 `generate()`。

### 5.4 图的执行规定

- 每次 `generate()` 在 C++23 中建立新的 `EvaluationSession`；缓存、依赖、错误与 Telemetry TraceContext 属于该 Session；pybind11 仅提供 Python 入口。
- 节点状态为 `UNSEEN / VISITING / DONE / FAILED`；访问到 `VISITING` 节点意味着循环，抛出 `CycleError` 并列出完整依赖链。
- 一个 Session 内，标记为纯且没有动态副作用的共享节点最多求值一次；同一 `Source` 可在不同 Document 里复用。
- `bind` 新发现的依赖必须参加相同的循环检测、错误定位和缓存。
- 不保证单独 `generate()` 的结果在跨次运行时不变：文件与外部上下文可能发生变化；每次运行重新建立快照。
- 求值器对任意长度的依赖链设置可配置的节点数 / 深度上限，防止无限动态展开；这不等价于字符或 Token 预算。
- 有序 Document 的输出顺序由 Parts 决定；调度并行不允许改变最终拼接次序。
- 不允许在 `pipe` 回调内部偷偷调用另一个 `generate()` 以绕开当前图；依赖应以 `Source` 和 `bind` 表示。

## 6. 自定义操作与挂载协议

### 6.1 类型化操作（首要稳定接口）

```python
from syntwine import Text, Source, operation

@operation(input=Text, output=Text, pure=True, version="1")
def normalize_rules(value: str) -> str:
    return "\n".join(line.rstrip() for line in value.splitlines())

fragment = Source("rules.txt").as_text().pipe(normalize_rules)
```

`@operation` 将 Python 用户算子注册到 C++23 原生 Operation Registry，定义输入/输出类型、操作名称、版本、纯度以及可选配置。C++23 Session 调用该节点时由 pybind11 进入 Python 回调。`pipe` 只加入操作节点，不触发函数调用。用户函数在求值阶段收到**已解析的底层值**，而不是惰性容器对象。

### 6.2 命名挂载

```python
from syntwine import ops, Text

ops.mount(Text, "normalize_rules", normalize_rules)

# 动态便利调用；运行期可用。
x = Source("rules.txt").as_text().normalize_rules()

# 静态类型友好写法，始终可用。
y = Source("rules.txt").as_text().pipe(normalize_rules)

# 不依赖动态属性的命名调用。
z = Source("rules.txt").as_text().op("normalize_rules")
```

挂载操作按输入类型注册，冲突时默认抛 `OperationConflict`；可以显式指定作用域/覆盖策略。`pipe` 的静态类型签名通过 `Callable[[T], U]` / `Protocol` 推断 `Source[U]`；动态注入的 `.normalize_rules()` 不对 Pyright/mypy 承诺自动补全，静态扩展通过 `Protocol` / 显式子类或 `.pyi` 实现。

### 6.3 Document 整篇自定义算子

```python
from syntwine import Document, Text, operation

@operation(input=bytes, output=bytes, pure=True)
def add_banner(payload: bytes) -> bytes:
    return b"# Generated\n\n" + payload

doc = (
    Document(format="md")
    .append(Text("Hello."))
    .pipe(add_banner)  # 放进 Document 的 Post Pipeline；拼接后执行
    .squish(mode="markdown")
)
```

Document `pipe` 的默认输入/输出值为 bytes，适用于整篇后处理；若需要按结构操作，先通过 `.as_md()` 等建立目标类型表达式。算子可以声明 `pure=False`，此时禁止缓存并承认副作用；`tap` 默认视为副作用节点。

### 6.4 扩展数据类型

第三方可以通过 pybind11 扩展接口及 Python 类型壳定义 `class YAML(Source[YAMLValue])` 等新内容类型，并注册解析器、序列化器、格式原生算子。`Source` 的求值、依赖、预算及诊断机制无需因此改动。自定义格式必须声明其 `parse(bytes)`、`serialize(value) -> bytes` 和可选验证函数。

## 7. generate、预算与产物

### 7.1 终止 API

```python
from syntwine import Budget, BuildOptions

artifact = doc.generate(
    budget=Budget(characters=28_000, bytes=100_000, tokens=7_000),
    tokenizer="o200k_base",
    options=BuildOptions(cache="session"),
)

artifact.bytes                 # 最终 bytes
artifact.text                  # UTF-8 严格解码；非法字节会抛错
artifact.value                 # 根 Source 对应的类型化值
artifact.usage.characters
artifact.usage.bytes
artifact.usage.tokens
artifact.sources               # 本次执行实际使用到的源
artifact.trace                 # 遥测关闭时为 None；开启后为 span 引用 / trace 视图

doc.write("AGENTS.md", budget=Budget(characters=28_000))
```

`generate()` 通过 pybind11 进入 C++23 的 `EvaluationSession`，完成全部依赖解析、算子求值、Document Post Stream、序列化、预算与返回；Python 不维护第二套构建器。`write()` 先完整生成与校验，再采用临时文件及同文件系统替换，失败时不得留下不完整产物。

### 7.2 硬预算

最终输出为字节串 `b`；如可严格 UTF-8 解码，`s = UTF8Decode(b)`。指定兼容的 Tokenizer `T_m`，则：

```
len(b) <= B_bytes
len(s) <= B_characters
len(T_m(s)) <= B_tokens
```

- `characters` 的单位是 Unicode 码点（与 Python `len(str)` 一致），不是 UTF-8 字节数或 grapheme cluster。
- 二进制内容可只限制 `bytes`；要求字符/Token 预算时必须能严格解码，否则报错。
- Tokenizer 编码必须明确，不能用字符数估计精确 Token 数。
- 只在**Document 全部 Post Stream 完成以后**针对**最终真实输出**计数，计入跨片段分隔符、XML/JSON/TOML 转义及代码围栏。片段 Token 数不得简单求和。
- 超限抛出 `BudgetExceeded`（实际值、预算、超额、追溯来源），保留本次 Session 的失败遥测（若启用）；不隐式截断或删除片段。

## 8. 端到端遥测（Telemetry）与诊断规范

### 8.1 启用模型：默认 OFF，Python 可配置

遥测是完整核心功能，不是外置脚本、实验特性或以后追加的能力。**默认状态 `enabled=False`**：不自动收集、不写盘、不向网络发送；C++ 原生埋点在关闭时采用 No-op 快速路径。开启后，对一次 `generate()` 的所有步骤产生关联的 Traces、Metrics、Events。

```python
from syntwine import telemetry
from syntwine.telemetry import TelemetryConfig, JSONLSink, SQLiteSink, OTLPSink

telemetry.configure(
    TelemetryConfig(
        enabled=True,
        traces=True,
        metrics=True,
        events=True,
        level="operations",          # build | stages | operations
        sample_rate=1.0,
        include_content=False,       # 默认仅记录元数据/大小/标识
        sink=JSONLSink(".syntwine/telemetry.jsonl"),
    )
)

# 临时覆盖，不改变其他任务或全局默认值。
with telemetry.use(TelemetryConfig(
    enabled=True, sink=SQLiteSink(".syntwine/telemetry.db")
)):
    artifact = doc.generate()

# 单次构建优先级最高。
artifact = doc.generate(
    telemetry=TelemetryConfig(
        enabled=True,
        sink=OTLPSink(endpoint="http://localhost:4318"),
    )
)

telemetry.flush()
telemetry.configure(TelemetryConfig(enabled=False))
```

配置优先级：**本次 `generate(telemetry=...)` > 上下文 `telemetry.use(...)` > 全局 `telemetry.configure(...)` > 默认关闭**。上下文必须具有线程/任务隔离语义。设置支持在 Python 中完成；不得要求用户修改 C++ 配置文件或编译开关。

### 8.2 全链路 Span 结构

```
generate [trace_id, build_id]
├── graph.resolve
│   ├── source.read / source.decode
│   └── dynamic.bind / dependency.discover
├── node.evaluate (every selected Source / Operation)
│   ├── parse.md | parse.toml | parse.json | parse.xml
│   ├── transform / merge / custom.native
│   └── custom.python (pybind11 callback)
├── document.compose
│   ├── fragment.serialize
│   └── parts.join
├── document.post
│   ├── squish
│   └── pipe / replace / newline.normalize / ...
├── output.serialize / output.validate
├── budget.characters / budget.bytes / budget.tokens
└── write (only for write() terminal call)
```

每个 Span / Event 记录：`trace_id`、`span_id`、`parent_span_id`、`build_id`、`node_id`、`source_id`、`operation`、`type`、`status`、`start_ns`、`duration_ns`、`input_bytes`、`output_bytes`，可用时记录字符数、Token 数、缓存命中、发现依赖数与错误码。`source_id` / `operation` 与 `CycleError`、`BudgetExceeded`、`OperationError` 的诊断链使用同一套标识，便于从异常直达耗时节点。

- `graph.resolve` 必须记录求值时才发现的节点与依赖边；不能仅序列化声明期图。
- 用户 Python 算子必须显示为 `custom.python` Span，计时跨越 C++ → Python → C++。
- Document 整篇 `squish` 与分片处理必须可区分；预算检查也必须单独计时。
- 构建失败也必须记录完成/异常状态，并可在用户显式 `flush()` 时落盘。
- 遥测时钟使用单调时间计算持续时间；会话关联不依赖文件名或时间戳猜测。

### 8.3 Sink、生命周期与开销

| Sink | 用途 | 规范 |
| --- | --- | --- |
| `JSONLSink` | 本地分析、对照基准、调试 | JSON Lines 增量写入 |
| `SQLiteSink` | 多次构建、追踪索引、性能回归 | 结构化存储 builds / spans / events / metrics |
| `OTLPSink` | 与 OpenTelemetry Collector 接轨 | 显式配置时输出 Trace / Metrics |
| `MemorySink` | 单元测试、程序内查询 | 有界缓冲并可提取结果 |

所有 Sink 都由 C++23 核心接管事件、排队与 flush；Python 只配置与查询。缓存、输出字节、节点计数、失败数、构建耗时、原生算子耗时、Python 回调耗时、Token 耗时均可作为 Metrics。高频事件队列必须有上限；发生丢弃时记录 dropped_events 计数。`include_content=False` 时不记录提示词正文和二进制缓冲区，仅保留操作元数据；启用内容采集必须显式配置。

**重要**：遥测开关不改变构建结果，用户启用/关闭遥测的唯一差别是可观测数据及其运行时开销。

### 8.4 错误与静态类型

| 错误 | 触发情形 |
| --- | --- |
| `SourceError` | 读取外部来源失败 |
| `DecodeError` | bytes 严格解码失败 |
| `ParseError` | MD / XML / JSON / TOML 解析失败 |
| `TypeMismatch` | 算子或嵌套值类型错误 |
| `CycleError` | 静态或动态依赖循环 |
| `OperationError` | Python/C++ 操作失败、返回类型不匹配 |
| `OperationConflict` | 扩展算子重名 |
| `BudgetExceeded` | 最终硬预算超限 |
| `SerializationError` | 目标格式非法 |

错误携带 `source_id`、`operation`、`dependency_path`、`location`（可获得时）、`cause` 和可关联的 `trace_id`（若启用遥测）。`inspect()` / `explain()` 可用于查看声明期结构；启用遥测后的 `Generated.trace` 包含本次运行实际拓展的动态图。Python 静态类型标注提供 IDE 体验，但不替代 C++ 的运行时格式校验与动态依赖验证。

## 9. C++23 + pybind11 + Python 3.15+ 原生架构

### 9.1 强制技术栈

| 层面 | 标准 |
| --- | --- |
| 核心语言 | **ISO C++23**，不是 Rust，也不是纯 Python |
| Python 最低版本 | **CPython 3.15**（`requires-python = ">=3.15"`） |
| 绑定 | **pybind11**（固定项目依赖，直接绑定核心类型） |
| 构建 | **CMake + scikit-build-core** |
| 发布 | Windows / Linux / macOS 的 Python binary wheels + sdist |
| 类型系统 | Python 3.15+ 类型标注 + `py.typed` / `.pyi`；绑定的实现对象由 C++ 承载 |
| 遥测 | C++23 内建跨度/事件/指标埋点，Python 配置；OTLP 与本地 Sink |
| 依赖图 / 调度 | C++23 原生动态图调度器，不存在 Python 解释器版本的备用 Engine |

### 9.2 模块边界

```
syntwine/__init__.py, syntwine/typing.pyi, syntwine/telemetry.py
    └── Python 3.15+ API / user functions / type hints
         │
         ▼
   _syntwine_native (pybind11)
         │  bindings / type adapters / exception mapping
         ▼
C++23 core
 ├── Source<T>, Literal, Text, MD, TOML, JSON, XML, Document
 ├── Node / Operation / DependencyGraph / NodeId
 ├── EvaluationSession / memoization / cycle detection / dynamic bind
 ├── Parsers + Format-native Ops + Serialization
 ├── Document Parts + Full-document Post Stream
 ├── Budget + Tokenizer + Generated Artifact
 └── Telemetry: spans / metrics / events / sinks / trace context
         │
         └── Python callbacks when an operation explicitly uses user code
```

**Source、Fragment、Document、Operation 和 Evaluator 都由 C++23 持有权威状态。** Python 对象通过 pybind11 暴露对应绑定并提供 Python 类型标注，不在 Python 中实现一套同名表达式图。Python 扩展操作可以返回普通值或新的 Source；C++ 会在当前 EvaluationSession 中追踪依赖并继续求值。

### 9.3 原生数据模型与调度规定

- 图节点为不可变对象；使用共享持有的节点引用（例如 `std::shared_ptr<const Node>`），保障跨 Document 的表达式复用。
- 结果值使用 C++23 的类型化表示；二进制是字节缓冲，文本是已验证 UTF-8，结构化解析值拥有受控生命周期，错误可携带结构化诊断（可使用 `std::expected`）。
- `EvaluationSession` 是 C++23 状态机：运行时追加依赖、缓存纯节点、检测循环、保持 Document Part 顺序，并在 Python 自定义操作执行前后恢复图的正确关联。
- 纯 C++ 计算阶段不需要长期持有 Python GIL；调用 Python 自定义算子或操作 Python 对象时必须获取适当的 Python 运行时锁 / 线程状态。pybind11 接口负责生命周期、异常映射与跨语言边界。
- Python 的 `@operation` / `ops.mount` 注册到原生 Operation Registry，调用时不绕过预算、遥测和循环检测。
- Native Stream Operands 必须允许 Python 用户代码传入 `Source`、`Literal`、`Document` 等作为动态值，不能先转换成已生成的字符串才能使用。
- 构建失败的 C++ 错误必须映射为第 8 章统一 Python 异常类型；不得在跨语言边界丢失源位置、依赖链和 trace ID。
- 编码与格式转化是有明确定义的 C++ 操作；跨语言 `bytes` / `str` 转换严格保留二进制和 UTF-8 语义。

### 9.4 工程打包与维护要求

```toml
[project]
requires-python = ">=3.15"

[build-system]
requires = ["scikit-build-core", "pybind11"]
build-backend = "scikit_build_core.build"
```

CMake 设置 `CMAKE_CXX_STANDARD 23`，编译 C++23 核心库与 `_syntwine_native` 扩展。PyPI 分发包含已编译 wheel、`py.typed`、类型存根、公开 API 文档；Windows/Linux/macOS CI 进行原生单元测试、Python 端到端测试、跨语言回调测试及 wheel 安装测试。`pybind11` 的 ABI 策略按目标 CPython 3.15+ 的可用模式构建并验证，包括普通与 free-threaded 解释器（当分发支持该模式时）。不额外维护纯 Python 后备引擎。

端到端遥测埋点写入 C++23 核心的真实执行路径，不能仅通过 Python 装饰器包围 `generate()` 伪装成内部 Trace。原生 JSONL/SQLite/OTLP Sink 与 Python 配置必须共享一个 Session/TraceContext。

## 10. 一体化实现范围与交付验收

**本章是单一完整交付合同，不拆 Phase A/B/C/D/E，不定义“以后再移植 C++”或“遥测后续再加”。**

### 10.1 必须一次性具备的产品能力

- C++23 原生 `Source[T]`、`Literal`、`Text`、`MD`、`TOML`、`JSON`、`XML`、`Document` 的状态、表达式构建、生命周期与求值实现；Python 3.15+ 通过 pybind11 使用所有公开类型。
- 文件源、字面源、惰性 `as_*` 格式重解释、各类型原生操作与结构化转换。
- Document Parts、有序拼接、整篇 Stream 算子、`squish` 与格式验证；`Document` 可嵌套。
- 动态 `bind` / `Source.defer` / `zip` / `combine`，支持 Source 依赖注入 TOML/JSON/MD/XML/Text，包括 `Document.squish().as_literal()`。
- Python 用户操作 `@operation` / `pipe` / `ops.mount`，在 native 图中登记并于生成时执行。
- 生成/写入、精确字符/字节/Token 硬预算、结构化诊断、成功与失败产物的统一模型。
- C++ 原生端到端遥测：默认关闭，可由 Python 全局配置、上下文覆盖、单次生成覆盖；本地 JSONL/SQLite/Memory + OTLP Sink，跨 C++ / Python 操作的完整 Trace。
- CMake、pybind11、scikit-build-core 打包与跨平台编译发布；Python 最低版本 3.15，带类型存根与 Python/C++ 单元及端到端测试。

### 10.2 验收矩阵

| 端到端用例 | 必须通过的结果 |
| --- | --- |
| 构图惰性 | 调用任何中间操作不读取文件、不执行自定义算子、不生成文本 |
| 类型语义 | `Literal` 精确保留任意 bytes；`Text` 强制 UTF-8；重解释合法/非法行为确定 |
| 统一 native 内核 | Source/Document/动态图/预算实际在 C++23 执行；Python 仅绑定与用户回调 |
| 重复生成 | 同一图可再次 `generate()`；新 Session 读取更新源；单次复用共享节点 |
| 全文屏障 | Document Parts 全部合并之后运行 `squish` 与整篇 `pipe` |
| 动态依赖 | `bind` 可从 manifest 找出新路径并加入原生执行图 |
| Document 嵌入 TOML | 无中间生成调用，即可将 `doc.squish().as_literal()` 注入 TOML String |
| 错误定位 | 静态/动态循环、解析失败、算子异常均提供完整依赖路径和节点定位 |
| 跨语言回调 | Python 算子由 C++ Session 执行并返回正确类型，异常保真 |
| 硬预算 | 根据最终真实 bytes/字符/Token 验证；超限失败且 `write` 不写残缺文件 |
| 遥测默认关闭 | 无主动写盘/联网，没有 Span 对象的高频分配或内容采集 |
| 遥测全链路 | 一次 `generate()` 贯穿 Source → Operation → Dynamic Bind → Document → Squish → Budget |
| Python 遥测配置 | `configure`、`use`、`generate(telemetry=)` 的优先级和隔离正确 |
| 遥测 Sink | JSONL / SQLite / Memory / OTLP 均可消费同一原生事件模型 |
| 遥测失败路径 | 超预算和 Python 自定义异常仍记录闭合的 Trace、错误码与节点 |
| 发行包 | CPython 3.15+ 在 Windows/Linux/macOS 安装 binary wheel 后通过全套 API 测试 |

### 10.3 性能验收

持续维护小批量启动与构图开销、大文件解析、整篇 `squish`、Token 计数、动态高扇出图、跨语言回调、启用与关闭遥测的对照基准。性能优化位于 **C++23 核心设计与实现过程** 中；它不是单独的后置移植阶段。

## 11. 实现依据与相关历史

- Python 3.15 官方文档 —— 最低 Python 版本；3.15 于 2026-10-09 正式发布。
- pybind11 官方文档 —— C++/Python 类型、回调、异常、GIL 与 ABI 绑定。
- pybind11 稳定 ABI / abi3t 文档 —— 适配 Python 3.15+ 及 free-threaded 构建的发行策略。
- OpenTelemetry C++ —— Trace/Metric/Event 埋点和 OTLP 导出语义的成熟参考。
- Java Stream API —— Source → Intermediate → Terminal 模型；本规范不采用一次性消费。
- Dask Delayed —— 惰性依赖图参考。
- TOML v1.0 规范 —— TOML 无二进制原生值，嵌入 Literal bytes 需要严格编码。
- Build Systems à la Carte —— 动态依赖、调度与缓存模型的理论背景。
- xmlsquish｜XML Prompt 压缩工具
- xmlsquish｜pack / sopack 与编译管线

**规范摘要**：所有内容由 `Source` 表达；`Literal=bytes`、`Text=UTF-8`；文档整体操作在全部 Part 组合之后执行；C++23 原生动态图和算子内核经 pybind11 服务 Python 3.15+；整个 `generate()` 流程具备 Python 可配置、默认关闭的 C++ 原生端到端遥测。**这是单一完整技术方案，不进行分阶段开发。**