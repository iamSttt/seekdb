Validation artifacts for AMD CAGRA candidate v2 (2026-10-31)

Official task: https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/seebnbaoxot1b0tb
Frozen baseline: 634888d9b037153fa143d2c913ba4b25503e1d62

This directory contains development benchmarks, not the official grader.
Use the exact build commands in the task; do not replace the fixed hipVS library.
bench_bridge.py reports bridge-only time; bench_sql.py measures offline CREATE
VECTOR INDEX plus first query and individual SQL throughput.
Synthetic normal vectors are not SIFT or the organizer's hidden dataset.
Do not infer an official score, GPU-path gate, or memory gate from these results.
REPORT.txt and results/v2 describe the current candidate. REPORT_v1.txt and the
other results are historical. The v2 development SQL pairs exceed the scoring
formula's speed thresholds, but hidden data and official scoring remain pending.

From the repository root, with the organizer's frozen deps/3rd and Rust cache:
  HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm tools/hipvs_bridge/build.sh /tmp/cagra-bridge/libseekdb_cuvs_bridge.so
  cmake -S . -B build_release -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON \
    -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
    -DCUVS_BRIDGE_LIB=/tmp/cagra-bridge/libseekdb_cuvs_bridge.so
  cmake --build build_release --target seekdb

The Python commands below are run from tools/hipvs_bridge/validation.

Generate reproducible inputs:
  OPENBLAS_NUM_THREADS=24 python3 synthetic_data.py --output /tmp/cagra-data

Run each independently compiled bridge on the SAME inputs:
  python3 bench_bridge.py --library /path/to/libseekdb_cuvs_bridge.so --data /tmp/cagra-data --dataset synthetic256 --output /tmp/cagra-result --queries 4096

Run frozen and candidate variants sequentially on an idle GPU, alternate order,
and retain all results. Never reuse a graph between build measurements.
The script verifies ID bounds, uniqueness, finite distances, recall against exact
FP32 L2 neighbors, and snapshot graph/vector byte equality across a reload.
It separately records result identity because the fixed AUTO parallel search can
produce different approximate results on repeated calls to the same index.

Formal validation must additionally cover both datasets through 4096 individual
SQL queries at k=10 and 100, the offline DDL plus first query interval, lib=vsag
SQL functionality, GPU profiling, stability and resource limits. Final scores
come only from the official competition platform.

SQL development comparison (requires numpy and pymysql, installed in the test
environment only; these are not build or runtime dependencies of seekdb):
  python3 bench_sql.py --data /tmp/cagra-data --dataset synthetic256 --backend cuvs --output /tmp/sql-gpu
  python3 bench_sql.py --data /tmp/cagra-data --dataset synthetic256 --backend vsag --output /tmp/sql-cpu
Use an isolated seekdb instance with vector indexing configured. Each invocation
creates a uniquely named database and imports all vectors before starting the
DDL timer. Databases and input TSVs are retained for inspection. The script does
not restart the server, enforce hardware quotas, or measure peak GPU memory.
Its result is a development measurement, never an official platform score.

To reproduce the template demo comparison instead of the synthetic test:
  OPENBLAS_NUM_THREADS=24 python3 prepare_demo.py --output /tmp/cagra-data
Then pass --dataset demo --queries 100 to each SQL invocation.

Submission: select the delivered branch and full commit on the competition
platform. The task requires a private answer repository and a read-only token
in the registered repository URL. Configure those through GitHub/platform UI;
never put credentials into this repository, test logs, or the report.

Inspect the current paired SQL results without running GPU work:
  python3 summarize.py results/v2
This uses only Python's standard library. Its weighted ratios deliberately say
that Normal256 substitutes for Hidden250K; they are not an official score.

Reproduction layout for the recorded v2 experiment scripts:
  v2/ contains the exact orchestration scripts used in the cloud workspace.
  They expect a disposable working directory, not a production database.
  Copy those scripts into that directory; prepare.py followed by prepare_full.py
  generates normal128/normal256 and the full/ input directory (numpy required).
  source/ contains a single seekdb checkout with the frozen offline dependencies.
  frozen/ and candidate96/ contain independently compiled bridges with the same
  libseekdb_cuvs_bridge.so name; use frozen commit 634888d... for the comparator.
  The SQL pair scripts use fresh per-run instance directories and port 2881.
  Do not run multiple queues on the same GPU or reuse an existing instance path.
  build_full.py, bench_sql.py, and sql_pair.py retain exact flags and parameters.
  prepare_sift.py records the one-off range-download transport and its wait
  prerequisites; sift_provenance.json identifies those exact public input bytes.
  For portable benchmarking of any prepared dataset, prefer the commands above
  with explicit --data/--dataset/--library arguments over the archived queues.
  Profiling runs are separate and are excluded from the timing comparisons.

The final official build-script output is byte-identical to the tested
candidate96 bridge (see manifest_v2.json). No compiled binary, dataset, auth
configuration, or secret is part of this repository submission.
