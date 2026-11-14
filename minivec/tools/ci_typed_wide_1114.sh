#!/usr/bin/env bash
# Runs after the full ordinary root correctness gate; isolated source factor only.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="$repo/protocol-results/typed-wide"
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-typed-wide-XXXXXX")
baseline=760f66220f2c7186b4ba295f9e3065650e549f9e
candidate=9595e1477af1325795b0f9fed52e6a44bb217b1c
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
git fetch --no-tags --depth=1 origin "$baseline"
for role in baseline candidate; do
  commit="$baseline"; if [[ $role == candidate ]]; then commit="$candidate"; fi
  mkdir "$scratch/$role"
  git archive "$commit" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/$role"
done
docker build --no-cache --platform linux/amd64 --progress=plain -t minivec:typed-parent-1114 "$scratch/baseline" 2>&1 | tee "$out/parent-build.txt"
for role in baseline candidate; do
  image="$IMAGE"; if [[ $role == baseline ]]; then image=minivec:typed-parent-1114; fi
  container=$(docker create "$image")
  docker cp "$container:/minivec" "$scratch/$role-binary"
  docker rm "$container"
  docker image inspect "$image" > "$out/$role-image.json"
  python3 -B minivec/tools/typed_wide1114/verify_root.py --role "$role" \
    --context "$scratch/$role" --binary "$scratch/$role-binary" \
    --image-inspect "$out/$role-image.json" --output "$out/$role-proof.json"
done

# Root GCC13.3 O3 writer and executor, keeping assertions enabled.
docker run --rm -v "$repo:/work:ro" -v "$scratch:/out" -w /work "$compiler" \
  g++ -O3 -std=c++17 -fno-math-errno -static -pthread -Iminivec/src \
  minivec/tests/test_json_row_writer.cpp -o /out/writer-o3
"$scratch/writer-o3" | tee "$out/writer-o3.txt"
sources=(minivec/src/db.cpp minivec/src/exec.cpp minivec/src/flat_index.cpp minivec/src/sql.cpp minivec/src/value.cpp)
docker run --rm -v "$repo:/work:ro" -v "$scratch:/out" -w /work "$compiler" \
  g++ -O3 -std=c++17 -fno-math-errno -static -pthread -Iminivec/src \
  minivec/tests/test_typed_response.cpp "${sources[@]}" -o /out/response-o3
"$scratch/response-o3" | tee "$out/response-o3.txt"
g++ -O1 -g -std=c++17 -pthread -Iminivec/src -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  minivec/tests/test_json_row_writer.cpp -o "$scratch/writer-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/writer-san" | tee "$out/writer-san.txt"
g++ -O1 -g -std=c++17 -pthread -Iminivec/src -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  minivec/tests/test_typed_response.cpp "${sources[@]}" -o "$scratch/response-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/response-san" | tee "$out/response-san.txt"
# One-shot allocation failures including lazy state and all response allocations.
docker run --rm -v "$repo:/work:ro" -v "$scratch:/out" -w /work "$compiler" \
  g++ -O3 -std=c++17 -fno-math-errno -static -pthread -Iminivec/src \
  minivec/tests/test_typed_oom.cpp "${sources[@]}" -o /out/oom-o3
"$scratch/oom-o3" | tee "$out/oom-o3.txt"
g++ -O1 -g -std=c++17 -pthread -Iminivec/src -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  minivec/tests/test_typed_oom.cpp "${sources[@]}" -o "$scratch/oom-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/oom-san" | tee "$out/oom-san.txt"
for role in baseline candidate; do
  python3 -B minivec/tests/test_typed_response.py --binary "$scratch/$role-binary" --output "$out/utf8-$role.json"
done
python3 -B minivec/tests/typed_wide_1114.py --baseline "$scratch/baseline-binary" \
  --candidate "$scratch/candidate-binary" --output "$out/byte-oracle.json"
python3 -B minivec/tests/validated_request_diff.py --baseline "$scratch/baseline-binary" \
  --candidate "$scratch/candidate-binary" --expected-raw-nul-changes 0 --output "$out/request-differential.json"
python3 -B minivec/tests/short_decimal_protocol.py --baseline "$scratch/baseline-binary" \
  --candidate "$scratch/candidate-binary" --output "$out/numeric-protocol.json"
python3 -B minivec/tests/shared_bound_topk.py --binary "$scratch/candidate-binary" --output "$out/shared-bound-topk.json"
sha256sum "$scratch/"*-binary "$scratch/"*-o3 "$scratch/"*-san > "$out/binary-sha256.txt"
gcc -O2 -Wall -Wextra -static minivec/tools/research1114/cgroup_exec.c -o "$scratch/cgroup-exec"
sudo --preserve-env=PATH python3 -B minivec/tools/typed_wide1114/run_ablation.py \
  --baseline "$scratch/baseline-binary" --candidate "$scratch/candidate-binary" \
  --baseline-evidence "$out/baseline-proof.json" --candidate-evidence "$out/candidate-proof.json" \
  --launcher "$scratch/cgroup-exec" --output "$out/ablation.json"
python3 -B minivec/tools/typed_wide1114/verify_source.py
