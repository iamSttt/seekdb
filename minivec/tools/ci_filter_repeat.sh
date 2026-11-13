#!/usr/bin/env bash
# Fixed-policy follow-up to report-C. No production changes or other matrices.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ "${1:-}" = --container ]; then
  cd "$repo/minivec"
  common=(-O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections
    -Wl,--gc-sections -static -pthread -Isrc -Ithird_party)
  mkdir -p /out/binaries /out/layout
  g++ --version > /out/compiler-version.txt
  for side in legacy multi_only payload_only default; do
    flags=()
    case "$side" in
      legacy) flags=(-DMVDB_DISABLE_MULTI_POSTING -DMVDB_DISABLE_PAYLOAD_BATCH);;
      multi_only) flags=(-DMVDB_DISABLE_PAYLOAD_BATCH);;
      payload_only) flags=(-DMVDB_DISABLE_MULTI_POSTING);;
    esac
    printf '%s\n' "build-$side" > /out/current-stage.txt
    printf '%s\n' "${common[@]}" "${flags[@]}" > "/out/layout/$side-build-flags.txt"
    mkdir -p "/build/$side"
    objects=()
    for source in src/*.cpp; do
      object="/build/$side/$(basename "${source%.cpp}").o"
      g++ "${common[@]}" "${flags[@]}" -c "$source" -o "$object"
      objects+=("$object")
    done
    # Preserve the original src/*.cpp link order and capture layout without
    # changing optimization flags or adding alignment/padding experiments.
    g++ "${common[@]}" "${flags[@]}" "${objects[@]}" \
      "-Wl,-Map,/out/layout/$side-link.map" -o "/out/binaries/$side.bin"
    sha256sum "${objects[@]}" > "/out/layout/$side-object-sha256.txt"
    nm -nC "/out/binaries/$side.bin" > "/out/layout/$side-symbols.txt"
    objdump -drC "/build/$side/exec.o" > "/out/layout/$side-exec-object.asm"
    objdump -h "/out/binaries/$side.bin" > "/out/layout/$side-sections.txt"
  done
  exit 0
fi

cd "$repo"
out="$repo/filter-repeat-results"
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
status={'exit_code':int(sys.argv[2]),'stage':(out/'current-stage.txt').read_text().strip(),
        'passed_tests':passed,'official_score':None,'adopted':False}
(out/'run-status.json').write_text(json.dumps(status,indent=2)+'\n')
for path in sorted(out.glob('*.json')):
    print('MINIVEC_FILTER_REPEAT_BEGIN '+path.name)
    print(path.read_text())
    print('MINIVEC_FILTER_REPEAT_END')
PY
    report_code=$?
    if [ "$code" -eq 0 ] && [ "$report_code" -ne 0 ]; then code=$report_code; fi
    if [ "$report_code" -ne 0 ]; then
      printf '{"exit_code":%s,"report_collection_exit_code":%s}\n' "$code" "$report_code" > "$out/collection-error.json"
    fi
    exit "$code"
  }
  trap finish EXIT
  timeout --signal=TERM --kill-after=30s 3300 bash "${BASH_SOURCE[0]}" --run
  exit 0
fi

build=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-filter-repeat-XXXXXX")
compiler=$(awk '/^FROM .* AS build$/ {print $2; exit}' Dockerfile)
test "$compiler" = 'gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
git rev-parse HEAD > "$out/source-commit.txt"
git status --porcelain --untracked-files=no > "$out/worktree-status.txt"
find minivec/src minivec/third_party -type f -print0 | sort -z | xargs -0 sha256sum > "$out/source-sha256.txt"
sha256sum minivec/tools/{ci_filter_repeat.sh,benchmark_filter_repeat.py,benchmark_iteration.py} \
  minivec/tests/{json_protocol.py,topk_bounds.py} > "$out/harness-sha256.txt"
python3 minivec/tools/benchmark_filter_repeat.py --plan-only --output "$out/plan.json"
printf '%s\t%s\n' python interleaved_plan >> "$out/passed.tsv"
docker pull "$compiler"
docker run --rm --network none --platform linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$repo,target=/src,readonly" --mount "type=bind,source=$out,target=/out" \
  --mount "type=bind,source=$build,target=/build" --workdir /src --entrypoint /bin/bash \
  "$compiler" /src/minivec/tools/ci_filter_repeat.sh --container 2>&1 | tee "$out/build.log"

for side in legacy multi_only payload_only default; do
  printf '%s\n' "$side-protocol" > "$out/current-stage.txt"
  python3 minivec/tests/json_protocol.py --binary "$out/binaries/$side.bin" --timeout 15 | tee "$out/$side-json-protocol.log"
  printf '%s\t%s\n' "$side" json_protocol >> "$out/passed.tsv"
  for threads in 1 2; do
    MVDB_QUERY_THREADS="$threads" python3 minivec/tests/topk_bounds.py --binary "$out/binaries/$side.bin" | tee "$out/$side-topk-$threads.log"
    printf '%s\t%s\n' "$side" "topk-$threads" >> "$out/passed.tsv"
  done
done
printf '%s\n' interleaved_fixed_threads > "$out/current-stage.txt"
python3 minivec/tools/benchmark_filter_repeat.py --binaries "$out/binaries" \
  --source-commit "$(cat "$out/source-commit.txt")" --output "$out/interleaved.json" \
  --threads 1 2 --seeds 7319 8911 13337 --pairs 5 --queries 1000
sha256sum -c "$out/source-sha256.txt" > "$out/source-verified.txt"
sha256sum -c "$out/harness-sha256.txt" > "$out/harness-verified.txt"
python3 - "$out" "$compiler" <<'PY'
import hashlib,json,sys
from pathlib import Path
out=Path(sys.argv[1]);data=json.loads((out/'interleaved.json').read_text())
assert data['status']=='passed' and len(data['pairs'])==120 and data['binary_unchanged']
assert all(p['all_responses_identical'] for p in data['pairs'])
assert len(data['summary'])==8 and all(v['primary_all_requests']['query_before_update']['pairs']==15 for v in data['summary'].values())
objects={}
for side in ('legacy','multi_only','payload_only','default'):
    objects[side]={Path(path).name:digest for digest,path in
        (line.split(None,1) for line in (out/'layout'/f'{side}-object-sha256.txt').read_text().splitlines())}
context={'source_commit':data['source_commit'],'compiler_image':sys.argv[2],
         'compiler_version':(out/'compiler-version.txt').read_text(),
         'flags':'O3 static C++17; function/data sections and gc-sections; no PGO/LTO',
         'factors':{'legacy':[],'multi_only':['I05'],'payload_only':['I08'],'default':['I05','I08']},
         'object_hashes':objects,'exec_object_equal':{side:objects[side]['exec.o']==objects['legacy']['exec.o'] for side in objects},
         'binaries':data['binaries'],'source_verified':True,'harness_verified':True,
         'official_score':None,'adopted':False,
         'limits':['Object equality is not equal final code addresses; link maps/symbols are retained.',
                   'Do not infer I05 x I08 interaction until interleaved A/A and order strata are assessed.']}
(out/'context.json').write_text(json.dumps(context,indent=2)+'\n')
summary={'status':'passed','verified_pairs':120,'contrasts':list(data['summary']),
         'binary_unchanged':True,'source_verified':True,'official_score':None,'adopted':False}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
PY
printf '%s\n' passed > "$out/current-stage.txt"
