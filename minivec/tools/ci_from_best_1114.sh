#!/usr/bin/env bash
# Additional tests and measurements after the ordinary root-image regression gate.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="$repo/protocol-results/from-best"
mkdir -p "$out"
if [[ -f validation/combined-plan-20261114.json ]]; then
  python3 -B minivec/tools/confirmation1114/runner_identity.py capture "$out/runner.json"
fi
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-from-best-XXXXXX")
baseline=ee299adb296a2922d01982aa6c5707d37e767b41
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
git fetch --no-tags --depth=1 origin "$baseline"
mkdir "$scratch/baseline"
git archive "$baseline" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/baseline"
git diff "$baseline" HEAD -- Dockerfile .dockerignore minivec/third_party > "$out/build-input-diff.txt"
test ! -s "$out/build-input-diff.txt"
git diff "$baseline" HEAD -- minivec/src > "$out/runtime-change.txt"
docker build --platform linux/amd64 --progress=plain -t minivec:ee-1114 "$scratch/baseline" 2>&1 | tee "$out/baseline-build.log"
for role in baseline candidate; do
  image="$IMAGE"
  if [ "$role" = baseline ]; then image=minivec:ee-1114; fi
  container=$(docker create "$image")
  docker cp "$container:/minivec" "$scratch/$role-binary"
  docker rm "$container"
  docker image inspect "$image" > "$out/$role-image.json"
done
sha256sum "$scratch/"*-binary > "$out/binary-sha256.txt"
echo '9a437ae19ff20dbd7ab2e3242ce16c3b814d80536a0768c628d93d53a7b14bbd  '"$scratch/baseline-binary" | sha256sum -c -
# Addresses come from the already recorded, same-hash EE software profile.
# This immutable .text window needs no new profiling or source instrumentation.
objdump -d --start-address=0x42dc00 --stop-address=0x42e600 "$scratch/baseline-binary" > "$out/ee-hot-lambda-disassembly.txt"

# Combined CI already runs these same native/sanitizer/disabled-mode guards
# through ci_intcap_guard_1114.sh; do not compile and repeat them a second time.
if [[ ! -f validation/combined-plan-20261114.json ]]; then
# The new integer kernels are exact: validate raw sums, guards, and bound decisions.
docker run --rm -v "$repo:/work:ro" -v "$scratch:/out" -w /work "$compiler" \
  g++ -O3 -std=c++17 -fno-math-errno -static -pthread -Iminivec/src minivec/tests/test_fixed128_quantized.cpp -o /out/fixed128-test
"$scratch/fixed128-test" --bench | tee "$out/fixed128.txt"
g++ -O1 -g -std=c++17 -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src minivec/tests/test_fixed128_quantized.cpp -o "$scratch/fixed128-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/fixed128-san" | tee "$out/fixed128-sanitized.txt"
for mode in MVDB_DISABLE_AVX512_QUANTIZED MVDB_DISABLE_FIXED128_QUANTIZED; do
  docker run --rm -v "$repo:/work:ro" -v "$scratch:/out" -w /work "$compiler" \
    g++ -O3 -std=c++17 -static -Iminivec/src -D"$mode" minivec/tests/test_fixed128_quantized.cpp -o "/out/fixed128-$mode"
  "$scratch/fixed128-$mode" | tee "$out/fixed128-$mode.txt"
done

fi

# Compare complete request/response sequences and post-error effects against EE.
python3 minivec/tests/validated_request_diff.py --baseline "$scratch/baseline-binary" --candidate "$scratch/candidate-binary" --output "$out/request-differential.json"
python3 minivec/tests/short_decimal_protocol.py --baseline "$scratch/baseline-binary" --candidate "$scratch/candidate-binary" --output "$out/short-decimal-protocol.json"
python3 minivec/tests/shared_bound_topk.py --binary "$scratch/candidate-binary" --output "$out/shared-bound-topk.json"

# Same-machine paired screening, separate DB-only CPU quotas, all samples retained.
gcc -O2 -Wall -Wextra -static minivec/tools/research1114/cgroup_exec.c -o "$scratch/cgroup-exec"
sudo --preserve-env=PATH python3 minivec/tools/best_screen_1114.py \
  --baseline "$scratch/baseline-binary" --candidate "$scratch/candidate-binary" \
  --candidate-source "$RUNTIME_COMMIT" --launcher "$scratch/cgroup-exec" --output "$out/screen.json"

# Final combination uses the separately frozen retained-R protection stage.
# Independent source-factor experiments keep the original 9ed ablation below.
if [[ -f validation/combined-plan-20261114.json ]]; then
  bash "$repo/minivec/tools/retained_guards1114/ci_retained_guards.sh" \
    "$repo" "$scratch/candidate-binary" "$RUNTIME_COMMIT" \
    "$out/screen.json.d/candidate-file-identity.json" "$out/retained-r"
  exit 0
fi

# Isolate short-decimal parsing from its immediate runtime parent.
pre_share=9ed5baa1b1363ebd388850e74271b353954902da
git fetch --no-tags --depth=1 origin "$pre_share"
mkdir "$scratch/pre-share"
git archive "$pre_share" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/pre-share"
docker build --platform linux/amd64 --progress=plain -t minivec:pre-share-1114 "$scratch/pre-share" 2>&1 | tee "$out/pre-share-build.log"
container=$(docker create minivec:pre-share-1114)
docker cp "$container:/minivec" "$scratch/pre-share-binary"
docker rm "$container"
docker image inspect minivec:pre-share-1114 > "$out/pre-share-image.json"
sha256sum "$scratch/pre-share-binary" > "$out/pre-share-binary-sha256.txt"
sudo --preserve-env=PATH python3 minivec/tools/best_screen_1114.py \
  --baseline "$scratch/pre-share-binary" --baseline-source "$pre_share" \
  --candidate "$scratch/candidate-binary" --candidate-source "$RUNTIME_COMMIT" \
  --launcher "$scratch/cgroup-exec" --output "$out/shared-ablation.json"

# EE sampling runs in its separate diagnostic workflow; do not repeat it or
# mix sampling overhead into the paired measurements above.
