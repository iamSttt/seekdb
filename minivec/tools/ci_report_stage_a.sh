#!/usr/bin/env bash
# Isolate I05 / I08 with one fixed compiler; final adoption needs PGO image gates.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
baseline=969ecca98f817ecdef9b0c378fc99a0d6cef510d
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-report-a-XXXXXX")
test ! -e report-a-results
mkdir report-a-results "$scratch/baseline"
stage=archive
finish() {
  code=$?
  trap - EXIT
  set +e
  printf '{"exit_code":%d,"stage":"%s"}\n' "$code" "$stage" > report-a-results/run-status.json
  python3 - <<'PY'
from pathlib import Path
for path in sorted(Path('report-a-results').glob('*.json')):
    print('MINIVEC_REPORT_A_BEGIN '+path.name)
    print(path.read_text())
    print('MINIVEC_REPORT_A_END')
PY
  exit "$code"
}
trap finish EXIT
git archive "$baseline" | tar -x -C "$scratch/baseline"
compiler=$(awk '/^FROM .* AS build$/ {print $2; exit}' Dockerfile)
test "$compiler" = 'gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
docker pull "$compiler"
for side in baseline posting_only batch_only combined; do
  stage="build-$side"
  source="$repo"
  flags=()
  case "$side" in
    baseline) source="$scratch/baseline" ;;
    posting_only) flags=(-DMVDB_DISABLE_PAYLOAD_BATCH) ;;
    batch_only) flags=(-DMVDB_DISABLE_MULTI_POSTING) ;;
  esac
  docker run --rm --network none --platform linux/amd64 \
    --mount "type=bind,source=$source,target=/src,readonly" \
    --mount "type=bind,source=$scratch,target=/out" --workdir /src/minivec \
    --entrypoint /bin/bash "$compiler" -c \
    'g++ -O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc -Ithird_party "${@:2}" src/*.cpp -o "/out/$1.bin"' \
    _ "$side" "${flags[@]}"
  sha256sum "$scratch/$side.bin" > "report-a-results/$side-sha256.txt"
done
stage=correctness
# Dedicated path tests also run on disabled-feature and forced-disk fallbacks.
for flags in '-DMVDB_DISABLE_MULTI_POSTING' '-DMVDB_FLAT_INDEX_MEMORY_BYTES=1 -DMVDB_SPILL_BYTES=1'; do
  g++ -O2 -std=c++17 -pthread -Iminivec/src $flags minivec/tests/test_filter_paths.cpp \
    minivec/src/db.cpp minivec/src/exec.cpp minivec/src/sql.cpp minivec/src/value.cpp minivec/src/flat_index.cpp \
    -o "$scratch/filter-fallback"
  "$scratch/filter-fallback"
done
g++ -O2 -std=c++17 -pthread -Iminivec/src -DMVDB_DISABLE_PAYLOAD_BATCH -DMVDB_TEST_WRAP_PWRITE \
  minivec/tests/test_payload_batch.cpp minivec/src/db.cpp minivec/src/flat_index.cpp minivec/src/value.cpp \
  -Wl,--wrap=pwrite -o "$scratch/payload-fallback"
"$scratch/payload-fallback"
for side in posting_only batch_only combined; do
  python3 minivec/tests/json_protocol.py --binary "$scratch/$side.bin" --timeout 15
  MVDB_QUERY_THREADS=1 python3 minivec/tests/topk_bounds.py --binary "$scratch/$side.bin"
  MVDB_QUERY_THREADS=2 python3 minivec/tests/topk_bounds.py --binary "$scratch/$side.bin"
done
stage=paired-measurements
export REPORT_A_SCRATCH="$scratch" REPORT_A_BASELINE="$baseline"
python3 - <<'PY'
import hashlib, json, os, platform, subprocess
from pathlib import Path
scratch=Path(os.environ['REPORT_A_SCRATCH'])
cpus=sorted(os.sched_getaffinity(0))[:2]
if len(cpus)!=2: raise SystemExit('need two allowed CPUs')
os.sched_setaffinity(0,cpus)
context={'baseline_commit':os.environ['REPORT_A_BASELINE'],
 'candidate_commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
 'cpu_affinity':cpus,'cpu_info':subprocess.check_output(['lscpu'],text=True),
 'compiler':'pinned GCC13.3.0 O3 static; PGO disabled equally on every side for factor isolation',
 'binaries':{name:hashlib.sha256((scratch/(name+'.bin')).read_bytes()).hexdigest()
             for name in ['baseline','posting_only','batch_only','combined']},
 'limitations':['Screening plus paired standard validation, not official scoring.',
 'Current release uses PGO; this experiment isolates source changes without PGO.',
 'No omitted negative samples; shared cache and process proxies remain.']}
Path('report-a-results/context.json').write_text(json.dumps(context,indent=2)+'\n')
def run(harness,side,name,extra):
 subprocess.run(['python3','minivec/tools/'+harness,'--baseline',str(scratch/'baseline.bin'),
  '--candidate',str(scratch/(side+'.bin')),'--output','report-a-results/'+name+'.json']+extra,check=True)
run('benchmark_iteration.py','combined','standard',
 ['--suite','full','--scenario','standard10k','--queries','1000','--pairs','5','--seeds','7319','8911','13337'])
run('benchmark_report_matrix.py','combined','matrix',
 ['--queries','32','--pairs','1','--seeds','7319','8911','13337'])
run('benchmark_report_matrix.py','posting_only','posting-ablation',
 ['--scenario','compound','--scenario','alternate_columns','--scenario','range',
  '--queries','64','--pairs','1','--seeds','7319','8911','13337'])
run('benchmark_iteration.py','batch_only','batch-ablation',
 ['--suite','full','--scenario','spill','--queries','64','--pairs','1','--seeds','7319','8911','13337'])
after={name:hashlib.sha256((scratch/(name+'.bin')).read_bytes()).hexdigest() for name in context['binaries']}
if after!=context['binaries']: raise SystemExit('binary changed during measurement')
Path('report-a-results/summary.json').write_text(json.dumps({'status':'passed','binary_unchanged':True,
 'standard_pairs':15,'matrix_pairs':39,'posting_ablation_pairs':9,'batch_ablation_pairs':3,
 'total_pairs':66,'official_score':None},indent=2)+'\n')
PY
stage=completed
