#!/usr/bin/env bash
# Isolated A native guard only; does not dispatch CI or benchmark.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
out=${1:?absolute output directory required}
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/mvdb-batch-a-XXXXXX")
cd "$repo/minivec"
sources=(src/db.cpp src/flat_index.cpp src/value.cpp)
for flavor in optimized sanitized; do
  flags=(-O2 -std=c++17 -pthread -Isrc)
  if [[ $flavor == sanitized ]]; then flags=(-O1 -g -std=c++17 -pthread -Isrc -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie); fi
  g++ "${flags[@]}" -DMVDB_SPILL_BYTES=1 tests/test_flat_batch_1114.cpp "${sources[@]}" -Wl,--wrap=pwrite -o "$scratch/flat-$flavor" > "$out/flat-$flavor-build.txt" 2>&1
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/flat-$flavor" > "$out/flat-$flavor.txt" 2>&1
  g++ "${flags[@]}" -DMVDB_SPILL_BYTES=1 tests/test_flat_index.cpp "${sources[@]}" -o "$scratch/recovery-$flavor" > "$out/recovery-$flavor-build.txt" 2>&1
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/recovery-$flavor" > "$out/recovery-$flavor.txt" 2>&1
done
sha256sum "$scratch/"* > "$out/binary-sha256.txt"
# Existing matrix also covers scalar-cache allocation fallback and budgets.
MINIVEC_REPORTS="$out" bash "$repo/minivec/tools/ci_spill_guard_1114.sh"
