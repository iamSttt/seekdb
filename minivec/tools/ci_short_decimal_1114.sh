#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="${MINIVEC_REPORTS:?}/short-decimal"
mkdir -p "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-short-decimal-XXXXXX")
prototype=minivec/tools/short_decimal_experiment
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
python3 "$prototype/verify_source.py" minivec/src/sql.cpp > "$out/source-binding.json"
python3 "$prototype/make_oracle.py" --output "$scratch/oracle.tsv" > "$out/oracle-generation.json"
python3 - "$scratch/oracle.tsv" "$prototype/oracle.json" <<'PY'
import hashlib,json,sys
from pathlib import Path
assert hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest()==json.loads(Path(sys.argv[2]).read_text())['sha256']
PY
# Same optimizing flags/library as the root image, not a special FP build.
docker run --rm -v "$repo:/work:ro" -v "$scratch:/out" -w /work "$compiler" \
  g++ -O3 -std=c++17 -fno-math-errno -static -pthread -Iminivec/src \
  "$prototype/test_short_decimal.cpp" -o /out/oracle-rootflags
"$scratch/oracle-rootflags" "$scratch/oracle.tsv" | tee "$out/oracle-rootflags.txt"
# Additional strict correctness only; these flags never enter the micro build.
docker run --rm -v "$repo:/work:ro" -v "$scratch:/out" -w /work "$compiler" \
  g++ -O3 -std=c++17 -fno-math-errno -frounding-math -static -pthread -Iminivec/src \
  "$prototype/test_short_decimal.cpp" -o /out/oracle-strict
"$scratch/oracle-strict" "$scratch/oracle.tsv" | tee "$out/oracle-strict.txt"
# This additional instrumented build explicitly observes FP control changes.
g++ -O1 -g -std=c++17 -fno-math-errno -frounding-math -fno-omit-frame-pointer -fsanitize=address,undefined \
  -no-pie -pthread -Iminivec/src "$prototype/test_short_decimal.cpp" -o "$scratch/oracle-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$scratch/oracle-san" "$scratch/oracle.tsv" | tee "$out/oracle-sanitized.txt"
docker run --rm -v "$repo:/work:ro" -v "$scratch:/out" -w /work "$compiler" \
  g++ -O3 -std=c++17 -fno-math-errno -static -pthread -Iminivec/src \
  -save-temps=obj -fverbose-asm "$prototype/bench_short_decimal.cpp" -o /out/bench
first_cpu=$(python3 -c 'import os;print(min(os.sched_getaffinity(0)))')
taskset -c "$first_cpu" "$scratch/bench" --pairs 8 --target-ms 25 | tee "$out/micro.txt"
cat "$scratch/"*.s > "$out/micro-translation-unit.txt"
sha256sum "$scratch/oracle-rootflags" "$scratch/oracle-strict" "$scratch/oracle-san" "$scratch/bench" > "$out/binary-sha256.txt"
