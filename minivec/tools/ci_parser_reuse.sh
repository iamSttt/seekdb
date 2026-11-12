#!/usr/bin/env bash
# I13 owning-token reuse only. No borrowed tokens or grammar template cache.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

if [[ "${1:-}" == --container ]]; then
  cd /src/minivec
  common=(-O3 -std=c++17 -static -pthread -fno-math-errno -Isrc -Ithird_party)
  for side in baseline candidate tiny_cache; do
    source=/build/baseline-sql.cpp
    flags=()
    if [[ "$side" != baseline ]]; then source=src/sql.cpp;flags+=(-DMVDB_ENABLE_TOKEN_REUSE);fi
    [[ "$side" == tiny_cache ]] && flags+=(-DMVDB_TOKEN_REUSE_BYTES=1)
    printf '%s\n' "$side/build-and-correctness" > /out/current-stage.txt
    g++ "${common[@]}" "${flags[@]}" -c "$source" -o "/build/$side-sql.o"
    for test in sql parser_reuse; do
      g++ "${common[@]}" "${flags[@]}" "tests/test_$test.cpp" "/build/$side-sql.o" src/value.cpp -o "/build/$side-$test"
      for threads in 1 2; do
        MVDB_PARSE_THREADS="$threads" "/build/$side-$test"
        printf '%s\t%s\t%s\n' "$side" "$test" "$threads" >> /out/passed.tsv
      done
    done
    if [[ "$side" != tiny_cache ]]; then
      for mode in timing allocations; do
        meter=()
        [[ "$mode" == allocations ]] && meter=(-DMVDB_METER_PARSER_ALLOCATIONS)
        g++ "${common[@]}" "${flags[@]}" "${meter[@]}" tools/bench_parser_reuse.cpp \
          "/build/$side-sql.o" src/value.cpp -o "/out/binaries/$side-$mode"
      done
      g++ "${common[@]}" "${flags[@]}" src/main.cpp src/db.cpp src/exec.cpp src/flat_index.cpp \
        src/value.cpp "/build/$side-sql.o" -o "/out/binaries/$side.bin"
    fi
  done
  exit 0
fi

cd "$repo"
out="$repo/parser-reuse-results"
test ! -e "$out"
mkdir -p "$out/binaries"
build=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-parser-reuse-XXXXXX")
# This commit's SQL parser predates this candidate. Only sql.cpp differs in the
# baseline build; all other source is the same current checkout on both sides.
baseline=9aa18865c85bd74578f9e408438dcd29e7247728
touch "$out/passed.tsv"
printf '%s\n' setup > "$out/current-stage.txt"
finish() {
  local code=$?
  trap - EXIT
  set +e
  PARSER_REPORT_CODE="$code" PARSER_REPORT_OUT="$out" python3 - <<'PY'
import json,os
from pathlib import Path
out=Path(os.environ['PARSER_REPORT_OUT'])
checks=[dict(zip(('variant','test','parse_threads'),line.split('\t')))
        for line in (out/'passed.tsv').read_text().splitlines()]
summary={'exit_code':int(os.environ['PARSER_REPORT_CODE']),
         'stage':(out/'current-stage.txt').read_text().strip(),'passed':checks,
         'adopted':False,'official_score':None}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
for path in sorted(out.glob('*.json')):
    print('MINIVEC_PARSER_BEGIN '+path.name)
    print(path.read_text())
    print('MINIVEC_PARSER_END')
PY
  exit "$code"
}
trap finish EXIT
git show "$baseline:minivec/src/sql.cpp" > "$build/baseline-sql.cpp"
compiler=$(awk '/^FROM .* AS build$/ {print $2; exit}' Dockerfile)
test "$compiler" = 'gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
docker pull "$compiler"
docker run --rm --network none --platform linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$repo,target=/src,readonly" \
  --mount "type=bind,source=$out,target=/out" \
  --mount "type=bind,source=$build,target=/build" --workdir /src \
  --entrypoint /bin/bash "$compiler" /src/minivec/tools/ci_parser_reuse.sh --container

printf '%s\n' sanitizer > "$out/current-stage.txt"
san=(-O1 -g -std=c++17 -pthread -fno-omit-frame-pointer -no-pie -fsanitize=address,undefined
     -DMVDB_ENABLE_TOKEN_REUSE -Iminivec/src)
g++ "${san[@]}" -c minivec/src/sql.cpp -o "$build/sanitizer-sql.o"
for test in sql parser_reuse; do
  g++ "${san[@]}" "minivec/tests/test_$test.cpp" "$build/sanitizer-sql.o" minivec/src/value.cpp -o "$build/$test-san"
  for threads in 1 2; do
    MVDB_PARSE_THREADS="$threads" ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$build/$test-san"
    printf '%s\t%s\t%s\n' sanitizer "$test" "$threads" >> "$out/passed.tsv"
  done
