# SAMPLE FIXTURE (deliberately broken) -- not evidence.
#
# Identical to tools/samples/good/ except for ONE thing: the first quote has
# been re-typed instead of copied, so it no longer appears verbatim at the
# pointer it names. The generator must refuse to emit and say so, precisely:
#
#   error: E_QUOTE_NOT_FOUND /conversations/0/evidence/0
#     the quote does not appear verbatim at /conversations/0/prompt
#     quote: 每条都能用一条命令验证
#     at /conversations/0/prompt: 示例（模板）：把上面那份清单压缩到三条，每条都必须能用一条命令验证。
#
# `bash tools/ci.sh` runs this directory and requires that it FAILS.
# This is what the rule is for: a reviewer clicking the pointer must land on
# the quoted text, or the claim is worthless.

```record
id: sample-p1
kind: prompt
dimension: 问题理解与学习规划
date: 2026-11-01
text: |
  示例（模板）：先把这份赛题拆成五条能验收的能力点，每条后面写一句"用什么命令证明它做到了"，再按这个清单给我第一周的开发顺序。
```

```record
id: sample-c1
kind: conversation
dimension: Prompt 设计与迭代
date: 2026-11-01
prompt: |
  示例（模板）：把上面那份清单压缩到三条，每条都必须能用一条命令验证。
decision: |
  示例（模板）：第一条"支持 SQL"没法验证，所以我把它改成"harness 跑通 9 个用例"。
evidence:
  - target: sample-c1#prompt
    quote: 每条都能用一条命令验证
result_evidence:
  summary: 示例（模板）：清单从 17 条压到 3 条
  measured: 示例数字：3/3
```
