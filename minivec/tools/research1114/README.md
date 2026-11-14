# Source-comparison screening harness, 2026-11-14

`../best_screen_1114.py` reuses the historical benchmark and independent sampled
TopK oracle without modifying the engine or the existing CI scripts. The eight
imported Python/config dependencies retain their original relative layout and bytes;
the ninth imported file is the original launcher at `cgroup_exec.c`. Their original
commit, Git blob IDs and local byte SHA256 values are in `source-manifest.json`.
`metrics.py` is the verbatim `standard_metrics` function from lines 20–53 of the
original `summarize_highest_primary.py`, with only its required imports. Scoped
`.gitattributes` sets `-text` so Git preserves the hashed vendor bytes on Linux
even when the Windows checkout uses `core.autocrlf=true`.

On a Linux host with writable cgroup v2 and an already compiled `cgroup_exec.c`
launcher:

```sh
sudo -n env PYTHONDONTWRITEBYTECODE=1 python3 minivec/tools/best_screen_1114.py \
  --baseline /absolute/ee-minivec \
  --candidate /absolute/candidate-minivec \
  --candidate-source FULL_40_CHARACTER_SOURCE_COMMIT \
  --launcher /absolute/cgroup_exec \
  --output /absolute/screen.json
```

The default baseline source is EE `ee299adb296a2922d01982aa6c5707d37e767b41`.
For a direct ablation against another built version, explicitly pass
`--baseline-source FULL_BASELINE_COMMIT`. This retains exactly the same 12-pair
schedule, generator, oracle and metrics, but uses the distinct
`source_ablation_12pair_screen_20261114` family. It reports no EE threshold
pass/fail; the raw files are named `comparison-1cpu.json` and
`comparison-2cpu.json`. Both declared source commits and measured binary hashes
remain explicit. This comparison does not by itself establish a single-change
causal effect: the caller must bind the source/build differences separately.

The unmodified underlying tool is invoked with `--highest-primary-reference ee`
solely to select its existing six-default-pairs-per-CPU schedule. Its raw
historical family/selector labels are preserved as source evidence, not used as
a claim that a non-EE baseline is EE. Wrapper metadata states this distinction.

Optional `--baseline-evidence` and `--candidate-evidence` record externally
produced build-proof file hashes. Binary SHA256 is independently measured here;
source commit labels remain caller declarations. This wrapper does not certify
the Docker build, source-to-ELF linkage, public/hidden correctness gates or the
identity of an official evaluator's executable. Parent CI must bind those facts.

The fixed plan is six alternating AB/BA pairs at each of 1 and 2 CPUs, 2 GiB,
swap zero, seed 7319, default 10k rows × 128 dimensions, exact search, 1000 queries
before shutdown. Original restart validation and 1000 post-restart queries are
also retained. The measured first-lifecycle wall sum includes shutdown; its CPU
counter ends before shutdown, matching the old definition. All inherited MVDB
variables are removed for both binaries. There is no automatic retry, cache
drop, warmup discard, source/ISA selection or sample trimming.

`screen.json` embeds both full raw arm reports and metric comparisons. Adjacent
`screen.json.d/` keeps the original arm JSON and local identity records. Any
failure leaves completed pairs and the original partial failure report, exits
nonzero and stops subsequent arms. Existing output paths are never overwritten.

In default EE mode, the four historical EE thresholds are displayed per CPU, including AB/BA
directions and every pair. A threshold pass is only an initial screening signal:
this is **12 EE pairs**, not the historical **24-pair** release gate. Retained-R
default, spill and wide-vector protections and independent confirmation remain
outside this wrapper. In explicit non-EE mode, only the paired metrics are
reported, without those EE thresholds. No score prediction or automatic adoption
is performed in either mode.

`--plan-only` validates vendored hashes and writes the plan without launching a
database. Local checks covered Python syntax, imports, exact vendor hashes and
the CLI plan; actual Linux/cgroup execution is left to the parent CI run.
