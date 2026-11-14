#!/usr/bin/env bash
# Runs only inside the experiment's pinned compiler build stage.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_FANOUT_ROWS
unset MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
/training/train generate /training/corpus
: > /training/verification.jsonl
for threads in 1 2; do
  data="/training/data-$threads"
  mkdir "$data"
  for phase in 0 1 2; do
    output="/training/responses-$threads-$phase.jsonl"
    errors="/training/stderr-$threads-$phase.log"
    timeout 240 env MVDB_PARSE_THREADS="$threads" MVDB_QUERY_THREADS="$threads" \
      /minivec --data "$data" < "/training/corpus/phase-$phase.jsonl" > "$output" 2> "$errors"
    /training/train verify "/training/corpus/phase-$phase.expected.jsonl" "$output" | tee -a /training/verification.jsonl
    if grep -Ei 'libgcov profiling error|profiling:.*(error|cannot|mismatch|overflow)' "$errors"; then
      echo 'Invalid compiler feedback; refusing profile correction' >&2
      exit 1
    fi
  done
done
mapfile -d '' profiles < <(find /pgo -type f -name '*.gcda' -print0 | sort -z)
sources=(src/*.cpp)
test "${#profiles[@]}" -eq "${#sources[@]}"
for path in "${profiles[@]}"; do test -s "$path"; done
sha256sum "${profiles[@]}" > /training/profile-sha256.txt
sha256sum /training/corpus/*.jsonl /training/corpus/manifest.json > /training/corpus-sha256.txt
cat /training/profile-sha256.txt

