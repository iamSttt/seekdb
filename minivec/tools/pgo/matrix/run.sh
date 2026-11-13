#!/usr/bin/env bash
# Same-commit experiment; production Dockerfile and build-only helper are untouched.
# Usage: run.sh REPO COMMIT NEW_OUTPUT_DIR [--build-only|--screen]
set -euo pipefail
if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
  echo 'usage: run.sh REPO COMMIT NEW_OUTPUT_DIR [--build-only|--screen]' >&2; exit 2
fi
mode=${4:-full}
case "$mode" in full|--build-only|--screen) ;; *) exit 2 ;; esac
repo=$(cd "$1" && pwd)
revision=$(git -C "$repo" rev-parse --verify "$2^{commit}")
test ! -e "$3"
mkdir -p "$3"
out=$(cd "$3" && pwd)
printf '%s\n' "$revision" > "$out/source-commit.txt"
printf '%s\n' "$mode" > "$out/mode.txt"
printf '%s' archive > "$out/stage.txt"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-pgo-matrix-XXXXXX")
printf '%s\n' "$scratch" > "$out/scratch-path.txt"
snapshot="$scratch/snapshot"
context="$scratch/context"
mkdir -p "$snapshot" "$context/pgo"
git -C "$repo" archive "$revision" minivec/src minivec/third_party minivec/tests \
  minivec/tools protocol_check mysqltest STDIO_PROTOCOL.md Dockerfile | tar -x -C "$snapshot"
