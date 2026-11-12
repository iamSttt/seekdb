#!/usr/bin/env bash
# Default-off I11 admission experiment. No production Dockerfile changes.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ "${1:-}" = --container ]; then
  cd "$repo/minivec"
  common=(-O3 -std=c++17 -Wall -Wextra -fno-math-errno -ffunction-sections
    -fdata-sections -Wl,--gc-sections -static -pthread -Isrc -Ithird_party)
  mkdir -p /out/binaries
  for side in base broad selective; do
    flags=()
    case "$side" in
      broad) flags=(-DMVDB_ENABLE_BLOCK_QUANT_SCALE);;
      selective) flags=(-DMVDB_ENABLE_BLOCK_QUANT_SCALE -DMVDB_BLOCK_SCALE_SELECTIVE);;
    esac
    mkdir -p "/build/$side"
    printf '%s\n' "build-$side" > /out/current-stage.txt
    for source in db exec sql value flat_index; do
      g++ "${common[@]}" "${flags[@]}" -c "src/$source.cpp" -o "/build/$side/$source.o"
    done
    g++ "${common[@]}" "${flags[@]}" src/main.cpp /build/"$side"/{db,exec,sql,value,flat_index}.o -o "/out/binaries/$side.bin"
    for test in quantized_bound selective_scale filter_paths index_budget; do
      extra=()
      case "$test" in
        selective_scale) extra=("/build/$side/flat_index.o");;
        filter_paths) extra=(/build/"$side"/{db,exec,sql,value,flat_index}.o);;
        index_budget) extra=("/build/$side/flat_index.o" -DMVDB_TEST_WRAP_PWRITE -Wl,--wrap=pwrite);;
      esac
      printf '%s\n' "$side-test-$test" > /out/current-stage.txt
      g++ "${common[@]}" "${flags[@]}" "tests/test_$test.cpp" "${extra[@]}" -o "/build/$side/test-$test"
      "/build/$side/test-$test" | tee "/out/$side-test-$test.log"
      printf '%s\t%s\n' "$side" "$test" >> /out/passed.tsv
    done
    if [ "$side" != selective ]; then
      # Existing unrestricted-scale failure/cache tests keep their original
      # expected admissions; selective has the new boundary/strict-bound test.
      g++ "${common[@]}" "${flags[@]}" tests/test_quantized_scales.cpp "/build/$side/flat_index.o" -o "/build/$side/original-scales"
      "/build/$side/original-scales" | tee "/out/$side-original-scales.log"
      printf '%s\t%s\n' "$side" original_quantized_scales >> /out/passed.tsv
    fi
    g++ "${common[@]}" "${flags[@]}" tools/bench_selective_scales.cpp "/build/$side/flat_index.o" -o "/out/binaries/$side-micro.bin"
  done
  # Defining SELECTIVE without the parent experiment must be inert, including
  # class layout: link against the unmodified macro-off flat-index object.
  g++ "${common[@]}" -DMVDB_BLOCK_SCALE_SELECTIVE tests/test_selective_scale.cpp /build/base/flat_index.o -o /build/inert-selective
  /build/inert-selective | tee /out/inert-selective.log
  printf '%s\t%s\n' inert selective_scale >> /out/passed.tsv
  # The exact arithmetic bound must still compose with I10 coordinate blocking.
  g++ "${common[@]}" -DMVDB_ENABLE_BLOCK_QUANT_SCALE -DMVDB_BLOCK_SCALE_SELECTIVE -DMVDB_QUANTIZED_BLOCK_SIZE=64 \
    tests/test_selective_scale.cpp src/flat_index.cpp -o /build/blocked-selective
  /build/blocked-selective | tee /out/blocked-selective.log
  printf '%s\t%s\n' block64 selective_scale >> /out/passed.tsv
  exit 0
fi

