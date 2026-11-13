#!/usr/bin/env bash
# Fixed delivered-source baseline, isolated bulk guard candidate, 192 interleaved pairs.
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
if [[ "${1:-}" == --unit ]]; then
  cd /src/minivec
  mkdir -p /out/unit
  : > /out/unit/passed.tsv
  common=(-std=c++17 -pthread -Isrc -DMVDB_ENABLE_SPILL_FILTER -DMVDB_ENABLE_TYPED_JSON
    -DMVDB_TYPED_WIDE_VECTOR_ONLY -DMVDB_MULTI_POSTING_REUSE_ONLY -DMVDB_JSON_RESPONSE_DIAGNOSTICS)
  g++ --version > /out/unit/compiler-version.txt
  for role in control bulk; do
    features=();tests=(bulk_typed_response);modes=(normal)
    if [[ "$role" == bulk ]]; then
      features=(-DMVDB_TYPED_BULK_VECTOR_ONLY);modes=(normal sanitized)
    fi
    for mode in "${modes[@]}"; do
      optimize=(-O2)
      if [[ "$mode" == sanitized ]]; then optimize=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-pie -no-pie);fi
      for test in "${tests[@]}"; do
        label="$role-$mode-$test"
        printf '%s\n' "unit/$label" > /out/stage.txt
        printf '%s\n' "${common[@]}" "${features[@]}" "${optimize[@]}" > "/out/unit/$label-flags.txt"
        g++ "${common[@]}" "${features[@]}" "${optimize[@]}" "tests/test_$test.cpp" \
          src/{db,exec,sql,value,flat_index}.cpp -o "/out/unit/$label" \
          2>&1 | tee "/out/unit/$label-build.log"
        ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
          timeout 240 "/out/unit/$label" 2>&1 | tee "/out/unit/$label.log"
        printf '%s\t%s\t%s\n' "$role" "$mode" "$test" >> /out/unit/passed.tsv
      done
    done
  done
  exit 0
fi
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
baseline=e020fdeb67dd8b64b77f0f8c2905f137f0112bba
revision=$(git rev-parse HEAD)
git cat-file -e "$baseline^{commit}"
out="$repo/bulk-guard-confirmation-results"
test ! -e "$out"
mkdir -p "$out/build-input"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-bulk-guard-XXXXXX")
container=
printf '%s\n' archive > "$out/stage.txt"
finish() {
  local code=$?
  trap - EXIT
  set +e
  if [[ -n "$container" ]]; then docker rm -v "$container";fi
  python3 "$repo/minivec/tools/bulk_guard_proof.py" collect "$out" "$code"
  local report_code=$?
  if [[ "$code" == 0 && "$report_code" != 0 ]]; then code=$report_code;fi
  if [[ "$report_code" != 0 ]]; then
    printf '{"exit_code":%d,"report_collection_exit_code":%d,"automatic_adoption":false}\n' \
      "$code" "$report_code" > "$out/collection-error.json"
  fi
  exit "$code"
}
trap finish EXIT
for role in baseline candidate; do
  source=$baseline;[[ "$role" == candidate ]] && source=$revision
  mkdir "$scratch/$role"
  git archive "$source" Dockerfile .dockerignore minivec/src minivec/third_party \
    minivec/tests minivec/tools protocol_check | tar -x -C "$scratch/$role"
done
if ! grep -q MVDB_TYPED_BULK_VECTOR_ONLY "$scratch/candidate/minivec/src/exec.cpp"; then
  echo 'Bulk typed candidate is absent from this revision.' >&2
  exit 2
fi
git status --porcelain --untracked-files=no > "$out/worktree-status.txt"
lscpu > "$out/lscpu.txt"
helper="$scratch/candidate/minivec/tools/bulk_guard_proof.py"
python3 "$helper" init "$out" "$scratch" "$revision"
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
docker pull "$compiler"
for role in baseline bulk; do
  snapshot=candidate;[[ "$role" == baseline ]] && snapshot=baseline
  target="$out/$role-pgo"
  mkdir -p "$target/training"
  image="minivec:bulk-guard-$(basename "$scratch")-$role"
  printf '%s\n' "build/$role" > "$out/stage.txt"
  started=$(date +%s%N)
  docker build --no-cache --network none --platform linux/amd64 --progress=plain \
    --build-arg ENABLE_PGO=1 --build-arg FEATURE_FLAGS= --target build \
    -f "$out/build-input/Dockerfile.$role" -t "$image" "$scratch/$snapshot" \
    2>&1 | tee "$target/build.log"
  stopped=$(date +%s%N)
  printf '{"nanoseconds":%s,"pgo_enabled":true,"status":"passed"}\n' "$((stopped-started))" > "$target/build-cost.json"
  docker image inspect "$image" > "$target/image.json"
  container=$(docker create --entrypoint /bin/true "$image")
  docker cp "$container:/minivec" "$target/minivec"
  docker cp "$container:/src/src" "$target/built-source"
  docker cp "$container:/src/third_party" "$target/built-third-party"
  docker cp "$container:/pgo" "$target/training/profile-data"
  for name in train.cpp run-training.sh corpus corpus-sha256.txt profile-sha256.txt verification.jsonl \
    instrumented-minivec instrumented-binary-sha256.txt effective-flags.txt unstripped-minivec symbols.txt sections.txt; do
    docker cp "$container:/training/$name" "$target/training/$name"
  done
  for threads in 1 2; do
    for phase in 0 1 2; do
      docker cp "$container:/training/responses-$threads-$phase.jsonl" "$target/training/"
      docker cp "$container:/training/stderr-$threads-$phase.log" "$target/training/"
    done
  done
  docker rm -v "$container";container=
  sha256sum "$target/minivec" > "$target/binary-sha256.txt"
done
python3 "$helper" verify "$out" "$scratch"
printf '%s\n' source-diagnostic-sanitizer-gates > "$out/stage.txt"
docker run --rm --network none --platform linux/amd64 --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$scratch/candidate,target=/src,readonly" \
  --mount "type=bind,source=$out,target=/out" --workdir /src --entrypoint /bin/bash \
  "$compiler" /src/minivec/tools/ci_bulk_guard_confirmation.sh --unit 2>&1 | tee "$out/source-gates.log"
python3 "$helper" gates "$out" "$scratch"
printf '%s\n' interleaved-192-pairs > "$out/stage.txt"
python3 "$scratch/candidate/minivec/tools/benchmark_bulk_guard.py" --binaries "$out" \
  --build-context "$out/context.json" --output "$out/measurements.json" 2>&1 | tee "$out/measurements.log"
# Repeat source/profile/flags checks after all test/measurement activity.
python3 "$helper" verify "$out" "$scratch"
printf '%s\n' complete > "$out/stage.txt"
