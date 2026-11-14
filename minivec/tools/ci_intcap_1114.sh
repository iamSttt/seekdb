#!/usr/bin/env bash
# Isolated integer-cap source ablation after unchanged ordinary native/image gates.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="$MINIVEC_REPORTS/intcap"
mkdir -p "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-intcap-XXXXXX")
baseline=9ed5baa1b1363ebd388850e74271b353954902da
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
cp minivec/tools/intcap1114/plan.json "$out/frozen-plan.json"
sha256sum minivec/tools/intcap1114/plan.json > "$out/frozen-plan-sha256.txt"
python3 -B minivec/tools/intcap1114/evaluate.py verify --output "$out/source-proof.json"
mkdir "$scratch/baseline"
git archive "$baseline" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/baseline"
docker build --no-cache --platform linux/amd64 --progress=plain -t minivec:intcap-parent-1114 "$scratch/baseline" 2>&1 | tee "$out/baseline-build.txt"
for role in baseline candidate; do
  image="$IMAGE"
  if [ "$role" = baseline ]; then image=minivec:intcap-parent-1114; fi
  container=$(docker create "$image")
  docker cp "$container:/minivec" "$scratch/$role-binary"
  docker rm "$container"
  docker image inspect "$image" > "$out/$role-image.json"
  python3 -B minivec/tools/intcap1114/evaluate.py proof --role "$role" --binary "$scratch/$role-binary" \
    --image-inspect "$out/$role-image.json" --output "$out/$role-build-proof.json"
done
sha256sum "$scratch/"*-binary > "$out/binary-sha256.txt"

# Reuse exact legacy floating-cap equivalence tests and unchanged fixed128 oracle.
for test in test_quantized_bound test_fixed128_quantized; do
  docker run --rm -v "$repo:/work:ro" -v "$scratch:/out" -w /work "$compiler" \
    g++ -O3 -std=c++17 -fno-math-errno -static -pthread -Iminivec/src "minivec/tests/$test.cpp" -o "/out/$test"
  "$scratch/$test" | tee "$out/$test.txt"
  g++ -O1 -g -std=c++17 -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie \
    -Iminivec/src "minivec/tests/$test.cpp" -o "$scratch/$test-san"
  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/$test-san" | tee "$out/$test-san.txt"
done
for mode in MVDB_DISABLE_AVX512_QUANTIZED MVDB_DISABLE_FIXED128_QUANTIZED; do
  docker run --rm -v "$repo:/work:ro" -v "$scratch:/out" -w /work "$compiler" \
    g++ -O3 -std=c++17 -static -Iminivec/src -D"$mode" minivec/tests/test_fixed128_quantized.cpp -o "/out/fixed128-$mode"
  "$scratch/fixed128-$mode" | tee "$out/fixed128-$mode.txt"
done
cat > "$scratch/layout.cpp" <<'CPP'
#include "quantized_bound.h"
#include <iostream>
int main() {std::cout<<sizeof(mvdb::QuantizedQueryBound)<<"\n";}
CPP
for role in baseline candidate; do
  context="$repo/minivec"
  if [ "$role" = baseline ]; then context="$scratch/baseline/minivec"; fi
  docker run --rm -v "$context:/src:ro" -v "$scratch:/out" -w /src "$compiler" \
    g++ -O3 -std=c++17 -static -Isrc /out/layout.cpp -o "/out/layout-$role"
  "$scratch/layout-$role" > "$out/layout-$role.txt"
done
cmp "$out/layout-baseline.txt" "$out/layout-candidate.txt"
python3 -B minivec/tests/validated_request_diff.py --baseline "$scratch/baseline-binary" \
  --candidate "$scratch/candidate-binary" --output "$out/request-differential.json"
python3 -B minivec/tests/shared_bound_topk.py --binary "$scratch/candidate-binary" --output "$out/fraction-topk.json"

gcc -O2 -Wall -Wextra -static minivec/tools/research1114/cgroup_exec.c -o "$scratch/cgroup-exec"
sudo --preserve-env=PATH python3 -B minivec/tools/best_screen_1114.py \
  --baseline "$scratch/baseline-binary" --baseline-source "$baseline" \
  --baseline-evidence "$out/baseline-build-proof.json" \
  --candidate "$scratch/candidate-binary" --candidate-source "$RUNTIME_COMMIT" \
  --candidate-evidence "$out/candidate-build-proof.json" \
  --launcher "$scratch/cgroup-exec" --output "$out/screen.json"
python3 -B minivec/tools/intcap1114/evaluate.py evaluate --screen "$out/screen.json" --output "$out/decision.json"
python3 -B minivec/tools/intcap1114/evaluate.py verify --output "$out/source-proof-after.json"
