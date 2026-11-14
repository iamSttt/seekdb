#!/usr/bin/env bash
# Safety checks only for the isolated cap feature in the current combined tree.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="${1:-${MINIVEC_REPORTS:-$repo/protocol-results}/intcap-guard}"
test ! -e "$out"
mkdir -p "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-intcap-guard-XXXXXX")
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
baseline=9ed5baa1b1363ebd388850e74271b353954902da
git rev-parse HEAD > "$out/runtime-commit.txt"
sha256sum minivec/src/quantized_bound.h minivec/src/distance.h \
  minivec/tests/test_intcap_equivalence.cpp minivec/tests/test_quantized_bound.cpp \
  minivec/tests/test_fixed128_quantized.cpp minivec/tools/ci_intcap_guard_1114.sh > "$out/input-sha256.txt"
for test in test_intcap_equivalence test_quantized_bound test_fixed128_quantized; do
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
git cat-file -e "$baseline^{commit}" || git fetch --no-tags --depth=1 origin "$baseline"
mkdir "$scratch/baseline"
git archive "$baseline" minivec/src | tar -x -C "$scratch/baseline"
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
python3 - "$out" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1])
size=int((p/'layout-candidate.txt').read_text())
assert size==int((p/'layout-baseline.txt').read_text())==88
report={'status':'passed','scope':'Current combined-tree cap/kernel correctness and layout only; no performance or release inference.',
        'runtime_commit':(p/'runtime-commit.txt').read_text().strip(),
        'tests':['legacy cap equivalence standalone','existing complete quantized bound','existing fixed1286600',
                 'same three ASan+UBSan tests','fixed128 AVX512-disabled and fixed128-disabled','pinned GCC layout equality'],
        'sizeof_baseline_and_candidate':size,'leak_detection_enabled':False,'database_pairs':0,
        'release_gate_passed':False,'official_score':None}
(p/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
PY
