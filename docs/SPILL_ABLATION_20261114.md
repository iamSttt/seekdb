# Spill-only parent-source ablation

[Actions run43543849779](https://github.com/iamSttt/seekdb/actions/runs/43543849779) completed all12 planned pairs without a preregistered numerical guard trigger. The harness commit was `b3ee266bcc10d64be57e81e300fda560971546ca`. Both executables were rebuilt from their original root Docker recipes and matched their previously observed ELF hashes:

| Role | Runtime source | Actual ELF SHA256 |
| --- | --- | --- |
| Baseline | `9ed5baa1b1363ebd388850e74271b353954902da` | `07af43283446b85e73f14a5c378183bb0855d83858b7925ce77561d276b44799` |
| Spill only | `2bbe6d00f2dc97182d3021f21449473581d623d2` | `aecd9ce4887b60e12ec747677f8f02bf608a8b7173583d2ad0386b23f70b2a28` |

This comparison isolates the three-file spill scalar cache against its9ed parent. It contains no later parser/lazy changes, does not identify either executable as the original EE or retained R reference, and does not constitute the full24-pair release gate. The unchanged historical `retained` selector supplies only the fixed schedule. No formal evaluation was submitted.

The plan was frozen before measurement: exact mode, seed7319, default10k/128-dimensional/1000 queries at1CPU and2CPU with four alternating AB/BA pairs each; high-hit spill16 queries at1CPU with two pairs; wide768 at2CPU with two pairs. Both roles used empty declared environment,2GiB memory, zero swap, fixed affinity and CPU quotas. The original measurement tools, oracle and numerical rules were reused without changes. All12 pairs passed their existing oracle and identical-response checks; no sample was discarded. Supplemental low/high-hit q1 pairs were not measured.

All numbers below are medians of **candidate/baseline** pair ratios; values below1 are better. Query CPU is the measured query-phase cgroup delta. Complete-lifecycle CPU includes restart/verification, whereas first-lifecycle wall stops before restart, so these two columns have different boundaries.

| Group | Pairs | Query wall | Query CPU | First lifecycle wall | Complete lifecycle CPU | Memory peak | Max process RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Default1CPU | 4 | 1.024773 | 1.018763 | 1.002391 | 0.998538 | 1.005837 | 1.003152 |
| Default2CPU | 4 | 0.975036 | 0.975236 | 0.983140 | 0.997998 | 1.000345 | 1.003154 |
| Spill1CPU, initial16 queries | 2 | 0.031047 | 0.026952 | 0.389450 | 0.390367 | 1.001577 | 1.001079 |
| Wide2CPU, seven query phases summed | 2 | 0.994442 | 1.006170 | 0.991802* | 1.003560 | 0.999562 | Not measured |

`*` Wide wall is its existing lifecycle-stage sum; it is not an independently timed first-lifecycle wall interval. Spill query is the initial16 requests before mutations, not the aggregate80 requests.

Default1CPU query wall is2.48% slower in the overall median. Its AB median is0.967113 and BA median1.043310, with two improved and two adverse pairs. The frozen default regression rule requires the overall, AB and BA medians all to exceed1.02 for time/CPU, or1.05 for memory; it was not triggered. This is not evidence that slowdown is impossible: the four query ratios are1.051560,1.088634,0.882666 and0.997985, showing substantial pair variation. Default2CPU query wall improved in all four pairs. All adverse pairs remain in the raw evidence.

The two spill pairs improve both initial query and first lifecycle, supporting this narrow payload-and-index-disk-backed enablement scope. Reading the same already-measured four write/COMMIT/16-query cycles as a descriptive secondary result gives summed-cycle ratios0.119515 and0.124080, and summed post-write first-query ratios0.618996 and0.645286. These are not new observations or additional acceptance gates. The old steady-query multiplier must not be substituted for the full lifecycle or for q1 behavior.

The special-scenario rule blocks when both individual pairs exceed the time/CPU2% or memory5% threshold for the same metric. No metric triggered that rule. The unchanged oracle, build identities and all31 numerical metric groups are preserved in the [result receipt](../validation/spill-ablation-43543849779-result.json). Its independent recheck binds56 input SHA256 values to the tested Git blobs and verifies the unchanged workflow blob. The two original arm JSON SHA256 values were recovered exactly after removing only the extra newline added by log transport.

The [raw archive](../validation/spill-ablation-43543849779-raw.zip) contains the complete23.6MB downloaded log, run identity, all18 reports, extraction manifest and independent recheck. ZIP SHA256: `fb56db97ae134217f09ceee885efbfc8f2794274b8989728a0dd873e37de4c0c`.

Correctness/native/image evidence for this exact candidate is in [the earlier diagnostic report](SPILL_DIAGNOSTIC_20261114.md). A later combined parser/lazy/spill executable still needs its own correctness, performance and independent confirmation. This result does not predict an official score and does not authorize replacing the main validation entry with this diagnostic branch's `ci_score.sh`.