cd "$repo"
out="$repo/selective-scale-results"
if [ "${1:-}" != --run ]; then
  test ! -e "$out"
  mkdir -p "$out"
  touch "$out/passed.tsv"
  printf '%s\n' setup > "$out/current-stage.txt"
  finish() {
    code=$?; trap - EXIT; set +e
    python3 - "$out" "$code" <<'PY'
import json,sys
from pathlib import Path
out=Path(sys.argv[1])
passed=[dict(zip(('variant','test'),line.split('\t'))) for line in (out/'passed.tsv').read_text().splitlines()]
report={'exit_code':int(sys.argv[2]),'stage':(out/'current-stage.txt').read_text().strip(),
        'passed_tests':passed,'official_score':None,'adopted':False}
(out/'run-status.json').write_text(json.dumps(report,indent=2)+'\n')
for path in sorted(out.glob('*.json')):
    print('MINIVEC_SELECTIVE_SCALE_BEGIN '+path.name)
    print(path.read_text())
    print('MINIVEC_SELECTIVE_SCALE_END')
PY
    report_code=$?
    if [ "$code" -eq 0 ] && [ "$report_code" -ne 0 ]; then code=$report_code; fi
    if [ "$report_code" -ne 0 ]; then
      printf '{"exit_code":%s,"report_collection_exit_code":%s}\n' "$code" "$report_code" > "$out/collection-error.json"
    fi
    exit "$code"
  }
  trap finish EXIT
  # A 60-minute CI job retains five minutes for collection and always-upload.
  timeout --signal=TERM --kill-after=30s 3300 bash "${BASH_SOURCE[0]}" --run
  exit 0
fi

build=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-selective-scale-XXXXXX")
compiler=$(awk '/^FROM .* AS build$/ {print $2; exit}' Dockerfile)
test "$compiler" = 'gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
git rev-parse HEAD > "$out/source-commit.txt"
git status --porcelain --untracked-files=no > "$out/worktree-status.txt"
find minivec/src minivec/third_party -type f -print0 | sort -z | xargs -0 sha256sum > "$out/source-sha256.txt"
sha256sum minivec/tools/{ci_selective_scale.sh,benchmark_selective_scale.py,benchmark_iteration.py,bench_selective_scales.cpp} \
  minivec/tests/test_selective_scale.cpp minivec/tests/test_selective_workload.py > "$out/harness-sha256.txt"
python3 minivec/tests/test_selective_workload.py 2>&1 | tee "$out/python-oracle.log"
printf '%s\t%s\n' python selective_workload >> "$out/passed.tsv"
docker pull "$compiler"
docker run --rm --network none --platform linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$repo,target=/src,readonly" --mount "type=bind,source=$out,target=/out" \
  --mount "type=bind,source=$build,target=/build" --workdir /src --entrypoint /bin/bash \
  "$compiler" /src/minivec/tools/ci_selective_scale.sh --container 2>&1 | tee "$out/build-unit.log"
for side in base broad selective; do
  printf '%s\n' "$side-protocol" > "$out/current-stage.txt"
  python3 minivec/tests/json_protocol.py --binary "$out/binaries/$side.bin" --timeout 15 | tee "$out/$side-json-protocol.log"
  printf '%s\t%s\n' "$side" json_protocol >> "$out/passed.tsv"
  for threads in 1 2; do
    MVDB_QUERY_THREADS="$threads" python3 minivec/tests/topk_bounds.py --binary "$out/binaries/$side.bin" | tee "$out/$side-topk-$threads.log"
    printf '%s\t%s\n' "$side" "topk-$threads" >> "$out/passed.tsv"
  done
done
printf '%s\n' sanitizer > "$out/current-stage.txt"
for test in quantized_bound selective_scale; do
  extra=()
  [ "$test" != selective_scale ] || extra=(minivec/src/flat_index.cpp)
  g++ -O1 -g -std=c++17 -pthread -fno-omit-frame-pointer -fno-pie -no-pie -fsanitize=address,undefined \
    -DMVDB_DISABLE_AVX512_QUANTIZED -DMVDB_ENABLE_BLOCK_QUANT_SCALE -DMVDB_BLOCK_SCALE_SELECTIVE \
    -DMVDB_QUANTIZED_BLOCK_SIZE=64 -Iminivec/src "minivec/tests/test_$test.cpp" "${extra[@]}" -o "$build/san-$test"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build/san-$test" | tee "$out/san-$test.log"
  printf '%s\t%s\n' sanitizer "$test" >> "$out/passed.tsv"
