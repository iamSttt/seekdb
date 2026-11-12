#!/usr/bin/env bash
# Linux-only standalone experiment; never edits the supplied repository.
# Usage: compare.sh /absolute/repo COMMIT /absolute/new-results-dir [--build-only]
set -euo pipefail
if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
  echo 'usage: compare.sh REPO COMMIT NEW_OUTPUT_DIR [--build-only]' >&2; exit 2
fi
if [ "$#" -eq 4 ] && [ "$4" != --build-only ]; then exit 2; fi
draft=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$1" && pwd)
revision=$(git -C "$repo" rev-parse --verify "$2^{commit}")
test ! -e "$3"
mkdir -p "$3"
out=$(cd "$3" && pwd)
printf '%s' source-archive > "$out/stage.txt"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-pgo-XXXXXX")
context="$scratch/context"
mkdir -p "$context/pgo"
git -C "$repo" archive "$revision" minivec/src minivec/third_party | tar -x -C "$context"
cp "$draft/Dockerfile.pgo" "$context/Dockerfile.pgo"
cp "$draft/.dockerignore" "$context/.dockerignore"
cp "$draft/train.cpp" "$draft/run-training.sh" "$context/pgo/"
printf '%s\n' "$revision" > "$out/source-commit.txt"
printf '%s\n' "$scratch" > "$out/scratch-path.txt"
git -C "$repo" show "$revision:Dockerfile" > "$out/reference-Dockerfile"
python3 - "$out/reference-Dockerfile" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text()
compiler = 'gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
command = 'g++ -O3 -std=c++17 -fno-math-errno -ffunction-sections -fdata-sections -Wl,--gc-sections -static -pthread -Isrc src/*.cpp -o /minivec'
if text.count('FROM '+compiler+' AS build') != 1 or text.count(command) != 1:
    raise SystemExit('Selected root Dockerfile changed compiler/options; review the PGO baseline before continuing')
PY
git -C "$repo" status --porcelain --untracked-files=no > "$out/source-worktree-status.txt"
lscpu > "$out/lscpu.txt"
sha256sum "$context/Dockerfile.pgo" "$context/.dockerignore" "$context/pgo/"* "$draft/compare.sh" "$draft/report.py" > "$out/draft-sha256.txt"
if [ -f "$draft/../ci_pgo.sh" ]; then sha256sum "$draft/../ci_pgo.sh" >> "$out/draft-sha256.txt"; fi
find "$context/minivec" -type f -print0 | sort -z | xargs -0 sha256sum > "$out/source-sha256.txt"
# A unique label avoids overwriting other experiments' images.
tag="minivec:pgo-draft-$(basename "$scratch")"
for mode in baseline pgo; do
  enabled=0; if [ "$mode" = pgo ]; then enabled=1; fi
  build_image="$tag-$mode-build"
  image="$tag-$mode"
  printf '%s' "build-$mode" > "$out/stage.txt"
  docker build --network none --platform linux/amd64 --progress=plain \
    --build-arg ENABLE_PGO="$enabled" --target build -f "$context/Dockerfile.pgo" \
    -t "$build_image" "$context" 2>&1 | tee "$out/build-$mode.log"
  if [ "$mode" = pgo ]; then
    container=$(docker create "$build_image")
    mkdir "$out/training"
    docker cp "$container:/training/corpus/manifest.json" "$out/training/manifest.json"
    docker cp "$container:/training/corpus-sha256.txt" "$out/training/corpus-sha256.txt"
    docker cp "$container:/training/profile-sha256.txt" "$out/training/profile-sha256.txt"
    docker cp "$container:/training/verification.jsonl" "$out/training/verification.jsonl"
    docker cp "$container:/pgo" "$out/training/profile-data"
    docker rm -v "$container"
  fi
  printf '%s' "release-$mode" > "$out/stage.txt"
  docker build --network none --platform linux/amd64 --progress=plain \
    --build-arg ENABLE_PGO="$enabled" --target release -f "$context/Dockerfile.pgo" \
    -t "$image" "$context" 2>&1 | tee "$out/release-$mode.log"
  container=$(docker create "$image")
  docker cp "$container:/minivec" "$out/$mode.bin"
  docker export "$container" | tar -tf - > "$out/$mode-files.txt"
  docker rm -v "$container"
  sha256sum "$out/$mode.bin" > "$out/$mode-sha256.txt"
  docker image inspect "$image" > "$out/$mode-image.json"
done
# Use exactly the selected revision's public driver and benchmark harness too.
mkdir "$scratch/harness"
git -C "$repo" archive "$revision" minivec/tests minivec/tools protocol_check mysqltest STDIO_PROTOCOL.md | tar -x -C "$scratch/harness"
cd "$scratch/harness"
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
python3 "$draft/report.py" check-binaries "$out"
printf '%s' complete-build-only > "$out/stage.txt"
# Refuse benchmarks unless corpus verification, copied profile hashes, both
# release packages and all eight correctness gates have concrete evidence.
python3 "$draft/report.py" collect "$out" "$revision" 0
if [ "${4:-}" = --build-only ]; then exit 0; fi
# Evaluation seeds are disjoint from training; the oracle never reads these.
# Pin the launcher and all children to two allowed CPUs. The existing benchmark
# alternates pairs and verifies sampled answers; it does not pin CPUs itself.
benchmark() {
  python3 - "$out/affinity.json" "$@" <<'PY'
import json, os, sys
cpus = sorted(os.sched_getaffinity(0))[:2]
if len(cpus) != 2:
    raise SystemExit('PGO comparison needs two available CPUs')
os.sched_setaffinity(0, cpus)
with open(sys.argv[1], 'w', encoding='utf-8') as file:
    json.dump({'cpu_affinity': cpus}, file)
os.execv(sys.executable, [sys.executable, 'minivec/tools/benchmark_iteration.py', *sys.argv[2:]])
PY
}
printf '%s' paired-standard > "$out/stage.txt"
benchmark --suite full --scenario standard10k \
  --baseline "$out/baseline.bin" --candidate "$out/pgo.bin" --queries 1000 \
  --seeds 7319 8911 13337 --pairs 5 --output "$out/standard.json"
printf '%s' paired-boundaries > "$out/stage.txt"
benchmark --suite full \
  --scenario resident30k --scenario resident50k --scenario low16 --scenario filter60k \
  --scenario update10 --scenario spill --baseline "$out/baseline.bin" --candidate "$out/pgo.bin" \
  --queries 64 --seeds 7319 8911 13337 --pairs 5 --output "$out/boundaries.json"
printf '%s' complete > "$out/stage.txt"

