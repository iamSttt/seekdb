#!/usr/bin/env bash
# I06/I07 are opt-in. Compile each ABI/configuration once, then reuse objects.
# Correctness first, then isolated screening; no PGO/threshold change/score.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
case "${STORAGE_CORRECTNESS_ONLY:-0}" in 0|1) ;; *) echo 'STORAGE_CORRECTNESS_ONLY must be 0 or 1' >&2;exit 2;;esac

run_test() {
  local variant=$1 test_name=$2; shift 2
  local executable="$build/$variant/$test_name"
  printf '%s\n' "$variant/$test_name" > "$out/current-stage.txt"
  "$cxx" "${compile_flags[@]}" "${variant_flags[@]}" "$repo/minivec/tests/$test_name.cpp" \
    "$build/$variant/"{db,exec,sql,value,flat_index}.o "$@" -o "$executable"
  "$executable"
  printf '%s\t%s\n' "$variant" "$test_name" >> "$out/passed.tsv"
}
build_variant() {
  local variant=$1 mode=$2; shift 2
  variant_flags=("$@")
  mkdir -p "$build/$variant"
  printf '%s\n' "$variant/compile" > "$out/current-stage.txt"
  for source in db exec sql value flat_index; do
    "$cxx" "${compile_flags[@]}" "${variant_flags[@]}" -c "$repo/minivec/src/$source.cpp" \
      -o "$build/$variant/$source.o"
  done
  if [[ "$mode" == binary ]]; then
    "$cxx" "${compile_flags[@]}" "${variant_flags[@]}" "$repo/minivec/src/main.cpp" \
      "$build/$variant/"{db,exec,sql,value,flat_index}.o -o "$out/binaries/$variant.bin"
  fi
}

if [[ "${1:-}" == --container ]]; then
  out=/out
  build=/build
  cxx=g++
  compile_flags=(-O3 -std=c++17 -Wall -Wextra -fno-math-errno -ffunction-sections
    -fdata-sections -Wl,--gc-sections -static -pthread -I"$repo/minivec/src" -I"$repo/minivec/third_party")
  mkdir -p "$out/binaries"
  for variant in baseline delta spill both; do
    features=()
    [[ "$variant" == delta || "$variant" == both ]] && features+=(-DMVDB_ENABLE_INDEX_DELTA)
    [[ "$variant" == spill || "$variant" == both ]] && features+=(-DMVDB_ENABLE_SPILL_FILTER)
    build_variant "$variant" binary "${features[@]}"
    # No threshold override: real 1 MiB Undo / 64 MiB payload spill and ENOSPC.
    run_test "$variant" test_resident_update -DMVDB_TEST_WRAP_PWRITE -Wl,--wrap=pwrite
    if [[ "$variant" == delta || "$variant" == both ]]; then
      run_test "$variant" test_index_delta
    fi
    if [[ "$variant" == both ]]; then
      run_test "$variant" test_storage_index_budget
      run_test "$variant" test_filter_paths
      run_test "$variant" test_index_budget -DMVDB_TEST_WRAP_PWRITE -Wl,--wrap=pwrite
      "$build/$variant/test_index_budget" --ann-budget
      printf '%s\t%s\n' "$variant" test_index_budget_ann >> "$out/passed.tsv"
    fi
  done
  # Forced spill is a separate test build, never one of the benchmark binaries.
  for variant in forced_baseline forced_spill forced_both budget_1 budget_8192; do
    features=(-DMVDB_SPILL_BYTES=1)
    [[ "$variant" != forced_baseline ]] && features+=(-DMVDB_ENABLE_SPILL_FILTER)
    [[ "$variant" == forced_both || "$variant" == budget_* ]] && features+=(-DMVDB_ENABLE_INDEX_DELTA)
    [[ "$variant" == budget_* ]] && features+=("-DMVDB_FLAT_INDEX_MEMORY_BYTES=${variant#budget_}")
    build_variant "$variant" tests "${features[@]}"
    run_test "$variant" test_spill_filter -DMVDB_TEST_WRAP_PWRITE -Wl,--wrap=pread -Wl,--wrap=pwrite
    [[ "$variant" == budget_* ]] && run_test "$variant" test_storage_index_budget
  done
  exit 0
fi