done
for side in baseline candidate; do
  printf '%s\n' "$side/protocol" > "$out/current-stage.txt"
  python3 minivec/tests/json_protocol.py --binary "$out/binaries/$side.bin" --timeout 15
  printf '%s\t%s\t%s\n' "$side" json_protocol default >> "$out/passed.tsv"
done

printf '%s\n' paired-parser-microbenchmarks > "$out/current-stage.txt"
export PARSER_BASELINE="$baseline" PARSER_BUILD="$build"
python3 - <<'PY'
import hashlib,json,os,platform,statistics,subprocess
from pathlib import Path
out=Path('parser-reuse-results')
cpus=sorted(os.sched_getaffinity(0))[:2]
if len(cpus)<2: raise SystemExit('need two allowed CPUs')
context={'baseline_sql_commit':os.environ['PARSER_BASELINE'],
 'candidate_commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
 'compiler':'pinned GCC13.3 O3 static, PGO off equally',
 'baseline_scope':'Historical sql.cpp only; all remaining source is identical current checkout.',
 'baseline_sql_sha256':hashlib.sha256((Path(os.environ['PARSER_BUILD'])/'baseline-sql.cpp').read_bytes()).hexdigest(),
 'candidate_sql_sha256':hashlib.sha256(Path('minivec/src/sql.cpp').read_bytes()).hexdigest(),
 'cpu_affinity':cpus,'platform':platform.platform(),
 'binary_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((out/'binaries').iterdir())},
 'limitations':['Parser microbenchmark, not end-to-end database or official score.',
   'Timing executables do not contain allocation hooks; counters use separate executables.',
   'new/delete counters exclude internal libc malloc calls and allocator headers.',
   '256 KiB retained scratch per calling thread; transient parse/AST memory is not capped.',
   'Worker tests use one ParsePool caller; concurrent SELECT tests prove TLS isolation, not concurrent batch INSERT safety.',
   'ASan leak detection disabled for the existing deliberately process-lived ParsePool.']}
(out/'context.json').write_text(json.dumps(context,indent=2)+'\n')
raw=[]
def run(side,mode,threads,divisor):
    os.sched_setaffinity(0,cpus[:threads])
    env=dict(os.environ,MVDB_PARSE_THREADS=str(threads))
    text=subprocess.check_output([str(out/'binaries'/(side+'-'+mode)),str(divisor)],env=env,text=True)
    rows=[json.loads(line) for line in text.splitlines()]
    if len(rows)!=6: raise RuntimeError('missing parser cases')
    return rows
for threads in (1,2):
    for pair in range(5):
        order=('baseline','candidate') if pair%2==0 else ('candidate','baseline')
        measured={side:run(side,'timing',threads,1) for side in order}
        for left,right in zip(measured['baseline'],measured['candidate']):
            if (left['case'],left['iterations'],left['checksum'])!=(right['case'],right['iterations'],right['checksum']):
                raise RuntimeError('baseline/candidate checksum mismatch')
            raw.append({'parse_threads':threads,'pair':pair,'order':order,
                        'baseline':left,'candidate':right})
counts=[]
for threads in (1,2):
    measurements={side:run(side,'allocations',threads,5) for side in ('baseline','candidate')}
    for left,right in zip(measurements['baseline'],measurements['candidate']):
        if (left['case'],left['checksum'])!=(right['case'],right['checksum']):
            raise RuntimeError('allocation-build checksum mismatch')
        counts.append({'parse_threads':threads,'baseline':left,'candidate':right,
                       'allocation_call_ratio':left['new_calls']/max(1,right['new_calls'])})
summary=[]
for threads in (1,2):
    for name in [item['baseline']['case'] for item in raw if item['parse_threads']==threads and item['pair']==0]:
        pairs=[item for item in raw if item['parse_threads']==threads and item['baseline']['case']==name]
        speedups=[item['baseline']['elapsed_ns']/item['candidate']['elapsed_ns'] for item in pairs]
        summary.append({'parse_threads':threads,'case':name,'pairs':len(pairs),
                        'speedups':speedups,'median_speedup':statistics.median(speedups)})
(out/'microbench.json').write_text(json.dumps({'timing_pairs':raw,'timing_summary':summary,
    'measured_allocations':counts,'automatic_adoption':False},indent=2)+'\n')
if context['binary_sha256']!={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((out/'binaries').iterdir())}:
    raise RuntimeError('binaries changed while measuring')
PY
printf '%s\n' completed > "$out/current-stage.txt"
