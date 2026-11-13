# 本轮对话材料与校验

当前材料是 `agent/conversation.json`，根目录 `conversation.json` 是相同字节的副本。从用户指定的 `a90b2b750e40f020793cddefe01d5b66631afc63` 开始记录；不得把旧历史重建成当前用户发言。材料内 ID 是仓库标识，不是聊天平台的原始 message ID。

## 规则来源与适用范围

2026-11-13 在用户登录的浏览器中核对了[赛题](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lg8enxlmwg69unt6)的 3.4、4.4 节（页面显示 11-11 18:25，规则版本 `agent-learning-evidence-v3-unbounded`）。该页面是规则来源，本指南只归纳当前核对内容；本地旧提交的说明不是新规则的替代品。

4.4 的材料输入约束：UTF-8 JSON，顶层对象，默认路径 `agent/conversation.json`，不超过 128 KiB（131072 字节），无重复键、NaN 或 Infinity。内部业务字段只是建议，**不要求输入具有固定五维字段**。记录必须真实、区分来源，未执行计划须明确标注；不得包含令牌等敏感凭据。

3.4 的五个维度为：问题理解与自主拆解、技术理解与迁移、独立验证与质疑、实验与反馈迭代、取舍与反思。一个修正链不能拆成多个维度重复计分。本仓库不提供 `score_hint` 或预计官方分数。

规则中评分报告的 `student_evidence` / `result_evidence` 引用要求 RFC 6901 pointer 定位字符串，并给出不超过 240 字符的精确原文；这是**评分输出的引用约束，不是强制材料输入格式**。下述可选本地索引借用同样的引用方式来避免坏链接，不声称自行生成了官方评分报告。

## 当前本地数据结构

- `messages`：`id`、`role`、`source_kind`、`content`。真实用户聊天原话保持完整并保留 `user`；实现、实验和解释保留为 `assistant`。报告附件以 `content_type=verbatim_attachment_excerpt` 明示逐字摘录，完整原件按原始字节归档；这是附件表示，不是新增用户发言或独立作者证明。压缩后的 assistant 内容明确标为语义摘要。计划不能改写为已执行结果。
- `adopted_directions` / `direction_status`：报告方向及状态。`source_message`、`source_pointer`、`quote` 定位实际用户来源；`implementation_role` 保持 `assistant`。I17/I18 的三段用户渠道提交已定位到更早的 assistant 原文，保留提交角色和原话，但不能计作用户独立预测或判断；详见 `attribution_audit`，独立个人证据仍需真实记录。
- `experiments`：实际执行者、状态、内置核心结果、原始证据文件路径及 SHA256。负结果、失败和回退决定保留。部分表用 `columns` 加 `rows` 共享列名；数值单位、基线/候选顺序与比值含义由同一结果中的列定义解释。摘要不能替代原始全部样本。
- `provenance.user_content_sha256`：保留逐条原始用户来源的 SHA256，真实聊天仍直接校验完整 `content`；报告附件的旧完整原文哈希保持不变。摘录另以 `excerpt_sha256` 校验当前 `content`，以 `original_content_sha256`、`original_bytes` 和 `attachment_original` 的仓库路径/哈希校验完整原件。`excerpt_line_ranges` 为从 1 开始、包含两端、按序不重叠的原文件行区间；原字节按 UTF-8 解码且不转换换行，拼接指定完整行必须逐字等于摘录。`is_new_user_message=false` 明示没有新造聊天。这些可编辑哈希不是作者身份认证。
- 可选 `evidence_units`：一个修正链一个唯一 `id`、一个上述维度，`student_evidence` 定位用户字符串，`result_evidence` 可定位实际结果；结果尚未产生时为 `null`。不填自评分。换 ID 或重新措辞仍可能是同一链，机械去重不能代替人工判断。

附件摘录例外目前仅允许 `user-report`；其他真实聊天消息不能通过改写 `source_kind` 切换到该分支。

## 校验入口与防覆盖

```sh
python3 -B minivec/tools/validate_iteration_material.py
python3 -B minivec/tests/test_iteration_tools.py
```

可用 `--root /path/to/repository` 只读检查另一份工作树。校验器没有生成、修复、覆盖或 `--write` 模式；成功和失败都不会写材料。构建与验证流程只调用检查入口，不重新生成对话。a90 基线已移除旧四维生成器，本轮不恢复它；不要运行历史版本的 raw 生成器覆盖这两份记录。

本地检查包括：两副本字节一致、UTF-8/对象/容量/重复键/有限数值、消息和实验 ID、用户内容哈希、来源角色、精确 quote 及 RFC 6901 pointer、表格列数、证据路径不能越出仓库、附件原文与摘录双哈希及逐字重建、可选证据单元去重和禁止自评分。报告原件使用单文件 `.gitattributes -text` 保持 Windows/Linux 检出字节一致。`python -O` 不能绕过这些检查。这些附加字段规则是本仓库的维护约定，不能宣称官方要求全部参赛材料使用此 schema。

修改材料前保留 Git 版本，逐条确认新增消息实际存在及作者；压缩时保留真实聊天原话、核心正负结果、必要来源和限制。附件可使用上述逐字摘录表示，但必须保留完整原件，不得把摘要或改写伪装成原话。当前报告保留 20 项标题及做法、方法限制和 I16–I20 完整段落，现有 quote/pointer 仍直接定位内置字符串；不能假定官方评审会读取归档中的其余文字。同步写入两副本后运行检查，审阅 diff。没有自动合并或生成器保证真实性，不应凭哈希一致就接受不实文字。

## 有效性边界

校验通过不代表官方 Prompt 有效、隐藏用例通过、作者真实性已认证或五维达到某个分数。机械工具也不能证明预测确实早于结果、用户确实理解技术、两个不同 ID 属于不同纠错链，或彻底排除文字中所有敏感信息；这些需要原始上下文和人工审阅。当前程序不访问网络、不上传材料。

本轮性能证据中的 RSS/HWM、进程 I/O 是代理，不能换算官方 E；不同提交和负载的加速比不能相乘。旧正式 E=3014.419039 属于 `ee299adb`；用户已提交的 `e020fdeb` 经登录核实为 E=2937.367246，Prompt仍无有效分数。6000分未验证。材料压缩和文档统一也不是提分证明。
