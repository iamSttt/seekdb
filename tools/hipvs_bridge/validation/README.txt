Validation artifacts for the AMD CAGRA V5 submission

Official task: https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/seebnbaoxot1b0tb
Frozen baseline: 634888d9b037153fa143d2c913ba4b25503e1d62

This directory contains development benchmarks, not the official grader.
Use the exact build commands in the task; do not replace the fixed hipVS library.
The benchmark reports bridge-only time, not CREATE VECTOR INDEX SQL time.
Synthetic normal vectors are not SIFT or the organizer's hidden dataset.
Do not infer an official score, GPU-path gate, or memory gate from these results.

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
FP32 L2 neighbors, and equality across a snapshot round trip.

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