cd "$repo"
out="$repo/storage-report-results"
test ! -e "$out"
mkdir -p "$out"
build=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-storage-build-XXXXXX")
touch "$out/passed.tsv"
printf '%s\n' setup > "$out/current-stage.txt"
finish() {
  local code=$?
  trap - EXIT
  set +e
  STORAGE_REPORT_EXIT="$code" STORAGE_REPORT_OUT="$out" python3 - <<'PY'
import json, os
from pathlib import Path
out=Path(os.environ['STORAGE_REPORT_OUT'])
passed=[dict(zip(('configuration','test'),line.split('\t'))) for line in
        (out/'passed.tsv').read_text().splitlines()]
summary={'exit_code':int(os.environ['STORAGE_REPORT_EXIT']),
         'stage':(out/'current-stage.txt').read_text().strip(),
         'passed':passed,'official_score':None,
         'performance_measured':(out/'performance-summary.json').exists()}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
for path in sorted(out.glob("*.json")):
    print("MINIVEC_STORAGE_BEGIN "+path.name)
    print(path.read_text())
    print("MINIVEC_STORAGE_END")
PY
  report_code=$?
  if [[ "$report_code" != 0 && "$code" == 0 ]]; then code=$report_code;fi
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
  --entrypoint /bin/bash "$compiler" /src/minivec/tools/ci_storage_report.sh --container

# Protocol/ranking gates bind directly to the four fixed-compiler static files.
for variant in baseline delta spill both; do
  printf '%s\n' "$variant/protocol" > "$out/current-stage.txt"
  python3 minivec/tests/json_protocol.py --binary "$out/binaries/$variant.bin" --timeout 15
  printf '%s\t%s\n' "$variant" json_protocol >> "$out/passed.tsv"
  python3 minivec/tests/topk_bounds.py --binary "$out/binaries/$variant.bin"
  printf '%s\t%s\n' "$variant" topk_bounds >> "$out/passed.tsv"
done

# Sanitizer runtime comes from the host compiler; it is not statically linked.
cxx=${CXX:-g++}
compile_flags=(-O1 -g -std=c++17 -pthread -fno-omit-frame-pointer -fno-pie -no-pie
  -fsanitize=address,undefined -I"$repo/minivec/src" -I"$repo/minivec/third_party")
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
build_variant sanitizer_both tests -DMVDB_ENABLE_INDEX_DELTA -DMVDB_ENABLE_SPILL_FILTER
run_test sanitizer_both test_index_delta
run_test sanitizer_both test_resident_update -DMVDB_TEST_WRAP_PWRITE -Wl,--wrap=pwrite
run_test sanitizer_both test_storage_index_budget
build_variant sanitizer_spill tests -DMVDB_ENABLE_INDEX_DELTA -DMVDB_ENABLE_SPILL_FILTER -DMVDB_SPILL_BYTES=1
run_test sanitizer_spill test_spill_filter -DMVDB_TEST_WRAP_PWRITE -Wl,--wrap=pread -Wl,--wrap=pwrite

python3 - <<'PY'
import hashlib,json,platform,subprocess
from pathlib import Path
out=Path('storage-report-results')
context={'candidate_commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
 'compiler':'pinned GCC 13.3.0, O3, static, PGO off on all four sides',
 'sanitizer_compiler':subprocess.check_output(['g++','--version'],text=True).splitlines()[0],
 'platform':platform.platform(),
 'binaries':{path.name:hashlib.sha256(path.read_bytes()).hexdigest()
             for path in sorted((out/'binaries').glob('*.bin'))},
 'limits':['Correctness gates only; no speedup or official score claimed.',
           'Forced spill and tiny index budgets affect test builds only.',
           'I06 is UPDATE-only and I07 still scans the vector index sequentially.']}
(out/'context.json').write_text(json.dumps(context,indent=2)+'\n')
PY
printf '%s\n' completed > "$out/current-stage.txt"
if [[ "${STORAGE_CORRECTNESS_ONLY:-0}" == 1 ]]; then exit 0;fi
printf '%s\n' paired-screen > "$out/current-stage.txt"
python3 - <<'PY'
import hashlib,json,os,subprocess
from pathlib import Path
out=Path('storage-report-results').resolve()
cpus=sorted(os.sched_getaffinity(0))[:2]
if len(cpus)!=2:raise SystemExit('two CPUs required')
os.sched_setaffinity(0,cpus)
before={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (out/'binaries').glob('*.bin')}
for candidate,harness,label,extra in [
 ('delta','benchmark_iteration.py','update',['--suite','full','--scenario','update1','--scenario','update10','--scenario','update100']),
 ('spill','benchmark_report_matrix.py','spill',['--scenario','spill_compound','--scenario','spill_high']),
 ('both','benchmark_iteration.py','combined',['--suite','full','--scenario','standard10k','--scenario','update10','--scenario','spill'])]:
 subprocess.run(['python3','minivec/tools/'+harness,'--baseline',str(out/'binaries/baseline.bin'),
  '--candidate',str(out/('binaries/'+candidate+'.bin')),'--output',str(out/(label+'.json')),
  '--queries','64','--pairs','1','--seeds','7319','8911','13337']+extra,check=True)
after={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (out/'binaries').glob('*.bin')}
assert before==after
(out/'performance-summary.json').write_text(json.dumps({'status':'passed','pairs':24,
 'affinity':cpus,'binary_unchanged':True,'official_score':None,
 'scope':'three-pair screening; retain or revert only after targeted confirmation'})+'\n')
PY
printf '%s\n' completed > "$out/current-stage.txt"
