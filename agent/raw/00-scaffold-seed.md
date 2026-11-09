# agent/raw/ —— 原始笔记（工具只读取 ```record 围栏内的内容）
#
# 写法见 docs/CONVERSATION-GUIDE.md；校验与生成：
#     python3 tools/build_conversation.py --check     # 只校验
#     python3 tools/build_conversation.py             # 生成 agent/conversation.json
#
# 本文件内容：**从仓库初始提交的 agent/conversation.json 原样转录**（没改写、没补写），
# 用来证明流水线本身能跑通，并给出一个诚实的起点。
# 真正的证据要你自己往 agent/raw/ 里加：具体 prompt 原文 + 它驱动的决策 + 量到的结果 + 纠错反思。
# 这里所有记录都没有 evidence / result_evidence，所以都不会被计分——这是准确的：
# 目前仓库里确实还没有可引用的实测记录。

```record
id: seed-p0
kind: prompt
dimension: 问题理解与学习规划
date: 2026-11-08
text: |
  Need a from-scratch vector DB: SQL subset, persistence, transactions, crash recovery, exact and approximate TopK over stdin/stdout, packaged in Docker. Start with the smallest thing that answers SQL.
note: 转录自 scaffold 初始提交（原 role_context：student asked the agent team to build MiniVectorDB from the competition brief）。
```

```record
id: seed-r0
kind: retrospective
dimension: 总结复盘与知识迁移
date: 2026-11-08
text: |
  First version: parser, in-memory tables, WAL + snapshot, exact L2 TopK. Not yet benchmarked against the platform.
```

```record
id: seed-l0
kind: limitation
date: 2026-11-08
text: |
  ANN index not yet wired into the executor
```

```record
id: seed-l1
kind: limitation
date: 2026-11-08
text: |
  Output protocol is an assumption (mysqltest-style tab separated); the official driver is not published yet
```
