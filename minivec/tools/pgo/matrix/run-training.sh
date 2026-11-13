#!/usr/bin/env bash
# Runs only inside the experiment's pinned compiler build stage.
set -euo pipefail
training_failure() {
  result=$?
  trap - EXIT
  if [ "$result" -ne 0 ]; then
    # A failed Docker RUN has no exportable build image. Preserve diagnostics in
    # the build log, and never reinterpret a failed build as a timing result.
    for path in /training/stderr-*.log /training/spill-evidence/*.stderr.log \
      /training/verification.jsonl /training/spill-verification.jsonl \
      /training/spill-evidence/checkpoints.json /training/spill-evidence/timings.json; do
      [ ! -f "$path" ] || { echo "TRAINING_FAILURE_EVIDENCE $path"; tail -c 32768 "$path"; }
    done
  fi
  exit "$result"
}
trap training_failure EXIT
case "${PGO_SPILL_TRAINING:-0}" in 0|1) ;; *) echo 'PGO_SPILL_TRAINING must be 0 or 1' >&2; exit 2 ;; esac
training_started=$(date +%s%N)
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_FANOUT_ROWS
unset MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
/training/train generate /training/corpus
# A build-only golden comparison proves the old corpus/oracle did not change.
/training/train-original generate /training/corpus-reference > /training/reference-manifest.json
for name in manifest.json phase-{0,1,2}.jsonl phase-{0,1,2}.expected.jsonl; do
  cmp "/training/corpus/$name" "/training/corpus-reference/$name"
done
printf '{"status":"passed","files_compared":7,"reference":"same-commit original training helper"}\n' > /training/original-corpus-match.json
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
base_finished=$(date +%s%N)
if [ "${PGO_SPILL_TRAINING:-0}" = 1 ]; then
  /training/train generate-spill /training/spill-corpus
  timeout 240 env MVDB_PARSE_THREADS=1 MVDB_QUERY_THREADS=1 \
    /training/train exercise-spill /minivec /training/spill-corpus \
      /training/spill-data /training/spill-evidence > /training/spill-verification.jsonl
  cat /training/spill-verification.jsonl
  cat /training/spill-evidence/checkpoints.json
  for errors in /training/spill-evidence/*.stderr.log; do
    if grep -Ei 'libgcov profiling error|profiling:.*(error|cannot|mismatch|overflow)' "$errors"; then
      echo 'Invalid spill compiler feedback; refusing profile correction' >&2; exit 1
    fi
  done
  sha256sum /training/spill-corpus/*.jsonl /training/spill-corpus/manifest.json > /training/spill-corpus-sha256.txt
fi
spill_finished=$(date +%s%N)
printf '{"base_nanoseconds":%s,"spill_extension_nanoseconds":%s,"total_nanoseconds":%s,"spill_enabled":%s,"scope":"generation+original-corpus-comparison+training+oracle+fd-inspection; compiler build separately timed"}\n' \
  "$((base_finished-training_started))" "$((spill_finished-base_finished))" \
  "$((spill_finished-training_started))" "${PGO_SPILL_TRAINING:-0}" > /training/training-cost.json
mapfile -d '' profiles < <(find /pgo -type f -name '*.gcda' -print0 | sort -z)
sources=(src/*.cpp)
test "${#profiles[@]}" -eq "${#sources[@]}"
for path in "${profiles[@]}"; do test -s "$path"; done
sha256sum "${profiles[@]}" > /training/profile-sha256.txt
sha256sum /training/corpus/*.jsonl /training/corpus/manifest.json > /training/corpus-sha256.txt
cat /training/profile-sha256.txt

