# I11 selective-scale experiment

The later confirmation completed 105 paired comparisons plus 3 A/A controls.
Outlier cases improved, but coordinate-dispersed and low16 cases regressed, so
this experiment remains disabled. See [results](../../validation/iteration3-selective-scale.json)
and the [current 20-direction decisions](../../docs/REPORT_20_DIRECTIONS.md).
The original hypotheses below describe the experiment design, not pending work.

`MVDB_BLOCK_SCALE_SELECTIVE` is an independent, default-off experiment. It only
has an effect together with `MVDB_ENABLE_BLOCK_QUANT_SCALE`. The production
Dockerfile and defaults were unchanged by this I11 experiment. No I11 default adoption or score gain is
claimed by this change.

The candidate admits local scales only for dimension >= 128 and when at least
80% of the **actual rows** have a block scale at most one quarter of the global
scale. The 128/80%/4x thresholds were conservative hypotheses used in the
experiment, not measured optimal thresholds or a guarantee of improvement.
The existing eight-scale, per-worker cache, code and overall memory limits also
apply. Short final blocks are weighted by their row count. The implementation
compares coverage with `N - N/5`, avoiding multiplication overflow.

Failed admission retains global encoding. Unfiltered serial and two-worker
scans choose global/local traversal outside the row loop; the global path
bypasses local/block-ID dispatch. Filtered paths retain their safe ordinal-aware
interface. The arithmetic bound, strict comparisons, uncertainty fallback,
legacy local two-argument rejection and DESC handling are unchanged.

Run in Linux CI:

```sh
bash minivec/tools/ci_selective_scale.sh
```

Use a 60-minute job and always upload `selective-scale-results/`. The script
enforces a 55-minute work limit, preserves incomplete results and fails if
report collection fails. It refuses to overwrite an existing results directory.

The fixed GCC 13.3 image builds base, unrestricted I11 and selective binaries
from the same source without PGO. Correctness includes the original arithmetic
tests, filter paths, memory budgets, JSON protocol, one/two-thread exact Top-K,
the original unrestricted I11 tests and selective admission tests. The selective
tests cover dimensions 16/127/128/768, exact 80% coverage, one-row tails,
insufficient twofold improvement, budget fallback, invalid cache metadata,
neighboring distance thresholds and ties. SELECTIVE without its parent macro is
also compiled against the base object. ASan/UBSan and I10 block64 composition
are separate correctness configurations.

Full-request base/selective comparisons use three evaluation seeds and five
alternating pairs, with 64 queries per phase, for each of seven scenarios:

- Original standard10k mixed-filter control.
- Ordinary Gaussian data at 10,000 x 128.
- Tail outliers and row-dispersed outliers at 20,000 x 128.
- Coordinate-dispersed outliers at 20,000 x 128.
- An actually quantizable 4,096 x 768 outlier case.
- The original 100,000 x 16 regression control.

Production norm ordering remains enabled: it can regroup row-dispersed
outliers. The coordinate-dispersed control instead places a fixed large
coordinate in every row, so sorting cannot remove block contamination. New
synthetic inputs use a finite dyadic grid and independently accumulate integer
squared distances for every query. Original controls retain their original
input generator and sampled oracle. Predicted gate summaries are explicitly
source-based reconstructions, not observed branch counters.

BEGIN/INSERT/COMMIT wall times are recorded separately, together with their raw
samples and the original full-phase totals. They include transport and JSON
decode; they are not direct fsync measurements. Three same-binary standard10k
pairs are an additional A/A control. Binary/source/harness hashes, all positive
and negative pairs, partial failures, stderr and resource proxies are retained.

`bench_selective_scales.cpp` is a **separate** single-thread microdiagnostic.
Unlike the older `bench_quantized_scales.cpp`, it includes the production float
prefilter and `distance_squared` fast/canonical key path. Its exhaustive
`distance_exact` oracle is outside the timer. It reports actual scale-row
coverage, integer/float rejections, canonical key evaluations, cache/build time
and retained bytes. It still excludes protocol and the two-worker split and
must not be treated as a full-request speedup. Its timings are not directly
comparable with the old microbenchmark's timings.

Local Python oracle verification:

```sh
python3 minivec/tests/test_selective_workload.py
```

This validates benchmark arithmetic and coverage accounting. Linux compilation,
live engine tests and paired timings are separate required evidence before any
adoption decision.
