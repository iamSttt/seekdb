#!/usr/bin/env bash
# Actual-root-image and native safety validation, no performance or score claim.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="${MINIVEC_REPORTS:?missing report directory}"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-spill-diagnostic-XXXXXX")
export RUNTIME_COMMIT
RUNTIME_COMMIT=$(git rev-parse HEAD)
image="minivec:spill-diagnostic-${RUNTIME_COMMIT:0:12}"
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
git rev-parse HEAD > "$out/runtime-commit.txt"
git ls-files -z Dockerfile .dockerignore minivec/src minivec/third_party minivec/tests minivec/tools/ci_score.sh minivec/tools/ci_spill_diagnostic_1114.sh | xargs -0 sha256sum > "$out/input-sha256.txt"
git rev-parse HEAD:.github/workflows/minivec-score.yml > "$out/workflow-blob.txt"
lscpu > "$out/lscpu.txt"
docker version > "$out/docker-version.txt"
g++ --version > "$out/native-compiler.txt"
printf '%s\n' 'Pinned base 9ed5baa1b1363ebd388850e74271b353954902da; only db.cpp/flat_index.cpp/flat_index.h runtime spill-cache overlay; no batch write; diagnostics only; no paired performance or formal submission.' > "$out/scope.txt"

docker build --platform linux/amd64 --progress=plain -t "$image" . 2>&1 | tee "$out/docker-build.txt"
docker image inspect "$image" > "$out/image-inspect.json"
container=$(docker create "$image")
docker cp "$container:/minivec" "$scratch/image-binary"
docker rm "$container"
sha256sum "$scratch/image-binary" > "$out/image-binary-sha256.txt"
python3 protocol_check/run.py --image "$image" --handshake-only --output "$out/image-handshake" 2>&1 | tee "$out/handshake.txt"
python3 protocol_check/run.py --image "$image" --output "$out/image-public" 2>&1 | tee "$out/public.txt"
python3 minivec/tests/image_interactive.py --image "$image" --output "$out/image-interactive" 2>&1 | tee "$out/interactive.txt"
python3 minivec/tests/json_protocol.py --binary "$scratch/image-binary" --timeout 15 2>&1 | tee "$out/image-json.txt"
python3 minivec/tests/topk_bounds.py --binary "$scratch/image-binary" 2>&1 | tee "$out/image-topk.txt"
MVDB_QUERY_THREADS=2 python3 minivec/tests/topk_bounds.py --binary "$scratch/image-binary" 2>&1 | tee "$out/image-topk-parallel.txt"

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
g++ -O1 -g -std=c++17 -pthread -Isrc -fno-omit-frame-pointer \
  -fsanitize=address,undefined -no-pie tests/test_cross_table_spill.cpp \
  src/db.cpp src/flat_index.cpp src/value.cpp -o "$scratch/cross-spill-sanitized" > "$out/cross-spill-sanitized-build.txt" 2>&1
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/cross-spill-sanitized" > "$out/cross-spill-sanitized.txt" 2>&1
g++ -O1 -g -std=c++17 -pthread -Isrc -fno-omit-frame-pointer \
  -fsanitize=address,undefined -no-pie tests/test_recovery_regressions.cpp "${sources[@]}" \
  -o "$scratch/recovery-sanitized" > "$out/recovery-sanitized-build.txt" 2>&1
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$scratch/recovery-sanitized" --wal-staging > "$out/recovery-sanitized.txt" 2>&1
sha256sum "$scratch/"spill-* "$scratch/"storage-* "$scratch/"*-sanitized "$scratch/payload-scope" > "$out/native-binary-sha256.txt"
cd "$repo"
git diff --exit-code -- Dockerfile .dockerignore minivec/src minivec/third_party .github/workflows
python3 - <<'PYSUMMARY'
import json, os
from pathlib import Path
root=Path(os.environ['MINIVEC_REPORTS'])
result={'status':'passed','kind':'spill_only_native_and_actual_image_diagnostic',
 'runtime_commit':os.environ['RUNTIME_COMMIT'],
 'base_commit':'9ed5baa1b1363ebd388850e74271b353954902da',
 'image_binary_sha256':(root/'image-binary-sha256.txt').read_text().split()[0],
 'workflow_blob':(root/'workflow-blob.txt').read_text().strip(),
 'performance_measured':False,'formal_submission':False,'release_recommendation':False,
 'checks':['actual root Docker build','actual image ready and20 public cases','interactive and JSON protocol',
 'actual image exact TopK serial/parallel','spill cache widths/extremes/budgets/allocation/read-write faults and mutation/restart',
 'shared index budgets 16MiB/8192B/1B','payload-resident disk-index exclusion','spill/cross-table/recovery ASan+UBSan'],
 'limitations':['No performance pairs or score prediction','No hidden formal cases','ASan leak detection disabled',
 'New standalone per-record writer fault assertions replace inapplicable R batch-tail expectations']}
(root/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
PYSUMMARY
