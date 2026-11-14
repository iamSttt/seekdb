# Frozen parent-source spill ablation

`plan.json` was frozen before measurement. It compares parent9ed (`9ed5baa1b1363ebd388850e74271b353954902da`, expected ELF `07af43283446b85e73f14a5c378183bb0855d83858b7925ce77561d276b44799`) with the isolated spill runtime (`2bbe6d00f2dc97182d3021f21449473581d623d2`, expected ELF `aecd9ce4887b60e12ec747677f8f02bf608a8b7173583d2ad0386b23f70b2a28`). Neither parser changes nor later lazy/shared variants are included.

Both ELFs are freshly extracted from their pinned original root Docker builds, with every archived build input matched to the frozen Git-blob SHA inventory. The candidate must also match the exact ELF that passed native and actual-image run43542440254. No imported binary substitutes for a failed rebuild.

The fixed12 pairs are default10k128 exact1000 at CPU1 and CPU2, four pairs each; spill-high-q16 at CPU1, two pairs; and wide768 at CPU2, two pairs. Every scenario alternates AB/BA, seed7319, 2GiB, swap0 and matching CPU quota/affinity. There are no additional q1 pairs in this frozen run. All samples, original per-request records, restart/oracle checks, cgroup resources, first queries and negative outcomes are retained.

The byte-identical `research1114` tools provide generation, transport, exact oracle, timing and cgroup measurements. Their `--highest-primary-reference retained` selects the existing four-default-plus-two-protection schedule only. Raw original selector/family fields are preserved, but this wrapper's family and source/ELF identities explicitly identify **parent-source ablation**, not retained-R or EE. The actual R wrapper and its canonical33/acdd identity checks are neither modified nor invoked.

Two pure metric-reader functions are extracted from `retained-guards-1114`, preserving their logic. The original numerical policy files are copied unchanged. `reader-preflight.json` verifies31 metric summaries against the original reader on existing raw data and verifies missing/invalid evidence fails. It is not new performance evidence.

The historical rules are used without relaxation:

- Default time/CPU regressions trigger only if overall, AB-only and BA-only ratio medians all exceed1.02; memory uses1.05.
- Spill/wide protection triggers only if the same metric exceeds its threshold in both individual pairs.
- Spill query protection uses its initial16 queries, not the later cycle aggregate. The wide seven phases are summed per run before ratios.
- Missing metrics, missing pairs, identity/response/oracle/resource failure, invalid numbers and zero denominators never pass.

The process can complete with a numerical regression block: inspect `numerical_ablation_guards_passed` and `guard_triggers`. Even with no trigger, release/full24/adoption fields remain false. This run does not replace the main EE/R comparison or prove a formal score. `ci_spill_ablation_1114.sh` is the isolated entry; the branch's unchanged workflow reaches it through its diagnostic-only `ci_score.sh`.