done
printf '%s\n' microdiagnostics > "$out/current-stage.txt"
python3 - "$out" <<'PY'
import hashlib,json,os,platform,subprocess,sys
from pathlib import Path
out=Path(sys.argv[1]);bins=out/'binaries'
cpus=sorted(os.sched_getaffinity(0))[:2]
if len(cpus)!=2:raise SystemExit('two allowed CPUs required')
os.sched_setaffinity(0,cpus)
hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(bins.glob('*.bin'))}
context={'source_commit':(out/'source-commit.txt').read_text().strip(),'binaries':hashes,'affinity':cpus,
 'compiler':'pinned GCC13.3 O3 static; same sources; only I11 ENABLE/SELECTIVE macros differ; no PGO',
 'variants':{'base':[],'broad':['MVDB_ENABLE_BLOCK_QUANT_SCALE'],
             'selective':['MVDB_ENABLE_BLOCK_QUANT_SCALE','MVDB_BLOCK_SCALE_SELECTIVE']},
 'platform':platform.platform(),'sanitizer_compiler':subprocess.check_output(['g++','--version'],text=True).splitlines()[0],
 'default_adoption':False,'official_score':None,
 'gate':'dimension>=128 and >=80% actual rows at least 4x finer; unchanged memory/cache caps'}
(out/'context.json').write_text(json.dumps(context,indent=2)+'\n')
records=[]
for repeat in range(3):
    group={}
    for side in (('base','broad','selective') if repeat%2==0 else ('selective','broad','base')):
        raw=subprocess.check_output([str(bins/(side+'-micro.bin')),'20000','128','20'],text=True)
        (out/f'micro-{repeat}-{side}.jsonl').write_text(raw)
        group[side]=[json.loads(line) for line in raw.splitlines()]
        records.append({'repeat':repeat,'side':side,'rows':group[side]})
    assert [row['checksum'] for row in group['base']]==[row['checksum'] for row in group['broad']]==[row['checksum'] for row in group['selective']]
(out/'microdiagnostics.json').write_text(json.dumps({'raw':records,'official_score':None,
 'limit':'single-thread diagnostic only; includes production float prefilter and fast canonical key; no protocol or worker split'},indent=2)+'\n')
(out/'current-stage.txt').write_text('aa-ingest-control')
subprocess.run(['python3','minivec/tools/benchmark_selective_scale.py','--baseline',str(bins/'base.bin'),
 '--candidate',str(bins/'base.bin'),'--output',str(out/'aa-control.json'),'--scenario','standard10k',
 '--pairs','1','--queries','8','--seeds','7319','8911','13337'],check=True)
(out/'current-stage.txt').write_text('paired-whole-requests')
subprocess.run(['python3','minivec/tools/benchmark_selective_scale.py','--baseline',str(bins/'base.bin'),
 '--candidate',str(bins/'selective.bin'),'--output',str(out/'paired.json'),'--pairs','5','--queries','64',
 '--seeds','7319','8911','13337'],check=True)
paired=json.loads((out/'paired.json').read_text())
expected={(scenario,seed,pair) for scenario in paired['parameters']['scenarios']
          for seed in [7319,8911,13337] for pair in range(5)}
assert paired['status']=='passed' and len(paired['pairs'])==len(expected)==105
assert {(p['scenario'],p['seed'],p['pair_index']) for p in paired['pairs']}==expected
assert hashes=={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(bins.glob('*.bin'))}
(out/'summary.json').write_text(json.dumps({'status':'passed','paired_count':len(expected),
 'aa_pairs':3,'micro_repeats':3,'binary_unchanged':True,'default_adoption':False,'official_score':None})+'\n')
PY
printf '%s\n' completed > "$out/current-stage.txt"
