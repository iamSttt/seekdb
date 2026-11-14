# Typed-wide on 760f: correct, but source-factor protection failed

Run [43552265888](https://github.com/iamSttt/seekdb/actions/runs/43552265888) completed all correctness checks and all12 frozen performance pairs. The candidate passes the wide-output benefit requirements, but CPU1 default queries fail the original2% stable regression protection. Keep this as a rejected standalone experiment; it is not a release recommendation or a formal score.

Runtime commit: `6412cbf862959655f351b4d908c430a98c5eabfe`. Tested driver: `4fd9e6e6b7de3b07a2fd5cca271e1cdc02ab5d98`. Branch: `research/typed-wide-1114`. The only production changes from760f are exec.cpp and R33's json_row_writer.h. JSON output switches to the typed writer only when an actual projected non-distance VECTOR has dimension>=128. Root Docker, compiler flags, and every other runtime file are unchanged; no PGO is used.

All ratios below are candidate/760f, so smaller is better. Default scenarios have4 pairs each; spill and wide each have2. Original AB/BA order, resource limits, workload, response oracle and31 guard metrics remain unchanged.

| Scenario | Query wall | Query CPU | Lifecycle wall metric (scope below) | Memory peak |
|---|---:|---:|---:|---:|
| CPU1 default10k | 1.031402 | 1.028204 | 1.027125 | 1.003360 |
| CPU2 default10k | 0.940218 | 0.948445 | 0.950091 | 1.001614 |
| CPU1 spill_high_q16 | 0.994437 | 0.969593 | 1.076715 | 0.997627 |
| CPU2 wide768 | 0.685175 | 0.686472 | 0.736753 | 0.911940 |

Default and spill use their first-process lifecycle metric. Wide uses the sum of timed startup, create/insert, shutdown and seven query phases; it has no restart and is not end-to-end process wall.

For wide768, the two query wall ratios are0.653994/0.716356, CPU0.679789/0.693155, and lifecycle stage sums0.714280/0.759226. Both pairs pass the frozen <=0.90 wall/CPU and <=0.95 lifecycle-stage-sum benefit criteria. This is a meaningful local wide-output gain, not a claim about hidden official workloads.

For CPU1 default queries, AB/BA median wall ratios are1.039189/1.031402 and CPU ratios1.031974/1.028204. Overall, AB and BA medians all exceed1.02 for those two metrics, triggering the predeclared stable protection. There are two slower and two faster pairs, so the4-pair median does not establish that every query is slower. Both sides' complete startup/restart logs select2 threads in all captured default cases; a different logged fanout choice is not observed. Added guard/object/code-layout costs are possible explanations, not measured attribution. The negative result is not discarded as noise and was not selectively rerun.

Spill lifecycle and ingestion have adverse medians, but the two individual pairs are not both beyond the same original threshold, so they do not trigger that stable rule. Their raw negative samples remain in the report. The successful wide result does not override the CPU1 failure, and this isolated stage neither reruns nor replaces full EE/R release acceptance.

Correctness passed: root Docker build and20 public cases; inherited native/recovery/JSONL/TopK/sanitizer, integer-cap and spill-cache checks; pinned GCC13.3 O3 and ASan/UBSan writer units; direct-executor54 malformed-UTF8 atomicity cases; live54 cases for each actual root ELF;165 raw-byte paired response queries with independent numeric/type/string/order expectations;464 request differentials plus4 NUL atomicity cases;56-row numeric oracle with100352 bitwise readbacks; independent shared-bound TopK oracle. The typed tests have no internal serializer-path counter, and do not claim one.

The initial attempt43551432852 ended before performance because a historical test passed an obsolete fifth parse_sql argument. The test-only adaptation preserved all54 cases and assertions. No performance sample existed in that failed attempt. Both attempts produced exactly the same runtime ELF, `41f20159c3b8ae4992bcbfd45ace6264e84125fa841140c84094d5dd6619cb06`; freshly built760f reproduced `fbeeb83977779eae977f5bec4f570b68ed7bfd5c87bb6642b7e5ffeafa76d456`.

The original `validation/runs/43552265888/` tree is preserved in this branch's [raw evidence ZIP](../validation/typed-wide-43552265888-raw.zip), alongside the [result](../validation/typed-wide-43552265888-result.json) and [import receipt](../validation/typed-wide-43552265888-receipt.json). It is also available at the [immutable original evidence commit](https://github.com/iamSttt/seekdb/tree/4e143266e798145c0ba9f044b83c5c4746c930d6/validation/runs/43552265888). Rechecking reproduced all31 metric summaries and triggers exactly, verified both raw-arm hashes and identity records, and matched each root build-input hash to its declared Git commit. The complete CI log is preserved compressed with its manifest. No formal evaluation was submitted.
