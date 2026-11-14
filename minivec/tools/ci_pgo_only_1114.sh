#!/usr/bin/env bash
# Independent PGO build-factor experiment. Runtime code is unchanged from760f.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
export PYTHONDONTWRITEBYTECODE=1
export MINIVEC_REPORTS="$repo/protocol-results"
test ! -e "$MINIVEC_REPORTS"
mkdir "$MINIVEC_REPORTS"
out="$MINIVEC_REPORTS/pgo-only"
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-pgo-only-XXXXXX")
source=$(git rev-parse HEAD)
base=760f66220f2c7186b4ba295f9e3065650e549f9e
finish() {
  local code=$?
  trap - EXIT
  set +e
  python3 - "$out" "$code" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]);code=int(sys.argv[2])
(p/'execution.json').write_text(json.dumps({'exit_code':code,'status':'completed' if code==0 else 'failed_or_blocked','release_gate_passed':False,'automatic_adoption':False,'official_score':None},indent=2)+'\n')
for f in sorted(p.parent.rglob('*')):
    if f.is_file() and f.suffix in {'.json','.txt'}:
        print('MINIVEC_REPORT_BEGIN '+json.dumps(f.relative_to(p.parent).as_posix()))
        print(f.read_text(encoding='utf8',errors='replace'))
        print('MINIVEC_REPORT_END')
