# Disk no-init candidate — rejected after the fixed twelve-pair screen

Runtime candidate: `e685eeeed6f436ab57745db9c323d946273bb5cb` (runtime bytes identical to `26e3e3616cb895dbe3963a03b3edcdeda7700a63`). Control: `760f66220f2c7186b4ba295f9e3065650e549f9e`. CI: https://github.com/iamSttt/seekdb/actions/runs/43555744931.

Actual paired host: AMD EPYC 9V74 80-Core Processor. Candidate/control ratios below1 are faster or smaller. Twelve pairs completed with identical paired responses; all31 original protection metrics were recomputed exactly from retained raw arms. No samples were discarded.

The unchanged-root Docker/public20/source/safety stages,464-request differential plus4 NUL atomicity checks,56-row numeric differential and exact TopK passed. The new disk fixture passed62 cases each on the control, candidate, and candidate ASan/UBSan. The identical candidate ELF was observed in both builds: `4830b882fb63191db9a2169fe786fa59dd0c2f9775c8dc428a2f3c3093fef1ec`.

All three extra spill-benefit conditions failed. The two initial16-query wall ratios were0.9988647371 and0.9920683202; CPU ratios1.0046304871 and0.9886759582; first-query ratios1.0108340898 and0.9734256343. Every pair had to be≤0.97/0.97/0.95 respectively. The original protection rules also blocked spill first-lifecycle and ingest wall. These wall-phase differences do not establish an allocation or I/O cause, and are not needed to reject the candidate: the intended query benefit was already too small.

| Group | Metric | Median C/B | AB median | BA median |
|---|---|---:|---:|---:|
| retained:1:standard10k | first_lifecycle_wall_seconds | 1.00333395 | 1.06069763 | 0.96737546 |
| retained:1:standard10k | first_1000_queries_wall_seconds | 0.99694290 | 1.09666140 | 0.94066975 |
| retained:1:standard10k | first_1000_queries_cpu_usec | 0.99508218 | 1.07998668 | 0.94847795 |
| retained:1:standard10k | first_query_wall_seconds | 0.98463488 | 0.98463488 | 1.01306070 |
| retained:1:standard10k | ingest_wall_seconds | 1.01470459 | 1.05423217 | 0.99614431 |
| retained:1:standard10k | ingest_cpu_usec | 1.00460665 | 1.00186162 | 1.00460665 |
| retained:1:standard10k | complete_lifecycle_cpu_usec | 0.99831784 | 1.02135612 | 0.98764657 |
| retained:1:standard10k | memory_peak_bytes | 1.00389579 | 1.00618987 | 0.98534978 |
| retained:1:standard10k | max_process_rss_bytes | 1.00000000 | 0.99984292 | 1.00000000 |
| retained:1:spill_high_q16 | first_lifecycle_wall_seconds | 1.17178067 | 1.11676436 | 1.22679699 |
| retained:1:spill_high_q16 | query_before_update_wall_seconds | 0.99546653 | 0.99886474 | 0.99206832 |
| retained:1:spill_high_q16 | query_before_update_cpu_usec | 0.99665322 | 1.00463049 | 0.98867596 |
| retained:1:spill_high_q16 | first_query_wall_seconds | 0.99212986 | 1.01083409 | 0.97342563 |
| retained:1:spill_high_q16 | ingest_wall_seconds | 1.19237496 | 1.13106544 | 1.25368447 |
| retained:1:spill_high_q16 | complete_lifecycle_cpu_usec | 1.00311852 | 1.00929915 | 0.99693788 |
| retained:1:spill_high_q16 | memory_peak_bytes | 1.00053383 | 0.99778591 | 1.00328175 |
| retained:1:spill_high_q16 | max_process_rss_bytes | 1.00010262 | 1.00010262 | 1.00010262 |
| retained:2:standard10k | first_lifecycle_wall_seconds | 1.09962142 | 0.87800931 | 1.32556538 |
| retained:2:standard10k | first_1000_queries_wall_seconds | 0.96932968 | 0.89254054 | 1.03599736 |
| retained:2:standard10k | first_1000_queries_cpu_usec | 0.98531335 | 0.93880243 | 1.02629957 |
| retained:2:standard10k | first_query_wall_seconds | 0.96858008 | 0.77589635 | 1.04680860 |
| retained:2:standard10k | ingest_wall_seconds | 1.19433778 | 0.89488298 | 1.45111756 |
| retained:2:standard10k | ingest_cpu_usec | 1.00466878 | 0.99927058 | 1.02166646 |
| retained:2:standard10k | complete_lifecycle_cpu_usec | 0.99070307 | 0.97636985 | 1.02382887 |
| retained:2:standard10k | memory_peak_bytes | 1.00391374 | 1.00391374 | 0.98994355 |
| retained:2:standard10k | max_process_rss_bytes | 1.00000000 | 1.00000000 | 1.00015713 |
| retained:2:wide768 | lifecycle_stage_wall_sum | 0.99498770 | 0.94810867 | 1.04186672 |
| retained:2:wide768 | seven_query_phases_wall_seconds | 0.98574138 | 0.92582609 | 1.04565667 |
| retained:2:wide768 | seven_query_phases_cpu_usec | 0.99588533 | 0.97966792 | 1.01210274 |
| retained:2:wide768 | complete_lifecycle_cpu_usec | 0.99901378 | 0.98752504 | 1.01050252 |
| retained:2:wide768 | memory_peak_bytes | 0.99935231 | 0.99623298 | 1.00247165 |

The first run43554996238 stopped before any performance pair because the760f request helper did not accept the already-tested parent-mode option. Its full log and61 original reports remain. Only the exact three-line b981 helper adaptation and its binding were added; runtime, workload, order, seeds, corpus and thresholds stayed fixed. This was a zero-pair diagnostic correction, not a retry of an adverse performance result.

This candidate is not adopted and is not a release or official-score claim. Both full job logs,131 original extracted reports, all52 adverse metric/pair observations, identities,31-metric recomputation and the zero-pair failure are preserved. No further variant or performance retry is planned.
