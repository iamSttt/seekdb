# SAMPLE FIXTURE -- not evidence.
#
# This file exists so that `python3 tools/build_conversation.py --check
# tools/samples/good` has something valid to validate. Every text field starts
# with `示例（模板）` on purpose: if you copy one of these records into
# agent/raw/ you are writing someone else's sentence into your own evidence
# file. Read docs/CONVERSATION-GUIDE.md and write your own.
#
# Format: one ```record block per record, markdown around the fences is ignored.

```record
id: sample-p1
kind: prompt
dimension: 问题理解与学习规划
date: 2026-11-01
text: |
  示例（模板）：先把这份赛题拆成五条能验收的能力点，每条后面写一句"用什么命令证明它做到了"，再按这个清单给我第一周的开发顺序。
note: 示例（模板）：这里原本应该是我当时复制粘贴的 prompt 原文，一字不改。
```

```record
id: sample-c1
kind: conversation
dimension: Prompt 设计与迭代
date: 2026-11-01
prompt: |
  示例（模板）：把上面那份清单压缩到三条，每条都必须能用一条命令验证。
decision: |
  示例（模板）：第一条"支持 SQL"没法验证，所以我把它改成"harness 跑通 9 个用例"；
  改完之后清单从描述性变成了可执行，这是之后所有用例的来源。
reflection: |
  示例（模板）：prompt 里加上"用什么命令证明"之后，产出立刻可验证；这句话后来成了我的固定前缀。
evidence:
  - target: sample-c1#prompt
    quote: 每条都必须能用一条命令验证
  - target: sample-p1#prompt
    quote: 用什么命令证明它做到了
  - target: sample-c1#decision
    quote: 第一条"支持 SQL"没法验证
result_evidence:
  summary: 示例（模板）：清单从 17 条压到 3 条，3 条都能用一条本地命令跑完
  command: python3 tools/harness.py run tests/cases
  measured: 示例数字：3/3 条命令可直接执行，无一条需要人工判断
  target: sample-e1#observed
score_hint: 1.5
```
