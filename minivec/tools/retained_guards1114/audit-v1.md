# Short-decimal v1 raw-record audit

Read-only audit of run `43544203031`, candidate `83bc1704a4949a195dcbd0ddbeadc99b2439b93f`; no new run, wrapper modification, sample deletion or numerical rule change. Sources: the run's `from-best/screen.json.d/ee-{1,2}cpu.json` and `from-best/shared-ablation.json.d/comparison-{1,2}cpu.json`.

All 48 measured arms have complete 882-byte stderr records. Startup and restart each record the 200-handshake calibration. Every logged eligible fanout decision selects two threads, including one-CPU runs. All 384 phase snapshots have zero throttled periods/time; whole-arm throttling and core-scheduling force-idle deltas are zero. Quota, affinity, memory and swap limits match across the paired roles. Candidate ELF, workload fingerprints and complete response SHA are consistent.

For the immediate-parent 9ed comparison on two CPUs, candidate/baseline paired medians are:

| Existing measurement or diagnostic split | Ratio |
|---|---:|
| First 1000 query wall time, original guard metric | 1.073560 |
| First 1000 query CPU | 1.026022 |
| Unfiltered (even-index) first-query phase wall sum | 1.066609 |
| Filtered (odd-index) first-query phase wall sum | 1.082861 |
| First individual request | 0.932942 |
| Restarted 1000 query wall time | 0.975457 |
| Restarted query CPU | 0.973136 |

Five of six first-query-phase pairs regress; AB and BA medians are 1.036456 and 1.153980. A diagnostic sum excluding the first four requests is still 1.075377. That calculation does not replace the original guard metric or discard samples. The regression is not isolated to fanout-eligible queries or startup/first-request cost. The restarted phase has a different direction, but this does not establish measurement noise as the cause.

The raw native-task records contain 10 ms CPU ticks and last-processor snapshots, not context-switch, page-fault, CPU-frequency or full scheduling traces. Driver affinity `[0,1,2,3]` overlaps DB affinity `[0,1]`, an acknowledged design limitation rather than proof of interference. No observed evidence warrants removing samples or dismissing the negative immediate-parent result. Exact causation remains unresolved.
