# Agent 学习证据填什么、怎么写（一页版）

`agent/conversation.json` 是评审唯一会读的文件：他们在里面按 **JSON pointer + 原文片段** 定位每一句话，
给四个维度各自的「独立证据实例」打 **0.5 / 1 / 1.5 / 2** 分，再排序。所以文件里每一句声称都必须
能被一条 pointer 指到、被一段逐字片段证明——否则那个实例就是 0。

**本工具链不生成内容。** `tools/build_conversation.py` 只做三件事：读你手写的 `agent/raw/`、
按四个维度重排、逐条校验。指到不存在的原文、超过 128 KiB、声称是证据却没有结果，一律拒绝输出。

---

## 1. 文件规格

| 项 | 要求 |
| --- | --- |
| 路径 | `agent/conversation.json`（仓库根下） |
| 大小 | ≤ 128 KiB（131072 字节），到 80 % 就该开始删 |
| 编码 | UTF-8，顶层是一个 JSON object |
| 必填 | `synthetic: false`，以及 `user_prompts` / `conversations` / `experiments` / `retrospective` / `limitations` 五个数组 |
| 引用 | 每条 `evidence` 必须有 `json_pointer` 和 `quote`；`quote` ≤ 240 字符，且**逐字**出现在该 pointer 位置上 |

## 2. 每个实例要回答四问

1. **原来那句 prompt 是什么** —— 原文，不要润色，不要事后总结成一句话。
2. **它驱动了什么决策** —— 因为这轮对话你改了什么（清单、顺序、验收标准、放弃的方案）。
3. **结果怎么量的** —— 命令、数字、产物路径。没有数字的这一条基本只能拿 0.5。
4. **哪里错了、你怎么发现的、改了什么** —— 评审在"验证纠错与独立判断"这一维度专门找这个。

四个维度分别收什么：

- **问题理解与学习规划**：你怎么拆赛题、怎么排优先级、哪些需求被明确砍掉及原因。
- **Prompt 设计与迭代**：同一目标下的 prompt 演进：加了什么约束（可验证、边界、输出格式），为什么加。
- **验证纠错与独立判断**：你自己设计的反例/对照实验；你**没有**照模型说的做、并用数据推翻它的那一次。
- **总结复盘与知识迁移**：把一次踩坑抽象成规则，并用在后面的地方（迁移）。

## 3. 原始笔记格式（`agent/raw/`）

两种都支持，可以混放，按文件名排序读取。

**A. Markdown 记录块**（推荐，写长文本舒服）—— 只有 ```record 围栏内的内容被读取，
围栏外的说明文字随便写：

````markdown
```record
id: p1
kind: prompt            # prompt | conversation | experiment | retrospective | limitation
dimension: 问题理解与学习规划   # 可省略，按 kind 取默认
date: 2026-11-02
text: |
  这里逐字粘贴我当时输入的 prompt 原文。
```
````

**B. JSONL**（推荐给实验记录）—— 一行一个对象，字段同上。

字段：

| kind | 必填 | 常用可选 |
| --- | --- | --- |
| `prompt` | `id` `text` | `note` |
| `conversation` | `id` `prompt` `decision` | `reflection` `prompt_ref` `score_hint` |
| `experiment` | `id` `question` `change` `command` `observed` | `conclusion` `score_hint` |
| `retrospective` / `limitation` | `id` `text` | — |

引用与结果（任意 kind 都可带）：

```yaml
evidence:
  - target: c1#decision      # <记录id>#<字段名>，也可以写 pointer: /conversations/0/decision
    quote: 逐字片段（≤240 字符）
    note: 可选，一句话说明它证明了什么
result_evidence:
  summary: harness 9/9，构建 42 秒   # 必填：到底量到了什么
  command: bash tools/ci.sh
  measured: 9/9 cases passed
  target: e1#observed        # 可选：结果写在哪个字段里
