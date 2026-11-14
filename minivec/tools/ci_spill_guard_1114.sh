#!/usr/bin/env bash
# Combined-candidate native spill guards; root-image and general recovery gates run in ci_score.sh.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="${MINIVEC_REPORTS:?missing report directory}/spill-guards"
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-combined-spill-XXXXXX")
cd minivec
common=(-O2 -std=c++17 -pthread -Isrc)
sources=(src/db.cpp src/exec.cpp src/sql.cpp src/value.cpp src/flat_index.cpp)
for budget in 16777216 8192 1; do
  g++ "${common[@]}" -DMVDB_ENABLE_SPILL_FILTER -DMVDB_TEST_WRAP_PWRITE \
    -DMVDB_SPILL_BYTES=1 -DMVDB_FLAT_INDEX_MEMORY_BYTES="$budget" \
    tests/test_spill_filter.cpp "${sources[@]}" -Wl,--wrap=pread -Wl,--wrap=pwrite \
    -o "$scratch/spill-$budget" > "$out/spill-$budget-build.txt" 2>&1
  "$scratch/spill-$budget" > "$out/spill-$budget.txt" 2>&1
  g++ "${common[@]}" -DMVDB_ENABLE_SPILL_FILTER -DMVDB_FLAT_INDEX_MEMORY_BYTES="$budget" \
    tests/test_storage_index_budget.cpp src/db.cpp src/flat_index.cpp src/value.cpp \
    -o "$scratch/storage-$budget" > "$out/storage-$budget-build.txt" 2>&1
  "$scratch/storage-$budget" > "$out/storage-$budget.txt" 2>&1
done
g++ "${common[@]}" -DMVDB_FLAT_INDEX_MEMORY_BYTES=8192 \
  tests/test_spill_payload_scope.cpp src/db.cpp src/flat_index.cpp src/value.cpp \
  -o "$scratch/payload-scope" > "$out/payload-scope-build.txt" 2>&1
"$scratch/payload-scope" > "$out/payload-scope.txt" 2>&1
g++ -O1 -g -std=c++17 -pthread -Isrc -fno-omit-frame-pointer \
  -fsanitize=address,undefined -no-pie -DMVDB_ENABLE_SPILL_FILTER -DMVDB_TEST_WRAP_PWRITE \
  -DMVDB_SPILL_BYTES=1 tests/test_spill_filter.cpp "${sources[@]}" \
  -Wl,--wrap=pread -Wl,--wrap=pwrite -o "$scratch/spill-sanitized" > "$out/spill-sanitized-build.txt" 2>&1
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/spill-sanitized" > "$out/spill-sanitized.txt" 2>&1
sha256sum "$scratch/"spill-* "$scratch/"storage-* "$scratch/payload-scope" > "$out/native-binary-sha256.txt"