draft="$snapshot/minivec/tools/pgo/matrix"
cp "$draft/Dockerfile" "$context/Dockerfile.matrix"
cp "$draft/.dockerignore" "$context/.dockerignore"
cp "$draft/train.cpp" "$draft/spill-training.inc" "$draft/run-training.sh" "$context/pgo/"
cp "$snapshot/minivec/tools/pgo/train.cpp" "$context/pgo/train-original.cpp"
mkdir -p "$context/minivec"
cp -a "$snapshot/minivec/src" "$snapshot/minivec/third_party" "$context/minivec/"
git -C "$repo" status --porcelain --untracked-files=no > "$out/worktree-status.txt"
cp "$snapshot/Dockerfile" "$out/production-Dockerfile"
lscpu > "$out/lscpu.txt"
(cd "$context"; find minivec -type f -print0 | sort -z | xargs -0 sha256sum) > "$out/source-sha256.txt"
(cd "$snapshot"; find minivec/tools/pgo minivec/tests protocol_check mysqltest -type f -print0 | sort -z | xargs -0 sha256sum) > "$out/harness-sha256.txt"
(cd "$context"; sha256sum Dockerfile.matrix .dockerignore pgo/*) > "$out/build-input-sha256.txt"
python3 "$draft/report.py" init "$out" "$revision"
tag="minivec:matrix-$(basename "$scratch")"
for variant in o3 lto pgo pgo_spill pgo_lto pgo_lto_spill; do
  pgo=0; lto=0; spill=0
  case "$variant" in pgo*) pgo=1 ;; esac
  case "$variant" in lto|pgo_lto*) lto=1 ;; esac
  case "$variant" in *_spill) spill=1 ;; esac
  dir="$out/$variant"
  mkdir "$dir"
  build="$tag-$variant-build"; image="$tag-$variant"
  printf '%s' "build-$variant" > "$out/stage.txt"
  start=$(date +%s%N)
  docker build --network none --platform linux/amd64 --progress=plain \
    --build-arg ENABLE_PGO="$pgo" --build-arg ENABLE_LTO="$lto" \
    --build-arg PGO_SPILL_TRAINING="$spill" --target build \
    -f "$context/Dockerfile.matrix" -t "$build" "$context" 2>&1 | tee "$dir/build.log"
  stop=$(date +%s%N)
  printf '{"nanoseconds":%s,"status":"passed","includes_training":%s}\n' "$((stop-start))" "$pgo" > "$dir/build-cost.json"
  if [ "$pgo" -eq 1 ]; then
    container=$(docker create "$build")
    mkdir "$dir/training"
    for name in corpus corpus-sha256.txt profile-sha256.txt verification.jsonl \
      original-corpus-match.json training-cost.json; do
      docker cp "$container:/training/$name" "$dir/training/$name"
    done
    for threads in 1 2; do
      for phase in 0 1 2; do
        docker cp "$container:/training/responses-$threads-$phase.jsonl" "$dir/training/"
        docker cp "$container:/training/stderr-$threads-$phase.log" "$dir/training/"
      done
    done
    docker cp "$container:/pgo" "$dir/training/profile-data"
    if [ "$spill" -eq 1 ]; then
      for name in spill-corpus spill-corpus-sha256.txt spill-evidence spill-verification.jsonl; do
        docker cp "$container:/training/$name" "$dir/training/$name"
      done
    fi
    docker rm -v "$container"
  fi
  printf '%s' "release-$variant" > "$out/stage.txt"
  docker build --network none --platform linux/amd64 --progress=plain \
    --build-arg ENABLE_PGO="$pgo" --build-arg ENABLE_LTO="$lto" \
    --build-arg PGO_SPILL_TRAINING="$spill" --target release \
    -f "$context/Dockerfile.matrix" -t "$image" "$context" 2>&1 | tee "$dir/release.log"
  container=$(docker create "$image")
  docker cp "$container:/minivec" "$dir/minivec"
  docker export "$container" | tar -tf - > "$dir/release-files.txt"
  docker rm -v "$container"
  sha256sum "$dir/minivec" > "$dir/binary-sha256.txt"
  docker image inspect "$image" > "$dir/image.json"
done
cd "$snapshot"
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
python3 "$draft/report.py" check-binaries "$out"
printf '%s' complete-build-only > "$out/stage.txt"
python3 "$draft/report.py" collect "$out" "$revision" 0
if [ "$mode" = --build-only ]; then exit 0; fi
benchmark() {
  python3 - "$out/affinity.json" "$@" <<'PY'
import json, os, sys
cpus = sorted(os.sched_getaffinity(0))[:2]
if len(cpus) != 2:
    raise SystemExit('matrix comparison needs two available CPUs')
os.sched_setaffinity(0, cpus)
with open(sys.argv[1], 'w', encoding='utf-8') as file:
    json.dump({'cpu_affinity': cpus}, file)
os.execv(sys.executable, [sys.executable, 'minivec/tools/benchmark_iteration.py', *sys.argv[2:]])
PY
}
# Four compiler choices at fixed base training, then the isolated extra-training
# factor with and without LTO. No construction failure is a performance sample.
pairs=5; queries=1000
if [ "$mode" = --screen ]; then pairs=1; queries=64; fi
boundary_pairs=${PGO_MATRIX_BOUNDARY_PAIRS:-1}
case "$boundary_pairs" in 1|5) ;; *) echo 'boundary pairs must be 1 (screen) or 5 (confirm)' >&2; exit 2 ;; esac
printf '{"mode":"%s","standard_pairs_per_seed":%s,"standard_queries":%s,"boundary_pairs_per_seed":%s,"boundary_queries":64}\n' \
  "$mode" "$pairs" "$queries" "$boundary_pairs" > "$out/measurement-plan.json"
for comparison in o3:lto o3:pgo o3:pgo_lto pgo:pgo_spill pgo_lto:pgo_lto_spill; do
  baseline=${comparison%:*}; candidate=${comparison#*:}
  name="$baseline-vs-$candidate"
  printf '%s' "standard-$name" > "$out/stage.txt"
  benchmark --suite full --scenario standard10k --baseline "$out/$baseline/minivec" \
    --candidate "$out/$candidate/minivec" --queries "$queries" --seeds 7319 8911 13337 \
    --pairs "$pairs" --output "$out/$name-standard.json"
  printf '%s' "boundary-$name" > "$out/stage.txt"
  benchmark --suite full --scenario resident30k --scenario low16 --scenario filter60k \
    --scenario update10 --scenario spill --baseline "$out/$baseline/minivec" \
    --candidate "$out/$candidate/minivec" --queries 64 --seeds 7319 8911 13337 \
    --pairs "$boundary_pairs" --output "$out/$name-boundaries.json"
done
printf '%s' complete > "$out/stage.txt"
