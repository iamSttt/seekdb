#!/usr/bin/env bash
# I11 isolated screening. Default and candidate share sources/compiler/flags;
# only MVDB_ENABLE_BLOCK_QUANT_SCALE differs. No adoption or score is implied.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

if [[ "${1:-}" == --container ]]; then
  cd "$repo/minivec"
  mkdir -p /out/binaries
  common=(-O3 -std=c++17 -Wall -Wextra -fno-math-errno -ffunction-sections
    -fdata-sections -Wl,--gc-sections -static -pthread -Isrc -Ithird_party)
  for side in base scale; do
    flags=()
    [[ "$side" == scale ]] && flags=(-DMVDB_ENABLE_BLOCK_QUANT_SCALE)
    mkdir -p "/build/$side"
    printf '%s\n' "$side/compile" > /out/current-stage.txt
    for source in db exec sql value flat_index; do
      g++ "${common[@]}" "${flags[@]}" -c "src/$source.cpp" -o "/build/$side/$source.o"
    done
    g++ "${common[@]}" "${flags[@]}" src/main.cpp /build/"$side"/{db,exec,sql,value,flat_index}.o \
      -o "/out/binaries/$side.bin"
    for test in quantized_bound quantized_scales index_budget filter_paths; do
      extra=()
      case "$test" in
        quantized_scales) extra=("/build/$side/flat_index.o");;
        index_budget) extra=("/build/$side/flat_index.o" -DMVDB_TEST_WRAP_PWRITE -Wl,--wrap=pwrite);;
        filter_paths) extra=(/build/"$side"/{db,exec,sql,value,flat_index}.o);;
      esac
      printf '%s\n' "$side/test_$test" > /out/current-stage.txt
      g++ "${common[@]}" "${flags[@]}" "tests/test_$test.cpp" "${extra[@]}" \
        -o "/build/$side/test_$test"
      "/build/$side/test_$test" | tee "/out/$side-test_$test.log"
      printf '%s\t%s\n' "$side" "test_$test" >> /out/passed.tsv
    done
    "/build/$side/test_index_budget" --ann-budget | tee "/out/$side-test_index_ann.log"
    printf '%s\t%s\n' "$side" test_index_budget_ann >> /out/passed.tsv
    g++ "${common[@]}" "${flags[@]}" tools/bench_quantized_scales.cpp \
      "/build/$side/flat_index.o" -o "/out/binaries/$side-diagnostic.bin"
  done
  # I10 and I11 can compose. Retain both the prior single-scale bound test and
  # the new ordinal/cache/index-budget tests with the coordinate early exit.
  for test in quantized_bound quantized_scales; do
    extra=()
    [[ "$test" == quantized_scales ]] && extra=(src/flat_index.cpp)
    printf '%s\n' "scale-block64/test_$test" > /out/current-stage.txt
    g++ "${common[@]}" -DMVDB_ENABLE_BLOCK_QUANT_SCALE -DMVDB_QUANTIZED_BLOCK_SIZE=64 \
      "tests/test_$test.cpp" "${extra[@]}" -o "/build/block64-$test"
    "/build/block64-$test" | tee "/out/scale-block64-test_$test.log"
    printf '%s\t%s\n' scale_block64 "test_$test" >> /out/passed.tsv
  done
  exit 0
fi

cd "$repo"
out="$repo/quant-scale-results"
test ! -e "$out"
mkdir -p "$out"
build=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-quant-scale-XXXXXX")
touch "$out/passed.tsv"
printf '%s\n' setup > "$out/current-stage.txt"
finish() {
  code=$?
  trap - EXIT
  set +e
  python3 - "$out" "$code" <<'PY'
import json,sys
from pathlib import Path
out=Path(sys.argv[1])
passed=[dict(zip(('configuration','test'),line.split('\t'))) for line in
        (out/'passed.tsv').read_text().splitlines()]
status={'exit_code':int(sys.argv[2]),'stage':(out/'current-stage.txt').read_text().strip(),
        'passed':passed,'official_score':None,'adoption':False}
(out/'run-status.json').write_text(json.dumps(status,indent=2)+'\n')
for p in sorted(out.glob('*.json')):
    print('MINIVEC_QUANT_SCALE_BEGIN '+p.name)
    print(p.read_text())
    print('MINIVEC_QUANT_SCALE_END')
PY
  exit "$code"
}
trap finish EXIT
compiler=$(awk '/^FROM .* AS build$/ {print $2; exit}' Dockerfile)
test "$compiler" = 'gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
docker pull "$compiler"
docker run --rm --network none --platform linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$repo,target=/src,readonly" \
  --mount "type=bind,source=$out,target=/out" \
  --mount "type=bind,source=$build,target=/build" --workdir /src \
  --entrypoint /bin/bash "$compiler" /src/minivec/tools/ci_quant_scale.sh --container \
  2>&1 | tee "$out/build-unit.log"

