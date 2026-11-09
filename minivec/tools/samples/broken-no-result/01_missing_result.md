# SAMPLE FIXTURE (deliberately broken) -- not evidence.
#
# Second refusal rule: an instance is CITED as evidence (it has citations) but
# records no measured result. The generator must refuse with
#
#   error: E_MISSING_RESULT_EVIDENCE /conversations/0
#     instance 'sample-nr1' is cited as evidence (1 citation(s)) but records no
#     result_evidence.summary
#
# The quote below is correct on purpose: the only violation under test is the
# missing measurement, so the message you see is unambiguous.

```record
id: sample-nr1
kind: conversation
dimension: Prompt 设计与迭代
date: 2026-11-03
prompt: |
  示例（模板）：给我一个能测出 WAL 截断后重放边界的用例，要求退出码非零。
decision: |
  示例（模板）：我把"边界"具体化成"截断 3 字节"，因为不写字节数就无法写出断言。
evidence:
  - target: sample-nr1#decision
    quote: 我把"边界"具体化成"截断 3 字节"
```
