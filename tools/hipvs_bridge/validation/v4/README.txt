Development tests; these are not the organizer's evaluation harness.

Use an isolated directory W on the contest GPU image. Copy these scripts and
validation/bench_sql.py into W. Build the current bridge as
W/compact/libseekdb_cuvs_bridge.so and current kernel as W/seekdb_compact.
Copy current adaptor source to W/compact_adaptor.cpp. Build parent375ab79
separately as W/seekdb_v3, with its bridge in W/build/libseekdb_cuvs_bridge.so.
Keep hipVS unchanged. Never load the parent bridge into the new kernel: the
new kernel requires seekdb_cuvs_copy_dataset.

Prepare full/{normal256,sift250k}_{base,query,truth}.npy: FP32 inputs and exact
top100 truth. Preparation scripts and provenance are retained in the evidence.
NumPy/PyMySQL are development dependencies only, not seekdb runtime dependencies.

Run test_dataset_copy.py and test_staging_file.py. After compiling both real
binaries, write PASS to W/build_compact.pass and run sql_compact_pair.py with
stdout/stderr saved in W/sql_compact_pair.log. It creates new test instances
under W/instances using port2881. Do not use an occupied port or reuse an old
test directory. Then run sql_snapshot_functional.py; it restarts those test
instances and exercises historical/new snapshots and fixture lifecycle tables.
Insert/delete checks only affect the newly created lifecycle database.

Inspect exit codes and JSON. Marker files are orchestration helpers, not proof
of successful compilation/tests. REPORT.txt records the actual measured result.
