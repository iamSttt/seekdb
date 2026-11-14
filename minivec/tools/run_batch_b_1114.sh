#!/usr/bin/env bash
# Isolated B native guards. No automatic CI dispatch or performance run.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
out=${1:?absolute output directory required}
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/mvdb-batch-b-XXXXXX")
cd "$repo/minivec"
sources=(src/db.cpp src/flat_index.cpp src/value.cpp)
for flavor in optimized sanitized; do
  flags=(-O2 -std=c++17 -pthread -Isrc)
  if [[ $flavor == sanitized ]]; then flags=(-O1 -g -std=c++17 -pthread -Isrc -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie); fi
  for control in enabled disabled; do
    defines=(-DMVDB_TEST_WRAP_PWRITE -DMVDB_SPILL_BYTES=1048576)
    if [[ $control == disabled ]]; then defines+=(-DMVDB_DISABLE_PAYLOAD_BATCH); fi
    g++ "${flags[@]}" "${defines[@]}" tests/test_payload_batch.cpp "${sources[@]}" -Wl,--wrap=pwrite -o "$scratch/payload-$flavor-$control" > "$out/payload-$flavor-$control-build.txt" 2>&1
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/payload-$flavor-$control" > "$out/payload-$flavor-$control.txt" 2>&1
  done
  g++ "${flags[@]}" -DMVDB_SPILL_BYTES=1 tests/test_flat_index.cpp "${sources[@]}" -o "$scratch/recovery-$flavor" > "$out/recovery-$flavor-build.txt" 2>&1
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/recovery-$flavor" > "$out/recovery-$flavor.txt" 2>&1
done
sha256sum "$scratch/"* > "$out/binary-sha256.txt"
MINIVEC_REPORTS="$out" bash "$repo/minivec/tools/ci_spill_guard_1114.sh"