PY
  exit "$code"
}
trap finish EXIT
git cat-file -e "$base^{commit}" || git fetch --no-tags --depth=1 origin "$base"
python3 -B minivec/tools/pgo_only1114/verify_source.py | tee "$out/static-identity.txt"
python3 -B minivec/tools/pgo_only1114/test_adapter.py | tee "$out/static-adapter-tests.txt"
cp validation/pgo-only-source-20261114.json "$out/source-manifest.json"
cp validation/pgo-only-plan-20261114.json "$out/frozen-plan.json"
cp validation/pgo-only-adapter-20261114.json "$out/adapter-manifest.json"
lscpu > "$out/lscpu.txt"
g++ --version > "$out/compiler.txt"
git rev-parse HEAD > "$out/source-commit.txt"
sha256sum Dockerfile .dockerignore minivec/tools/pgo/* minivec/tools/pgo_only1114/*.py minivec/tools/ci_pgo_only_1114.sh > "$out/adapter-input-sha256.txt"

# Each --no-cache builder trains once. The release-only commands reuse that
# just-built stage; compare extracted builder and release ELF bytes explicitly.
for label in A B; do
  d="$out/$label"
  mkdir "$d"
  builder="minivec:pgo-only-builder-${GITHUB_RUN_ID:-local}-$label"
  image="minivec:pgo-only-${GITHUB_RUN_ID:-local}-$label"
  docker build --no-cache --platform linux/amd64 --target build --build-arg ENABLE_PGO=1 --progress=plain -t "$builder" . 2>&1 | tee "$d/builder-build.log"
  docker image inspect "$builder" > "$d/builder-image.json"
  container=$(docker create "$builder")
  docker cp "$container:/minivec" "$scratch/builder-$label"
  docker cp "$container:/training/verification.jsonl" "$d/training-verification.txt"
  docker cp "$container:/training/profile-sha256.txt" "$d/profile-sha256.txt"
  docker cp "$container:/training/corpus-sha256.txt" "$d/corpus-sha256.txt"
  docker cp "$container:/training/corpus/manifest.json" "$d/corpus-manifest.json"
  for threads in 1 2; do
    for phase in 0 1 2; do
      docker cp "$container:/training/stderr-$threads-$phase.log" "$d/training-stderr-$threads-$phase.txt"
    done
  done
  docker rm "$container"
  sha256sum "$scratch/builder-$label" | cut -d ' ' -f1 > "$d/builder-ELF-sha256.txt"
  docker build --platform linux/amd64 --target release --build-arg ENABLE_PGO=1 --progress=plain -t "$image" . 2>&1 | tee "$d/release-build.log"
  docker image inspect "$image" > "$d/image.json"
  container=$(docker create "$image")
  docker cp "$container:/minivec" "$scratch/candidate-$label"
  docker rm "$container"
  chmod +x "$scratch/candidate-$label"
  python3 -B minivec/tools/pgo_only1114/build_audit.py record --root "$out" --label "$label" --source "$source" --binary "$scratch/candidate-$label"
done
# Stop before any performance if fresh PGO products differ. Never select one.
python3 -B minivec/tools/pgo_only1114/build_audit.py compare --root "$out"

# Build the exact original non-PGO root as source-factor control.
mkdir "$scratch/baseline-context"
git archive "$base" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/baseline-context"
baseline_image="minivec:pgo-only-control-${GITHUB_RUN_ID:-local}"
docker build --no-cache --platform linux/amd64 --progress=plain -t "$baseline_image" "$scratch/baseline-context" 2>&1 | tee "$out/baseline-build.log"
docker image inspect "$baseline_image" > "$out/baseline-image.json"
container=$(docker create "$baseline_image")
docker cp "$container:/minivec" "$scratch/baseline"
docker rm "$container"
chmod +x "$scratch/baseline"
python3 -B minivec/tools/pgo_only1114/build_audit.py baseline --root "$out" --binary "$scratch/baseline"

# Check both actual release images, even if their ELF hashes match.
for label in A B; do
  d="$out/$label"
  image="minivec:pgo-only-${GITHUB_RUN_ID:-local}-$label"
  binary="$scratch/candidate-$label"
  python3 protocol_check/run.py --image "$image" --handshake-only --output "$d/handshake"
  python3 protocol_check/run.py --image "$image" --output "$d/public"
  python3 minivec/tests/image_interactive.py --image "$image" --output "$d/interactive"
  python3 minivec/tests/json_protocol.py --binary "$binary" --timeout 15 | tee "$d/json-protocol.txt"
  python3 minivec/tests/topk_bounds.py --binary "$binary" | tee "$d/topk.txt"
  MVDB_QUERY_THREADS=2 python3 minivec/tests/topk_bounds.py --binary "$binary" | tee "$d/topk-parallel.txt"
  python3 minivec/tests/shared_bound_topk.py --binary "$binary" --output "$d/shared-bound.json"
  python3 minivec/tests/validated_request_diff.py --baseline "$scratch/baseline" --candidate "$binary" --output "$d/request-differential.json"
  python3 minivec/tests/short_decimal_protocol.py --baseline "$scratch/baseline" --candidate "$binary" --output "$d/numeric-protocol.json"
done

# Source-code safety regression once; source bytes are identical for both builds.
make -C minivec -j2 test 2>&1 | tee "$out/make-test.log"
bash minivec/tools/ci_intcap_guard_1114.sh
bash minivec/tools/ci_spill_guard_1114.sh
g++ -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src minivec/tests/test_recovery_regressions.cpp minivec/src/db.cpp minivec/src/exec.cpp minivec/src/sql.cpp minivec/src/value.cpp minivec/src/flat_index.cpp -o "$scratch/recovery-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 120 "$scratch/recovery-san" --wal-staging | tee "$out/recovery-san.txt"
g++ -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src minivec/tests/test_query_pool_safety.cpp -Wl,--wrap=pthread_create -o "$scratch/query-pool-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 25 "$scratch/query-pool-san" | tee "$out/query-pool-san.txt"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 25 "$scratch/query-pool-san" --create-fails | tee "$out/query-pool-create-fails-san.txt"
g++ -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src minivec/tests/test_cross_table_spill.cpp minivec/src/db.cpp minivec/src/flat_index.cpp minivec/src/value.cpp -o "$scratch/cross-spill-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 120 "$scratch/cross-spill-san" | tee "$out/cross-spill-san.txt"
g++ -std=c++17 -O1 -pthread -fsanitize=undefined -fno-sanitize-recover=all -Iminivec/src minivec/src/*.cpp -o "$scratch/minivec-ubsan"
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 python3 minivec/tests/json_protocol.py --binary "$scratch/minivec-ubsan" --timeout 15 | tee "$out/fullbinary-json-ubsan.txt"
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 python3 minivec/tests/shared_bound_topk.py --binary "$scratch/minivec-ubsan" --output "$out/shared-bound-ubsan.json"

gcc -O2 -Wall -Wextra -static minivec/tools/retained_guards1114/vendor/cgroup_exec.c -o "$scratch/cgroup-exec"
# Preserve both arms/builds even when a numerical guard fails for A.
failed=0
for label in A B; do
  set +e
  sudo --preserve-env=PATH python3 -B minivec/tools/pgo_only1114/source_factor.py \
    --baseline "$scratch/baseline" --baseline-build-proof "$out/baseline-proof.json" \
    --candidate "$scratch/candidate-$label" --candidate-source "$source" \
    --candidate-evidence "$out/$label/build-proof.json" --launcher "$scratch/cgroup-exec" \
    --output "$out/$label/source-factor.json"
  code=$?
  set -e
  if [[ $code != 0 ]]; then failed=1; fi
done
python3 -B minivec/tools/pgo_only1114/verify_source.py | tee "$out/final-static-identity.txt"
python3 -B minivec/tools/pgo_only1114/build_audit.py finish --root "$out"
test "$failed" = 0
