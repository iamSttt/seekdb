#!/usr/bin/env bash
# Archived rejected experiment: optional standalone rerun, not a release gate.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="${MINIVEC_REPORTS:?}/lazy128"
mkdir -p "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-lazy128-XXXXXX")
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
for mode in default MVDB_DISABLE_AVX512_BOUND MVDB_DISABLE_SIMD_BOUND; do
  flags=()
  if [[ "$mode" != default ]]; then flags+=("-D$mode"); fi
  docker run --rm -v "$repo:/work:ro" -v "$scratch:/out" -w /work "$compiler" \
    g++ -O3 -std=c++17 -fno-math-errno -Wall -Wextra -static -pthread \
    -Iminivec/tools/lazy128_rejected -Iminivec/src "${flags[@]}" minivec/tools/lazy128_rejected/test_lazy_interval128.cpp -o "/out/test-$mode"
  "$scratch/test-$mode" | tee "$out/oracle-$mode.txt"
done
g++ -O1 -g -std=c++17 -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie \
  -pthread -Iminivec/tools/lazy128_rejected -Iminivec/src minivec/tools/lazy128_rejected/test_lazy_interval128.cpp -o "$scratch/oracle-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$scratch/oracle-san" | tee "$out/oracle-sanitized.txt"
docker run --rm -v "$repo:/work:ro" -v "$scratch:/out" -w /work "$compiler" \
  g++ -O3 -std=c++17 -fno-math-errno -static -pthread -Iminivec/tools/lazy128_rejected -Iminivec/src \
  minivec/tools/lazy128_rejected/bench_lazy_interval128.cpp -o /out/bench
first_cpu=$(python3 -c 'import os;print(min(os.sched_getaffinity(0)))')
taskset -c "$first_cpu" "$scratch/bench" | tee "$out/micro.txt"
sha256sum "$scratch/"* > "$out/binary-sha256.txt"