for side in base scale; do
  printf '%s\n' "$side/json_protocol" > "$out/current-stage.txt"
  python3 minivec/tests/json_protocol.py --binary "$out/binaries/$side.bin" --timeout 15 \
    | tee "$out/$side-json_protocol.log"
  printf '%s\t%s\n' "$side" json_protocol >> "$out/passed.tsv"
  for threads in 1 2; do
    printf '%s\n' "$side/topk_threads_$threads" > "$out/current-stage.txt"
    MVDB_QUERY_THREADS="$threads" python3 minivec/tests/topk_bounds.py --binary "$out/binaries/$side.bin" \
      | tee "$out/$side-topk_threads_$threads.log"
    printf '%s\t%s\n' "$side" "topk_threads_$threads" >> "$out/passed.tsv"
  done
done

# Sanitizers use the host runtime, recorded separately from fixed-GCC timings.
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
for test in quantized_bound quantized_scales; do
  printf '%s\n' "sanitizer/test_$test" > "$out/current-stage.txt"
  extra=()
  [[ "$test" == quantized_scales ]] && extra=(minivec/src/flat_index.cpp)
  g++ -O1 -g -std=c++17 -pthread -fno-omit-frame-pointer -fno-pie -no-pie \
    -fsanitize=address,undefined -DMVDB_DISABLE_AVX512_QUANTIZED \
    -DMVDB_ENABLE_BLOCK_QUANT_SCALE -DMVDB_QUANTIZED_BLOCK_SIZE=64 \
    -Iminivec/src "minivec/tests/test_$test.cpp" "${extra[@]}" -o "$build/san-$test"
  "$build/san-$test" | tee "$out/sanitizer-test_$test.log"
  printf '%s\t%s\n' sanitizer "test_$test" >> "$out/passed.tsv"
done
unset ASAN_OPTIONS UBSAN_OPTIONS

printf '%s\n' diagnostics_and_paired_screen > "$out/current-stage.txt"
python3 - "$out" <<'PY'
import hashlib,json,os,platform,subprocess,sys
from pathlib import Path
out=Path(sys.argv[1]);bins=out/'binaries'
cpus=sorted(os.sched_getaffinity(0))[:2]
if len(cpus)!=2:raise SystemExit('two CPUs required')
os.sched_setaffinity(0,cpus)
hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(bins.glob('*.bin'))}
context={'commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
 'compiler':'pinned GCC13.3 O3 static; only I11 macro differs; no PGO',
 'sanitizer_compiler':subprocess.check_output(['g++','--version'],text=True).splitlines()[0],
 'affinity':cpus,'platform':platform.platform(),'binaries':hashes,
 'scope':'screen only, not adoption or official scoring','official_score':None}
(out/'context.json').write_text(json.dumps(context,indent=2)+'\n')
diagnostics=[]
for repeat in range(3):
    pair={}
    for side in (('base','scale') if repeat%2==0 else ('scale','base')):
        raw=subprocess.check_output([str(bins/(side+'-diagnostic.bin')),'20000','128','20'],text=True)
        (out/f'diagnostic-{repeat}-{side}.jsonl').write_text(raw)
        parsed=[json.loads(line) for line in raw.splitlines() if line.strip()]
        pair[side]=parsed
        diagnostics.append({'repeat':repeat,'side':side,'rows':parsed})
    assert [r['family'] for r in pair['base']]==[r['family'] for r in pair['scale']]
    assert [r['checksum'] for r in pair['base']]==[r['checksum'] for r in pair['scale']]
(out/'diagnostics.json').write_text(json.dumps({'raw':diagnostics,
 'limit':'index microdiagnostics exclude JSON/protocol/float prefilter; whole-request screens follow',
 'official_score':None},indent=2)+'\n')
for harness,label,extra in [
 ('benchmark_iteration.py','standard',['--suite','full','--scenario','standard10k']),
 ('benchmark_report_matrix.py','boundaries',['--scenario','high768_k1','--scenario','high768_k100',
                                            '--scenario','large100k'])]:
    subprocess.run(['python3','minivec/tools/'+harness,'--baseline',str(bins/'base.bin'),
     '--candidate',str(bins/'scale.bin'),'--output',str(out/(label+'.json')),
     '--queries','64','--pairs','1','--seeds','7319','8911','13337']+extra,check=True)
assert hashes=={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(bins.glob('*.bin'))}
(out/'summary.json').write_text(json.dumps({'status':'passed','whole_request_pairs':12,
 'synthetic_pairs':3,'synthetic_distributions':4,'binary_unchanged':True,'official_score':None,
 'limit':'screen only; measure larger held-out sweeps before default adoption'},indent=2)+'\n')
PY
printf '%s\n' completed > "$out/current-stage.txt"
