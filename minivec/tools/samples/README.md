# tools/samples/ —— 校验器的测试夹具（不是证据）

四个目录，`bash tools/ci.sh` 会逐个跑：

| 目录 | 期望 | 演示的规则 |
| --- | --- | --- |
| `good/` | `--check` 通过 | 合法记录：prompt / conversation / experiment + `target` 引用 + `result_evidence` |
| `broken/` | 拒绝，`E_QUOTE_NOT_FOUND` | 引文被重新打了一遍，不再逐字出现在它声明的 pointer 处 |
| `broken-no-result/` | 拒绝，`E_MISSING_RESULT_EVIDENCE` | 被当作证据引用（有 `evidence`），却没有任何测到的结果 |
| （ci.sh 现场生成） | 拒绝，`E_TOO_LARGE` | 合法但超过 128 KiB 的文档 |

**不要把这些文件复制进 `agent/raw/`。** 里面的每一句都以 `示例（模板）` 开头，那个前缀就是提醒：
模板可以抄结构，不能抄句子。真正的证据只能来自你自己当时的 prompt 原文与实测数字。

写法见 `docs/CONVERSATION-GUIDE.md`；生成与校验：

```bash
python3 tools/build_conversation.py --check                       # 校验 agent/raw/
python3 tools/build_conversation.py                               # 生成 agent/conversation.json
python3 tools/build_conversation.py --check tools/samples/good    # 夹具：应当通过
python3 tools/build_conversation.py --check tools/samples/broken  # 夹具：应当被拒绝
```
