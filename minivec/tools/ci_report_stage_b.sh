#!/usr/bin/env bash
# I09/I10/I12/I14 isolation. Default source path remains the reference.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="$repo/report-b-results"
test ! -e "$out"
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-report-b-XXXXXX")
stage=build
finish() {
  code=$?; trap - EXIT; set +e
  printf '{"exit_code":%d,"stage":"%s"}\n' "$code" "$stage" > "$out/run-status.json"
  python3 - "$out" <<'PY'
import sys
from pathlib import Path
for p in sorted(Path(sys.argv[1]).glob('*.json')):
    print('MINIVEC_REPORT_B_BEGIN '+p.name)
    print(p.read_text())
    print('MINIVEC_REPORT_B_END')
PY
  exit "$code"
}
trap finish EXIT
compiler=$(awk '/^FROM .* AS build$/ {print $2; exit}' Dockerfile)
test "$compiler" = gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818
docker pull "$compiler"
for side in base adaptive typed block16 block32 block64 pilot32 pilot64 pilot128; do
  flags=()
  case "$side" in
    adaptive) flags=(-DMVDB_ENABLE_WORKLOAD_FANOUT);;
    typed) flags=(-DMVDB_ENABLE_TYPED_JSON);;
    block*) flags=(-DMVDB_QUANTIZED_BLOCK_SIZE="${side#block}");;
    pilot*) flags=(-DMVDB_PILOT_COUNT="${side#pilot}");;
  esac
  stage="build-$side"
  docker run --rm --network none --platform linux/amd64 \
    --mount "type=bind,source=$repo,target=/src,readonly" \
    --mount "type=bind,source=$scratch,target=/out" --workdir /src/minivec \
    --entrypoint /bin/bash "$compiler" -c \
    'g++ -O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc -Ithird_party "${@:2}" src/*.cpp -o "/out/$1.bin"' \
    _ "$side" "${flags[@]}"
done
stage=correctness
g++ -O2 -std=c++17 -Wall -Wextra -pthread -Iminivec/src minivec/tests/test_query_cost.cpp -o "$scratch/query-cost"
"$scratch/query-cost"
g++ -O2 -std=c++17 -Wall -Wextra -pthread -Iminivec/src minivec/tests/test_json_row_writer.cpp -o "$scratch/typed-json"
"$scratch/typed-json"
for block in 0 16 32 64; do
  g++ -O2 -std=c++17 -pthread -Iminivec/src -DMVDB_QUANTIZED_BLOCK_SIZE="$block" \
    minivec/tests/test_quantized_bound.cpp -o "$scratch/bound-$block"
  "$scratch/bound-$block"
done
for test in query_cost json_row_writer quantized_bound; do
  g++ -O1 -g -std=c++17 -pthread -fno-omit-frame-pointer -fno-pie -no-pie \
    -fsanitize=address,undefined -DMVDB_DISABLE_AVX512_QUANTIZED -DMVDB_QUANTIZED_BLOCK_SIZE=32 \
    -Iminivec/src "minivec/tests/test_$test.cpp" -o "$scratch/san-$test"
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/san-$test"
done
for side in adaptive typed block16 block32 block64 pilot32 pilot64 pilot128; do
  python3 minivec/tests/json_protocol.py --binary "$scratch/$side.bin" --timeout 15
  MVDB_QUERY_THREADS=1 python3 minivec/tests/topk_bounds.py --binary "$scratch/$side.bin"
  MVDB_QUERY_THREADS=2 python3 minivec/tests/topk_bounds.py --binary "$scratch/$side.bin"
done
stage=paired-screen
python3 - "$scratch" "$out" <<'PY'
import hashlib,json,os,subprocess,sys
from pathlib import Path
scratch,out=map(Path,sys.argv[1:])
cpus=sorted(os.sched_getaffinity(0))[:2]
if len(cpus)!=2:raise SystemExit('two CPUs required')
os.sched_setaffinity(0,cpus)
names=['base','adaptive','typed','block16','block32','block64','pilot32','pilot64','pilot128']
context={'commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
 'compiler':'fixed GCC13.3 O3 static; no PGO on either side', 'affinity':cpus,
 'binaries':{n:hashlib.sha256((scratch/(n+'.bin')).read_bytes()).hexdigest() for n in names},
 'scope':'3-pair screening, not adoption or official scoring', 'official_score':None}
(out/'context.json').write_text(json.dumps(context,indent=2)+'\n')
for side in names[1:]:
 for harness,label,extra in [
  ('benchmark_iteration.py','standard',['--suite','full','--scenario','standard10k']),
  ('benchmark_report_matrix.py','boundaries',['--scenario','high768_k1','--scenario','high768_k100','--scenario','large100k'])]:
  subprocess.run(['python3','minivec/tools/'+harness,'--baseline',str(scratch/'base.bin'),
   '--candidate',str(scratch/(side+'.bin')),'--output',str(out/(side+'-'+label+'.json')),
   '--queries','64','--pairs','1','--seeds','7319','8911','13337']+extra,check=True)
after={n:hashlib.sha256((scratch/(n+'.bin')).read_bytes()).hexdigest() for n in names}
assert context['binaries']==after
(out/'summary.json').write_text(json.dumps({'status':'passed','pairs':96,'binary_unchanged':True,
 'official_score':None,'limit':'screen only; wins require repeated confirmation'})+'\n')
PY
stage=cgroup-diagnostics
gcc -O2 -Wall -Wextra -static minivec/tools/cgroup_exec.c -o "$scratch/cgroup-exec"
# This privileged measurement runs only on the disposable CI runner. The
# launcher joins a newly created bounded group before executing the database.
sudo python3 minivec/tools/benchmark_cgroup.py --baseline "$scratch/base.bin" \
  --candidate "$scratch/adaptive.bin" --launcher "$scratch/cgroup-exec" \
  --output "$out/cgroup-adaptive.json" --pairs 1 --queries 64 --seeds 7319 \
  --scenario standard10k --scenario high768_k100
stage=completed
