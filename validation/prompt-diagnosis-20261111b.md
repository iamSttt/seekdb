# Prompt input and evaluator diagnosis, 2026-11-11

The official Agent Prompt result is still unavailable. Local input validation is not an official evaluator result. This report records what can be checked and the remaining diagnostic gap; it does not assign an Agent score.

## Observed official results

| Submitted commit | Submission time (Asia/Shanghai) | MySQL | Displayed E | Agent Prompt |
|---|---|---|---:|---|
| `fa562be89069a84215a6196c9e24e4c6274f3862` | 11-10 21:33 | pass | 2180.028190 | invalid/pending review, no raw score |
| `ff9c6bfbe89e1045e12ac56652165245c834630c` | 11-11 12:23 | pass | 2708.402710 | same combined diagnostic |
| `2d1dc249bb64804e294b18f49a4634b427952c38` | 11-11 16:20 | pass | 2336.046455 | same combined diagnostic |

Source: authenticated [submission records](https://open.oceanbase.com/train/TestRecord?questionId=600081), read without initiating a formal submission. The result dialog exposes no per-dimension report, three-review results, material digest, rule version, or failure-specific reason. The message combines invalidity, pending review, the platform's 0–100 range, and a missing material summary; it does not identify which condition occurred.

`2d1dc24` is a child of `ff9c6bf` changing only the two conversation files. Source, third-party code, Dockerfile and Docker context are identical. Its lower E therefore cannot be attributed to a runtime source change. Missing component metrics and evaluation conditions prevent a more specific explanation.

## Contract checked against the current task

[Task sections 3.4 and 4.4](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lg8enxlmwg69unt6), re-read on 2026-11-11, specify:

- Default input `agent/conversation.json`, a UTF-8 JSON object no larger than 128 KiB, with unique keys and finite JSON numbers. Business field names are suggestions, not a mandatory schema.
- Actual user/Agent context and results must be inside that JSON. External files and links are audit trails, not substitutes for internal evidence.
- The current raw-score rule is `agent-learning-evidence-v3-unbounded`. Raw scores have no fixed cap. The separate 40-point award comes from rank; it is not a raw-score ceiling or guaranteed score.
- Instances require located, exact student and result quotations. One correction chain cannot be split across dimensions for repeated credit. Agent-authored choices, explanations and experiments do not by themselves establish independent student understanding.
- Three independent reviews are aggregated by dimension medians. Insufficient evidence, disagreement or inconsistent material-type judgments can require review. Invalid evaluator structure or citations are evaluator-output failures, not automatic proof of student zero.

The task's unbounded raw-score contract and the platform's combined 0–100 diagnostic are inconsistent signals. They justify requesting the actual evaluator status; they do not prove that the submitted raw score exceeded 100. Capping a locally invented score would not repair this uncertainty.

## Submitted material inspection

The two `ff9c6bf` copies are byte-identical, 118,132 bytes, with a nonempty summary. The two `2d1dc24` copies are also synchronized; its observed input is 105,774 bytes. All 34 pointer/quote references in the latter resolve to matching strings and meet the 240-character citation limit. Neither oversized input nor broken references explain the visible failure from the available evidence. A nonempty summary in the input also does not prove that an evaluator-output summary was produced or successfully extracted; the combined platform message does not specify which summary it means.

The later material has semantic issues worth repairing:

1. Five `tier: 1` annotations are local self-assessments, not a required input or official rating. Remove them while preserving the factual source records.
2. The five-part selection record uses Agent-authored option wording. Preserve that attribution and the prior submission source; do not describe it as five independently authored technical explanations.
3. Selecting an option about per-row filtering is not evidence that the user independently located full-row deserialization. Likewise, a recorded 300 ms choice does not establish its timing relative to an experiment.
4. A prior-day result cannot establish a subsequent validation chain for a newer selection. The ANN speed estimate is not an implemented ANN experiment or a measured recall result, and it does not demonstrate that a recall gate caused the decision.
5. The `47895bbddc88e3bea04946e84712b6ce30e6e5fe` performance experiment belongs to another branch; GitHub CI run `42981436030` independently confirms a successful run for that SHA. It is not included in the runtime tree of `2d1dc24`; its speedups must not be presented as that submitted commit's performance. Subtracting timings from different workloads does not establish a precise transport cost or a hard optimization ceiling.

The revised input preserves exact in-chat messages and separately attributed prior-submission records, embeds concrete measurements, removes unsupported scoring/causal assertions, and records unavailable evidence. It does not manufacture student contributions or hide the failed official results.

## Information only the official evaluator can supply

To distinguish input extraction from review/output failure, the useful diagnostic is the locked commit, extracted material path/byte count/SHA-256, actual rule/configuration version, three review statuses and dimension values, failing citation or structure checks, and the reason behind any `NEEDS_REVIEW` or range rejection. The current UI does not expose these. No external message or appeal was sent, and no formal submission was consumed during this investigation.