score_hint: 1.5              # 可选，只能 0.5 / 1 / 1.5 / 2
```

`kind` 到四个数组的映射：`prompt → user_prompts`、`conversation → conversations`、
`experiment → experiments`、`retrospective → retrospective`、`limitation → limitations`。

## 4. 生成与校验

```bash
python3 tools/build_conversation.py --check          # 只校验，不写文件（改完先跑这个）
python3 tools/build_conversation.py                  # 生成 agent/conversation.json
python3 tools/build_conversation.py --verify agent/conversation.json   # 校验已有文件
python3 tools/build_conversation.py --stats          # 各维度条数、引用数、分数区间
```

退出码 0 = 通过；1 = 被规则拒绝（stderr 给出 `E_*` 码 + 定位 + 原文对照）；2 = 用法错误。
**被拒绝时不会写文件**，所以坏文件进不了分支。

三条硬规则（也是 `bash tools/ci.sh` 会跑的）：

1. `quote` 在它声明的 pointer 处找不到 → `E_QUOTE_NOT_FOUND`（`tools/samples/broken/` 就是这个）。
2. 生成结果超过 128 KiB → `E_TOO_LARGE`（会列出最大的几个实例，先删长引文）。
3. 一条实例带了 `evidence` 却没有任何 `result_evidence.summary` → `E_MISSING_RESULT_EVIDENCE`
   （`tools/samples/broken-no-result/` 就是这个）。

---

## 5. 两个写法示例

> 下面两段**是模板，不是证据**。它们用 `示例（模板）` 开头，就是为了防止被原样粘进 `agent/raw/`。
> 抄结构，不要抄句子。

**示例 A —— "Prompt 设计与迭代" 该长什么样（模板）**

```record
id: c-template
kind: conversation
dimension: Prompt 设计与迭代
date: 2026-11-02
prompt: |
  示例（模板）：把上面那份清单压缩到三条，每条都必须能用一条命令验证。
decision: |
  示例（模板）：第一条"支持 SQL"没法验证，所以我把它改成"harness 跑通 9 个用例"；
  改完之后清单从描述性变成可执行，后面所有用例都是从这三条长出来的。
reflection: |
  示例（模板）：prompt 里加上"用什么命令证明"之后，产出立刻可验证。
evidence:
  - target: c-template#prompt
    quote: 每条都必须能用一条命令验证
result_evidence:
  summary: 示例（模板）：清单从 17 条压到 3 条，3 条都能用一条本地命令跑完
  command: python3 tools/harness.py run tests/cases
  measured: 示例数字：3/3 条命令可直接执行
score_hint: 1.5
```

**示例 B —— "验证纠错与独立判断" 该长什么样（模板）**

```record
id: e-template
kind: experiment
dimension: 验证纠错与独立判断
date: 2026-11-02
question: 示例（模板）：默认输出模式下 ORDER BY 普通列到底有没有生效？
change: 示例（模板）：先把期望值写成 1,2,3 跑一遍用例，而不是先改代码；跑之前不预设结论。
command: python3 tools/harness.py run tests/cases/06_update_delete.sql
observed: 示例（模板）：期望 1,2,3，实际是 2,3,1 —— 问题在输出路径，不在存储。
conclusion: 示例（模板）：先复现再定位，一次运行就把范围缩到排序分支。
evidence:
  - target: e-template#observed
    quote: 期望 1,2,3，实际是 2,3,1
result_evidence:
  summary: 示例（模板）：一次运行复现，0 次改代码就定位
  measured: 示例数字：1 次运行
score_hint: 2
```

## 6. 常见坑

- **逐字**：中文全角标点、空格、换行都算。改一个"的"就会 `E_QUOTE_NOT_FOUND`。
- 引文超过 240 字符会被截断式拒绝；够用就好，评审只需要能对上的那一小段。
- `observed` 写"看起来正常"这类形容词等于没写；写数字、退出码、diff。
- 不要为了凑条数把一件事拆成五条同质记录——评审按"独立实例"计分，重复陈述不加分。
- `synthetic` 只能是 `false`；从模板抄来的内容不许进这个文件。
