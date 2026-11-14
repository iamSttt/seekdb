#!/usr/bin/env bash
# Reusable Linux validation entry: verify this checkout, never infer a formal score.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
export RUNTIME_COMMIT
RUNTIME_COMMIT=$(git rev-parse HEAD)
export IMAGE="${IMAGE:-minivec:verify-${RUNTIME_COMMIT:0:12}}"
# Public drivers require fresh outputs; preserve earlier runs for inspection.
test ! -e protocol-results
mkdir protocol-results
export MINIVEC_REPORTS="$repo/protocol-results"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-checks-XXXXXX")
dump_reports() {
  result=$?
  set +e
  python3 - <<'PYREPORT'
import json, os
from pathlib import Path
root = Path(os.environ['MINIVEC_REPORTS'])
for path in sorted(root.rglob('*')):
    if not path.is_file() or path.suffix not in {'.json','.txt'}:
        continue
    print('MINIVEC_REPORT_BEGIN '+json.dumps(path.relative_to(root).as_posix()))
    print(path.read_text(encoding='utf-8', errors='replace'))
    print('MINIVEC_REPORT_END')
PYREPORT
  exit "$result"
}
trap dump_reports EXIT

git rev-parse HEAD > protocol-results/runtime-commit.txt
git ls-files -z Dockerfile .dockerignore minivec/src minivec/third_party minivec/Makefile minivec/tests minivec/tools/ci_score.sh STDIO_PROTOCOL.md protocol_check mysqltest | xargs -0 sha256sum > protocol-results/input-sha256.txt
lscpu > protocol-results/lscpu.txt
docker version > protocol-results/docker-version.txt
g++ --version > protocol-results/compiler.txt
# Build the submitted root Dockerfile
docker build --platform linux/amd64 --progress=plain -t "$IMAGE" . 2>&1 | tee protocol-results/docker-build.log
docker image inspect "$IMAGE" > protocol-results/image-inspect.json
docker image inspect "$IMAGE" --format '{{.Id}}' | tee protocol-results/image-id.txt

# Official image handshake and unchanged 20 public cases
python3 protocol_check/run.py --image "$IMAGE" --handshake-only --output protocol-results/image-handshake 2>&1 | tee protocol-results/image-handshake.log
python3 protocol_check/run.py --image "$IMAGE" --output protocol-results/image-public 2>&1 | tee protocol-results/image-public.log

# Interactive image requests with stdin kept open
python3 minivec/tests/image_interactive.py --image "$IMAGE" --output protocol-results/image-interactive 2>&1 | tee protocol-results/image-interactive.log

# Existing numeric transaction recovery and sorting regressions
make -C minivec -j2 test 2>&1 | tee protocol-results/source-regressions.log
python3 protocol_check/run.py --command '["./minivec/minivec","--data","{data_dir}"]' --handshake-only --output protocol-results/source-handshake 2>&1 | tee protocol-results/source-handshake.log
python3 protocol_check/run.py --command '["./minivec/minivec","--data","{data_dir}"]' --output protocol-results/source-public 2>&1 | tee protocol-results/source-public.log

# Validate the binary extracted from the actual image
container=$(docker create "$IMAGE")
docker cp "$container:/minivec" "$scratch/image-binary"
docker rm "$container"
sha256sum "$scratch/image-binary" | tee protocol-results/image-binary-sha256.txt
python3 minivec/tests/json_protocol.py --binary "$scratch/image-binary" --timeout 15 2>&1 | tee protocol-results/image-binary-json.log
python3 minivec/tests/topk_bounds.py --binary "$scratch/image-binary" 2>&1 | tee protocol-results/image-binary-topk.log
MVDB_QUERY_THREADS=2 python3 minivec/tests/topk_bounds.py --binary "$scratch/image-binary" 2>&1 | tee protocol-results/image-binary-topk-parallel.log


# Native tests above include all eight staged-WAL cases. Run them explicitly
# again under sanitizers, without weakening error or durability assertions.
reports="$PWD/protocol-results"
g++ -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src minivec/tests/test_recovery_regressions.cpp minivec/src/db.cpp minivec/src/exec.cpp minivec/src/sql.cpp minivec/src/value.cpp minivec/src/flat_index.cpp -o "$scratch/recovery-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 120 "$scratch/recovery-san" --wal-staging 2>&1 | tee "$reports/recovery-san.log"
g++ -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src minivec/tests/test_query_pool_safety.cpp -Wl,--wrap=pthread_create -o "$scratch/query-pool-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 25 "$scratch/query-pool-san" 2>&1 | tee "$reports/query-pool-san.log"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 25 "$scratch/query-pool-san" --create-fails 2>&1 | tee "$reports/query-pool-create-fails-san.log"
g++ -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src minivec/tests/test_cross_table_spill.cpp minivec/src/db.cpp minivec/src/flat_index.cpp minivec/src/value.cpp -o "$scratch/cross-spill-san"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 120 "$scratch/cross-spill-san" 2>&1 | tee "$reports/cross-spill-san.log"
g++ -std=c++17 -O1 -pthread -fsanitize=undefined -fno-sanitize-recover=all -Iminivec/src minivec/src/*.cpp -o "$scratch/minivec-ubsan"
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 python3 minivec/tests/json_protocol.py --binary "$scratch/minivec-ubsan" --timeout 15 2>&1 | tee "$reports/fullbinary-json-ubsan.log"
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 python3 minivec/tests/shared_bound_topk.py --binary "$scratch/minivec-ubsan" --output "$reports/shared-bound-ubsan.json"

bash minivec/tools/ci_from_best_1114.sh

# Cleanup must not change any official fixtures or source while tests run.
git diff --exit-code -- STDIO_PROTOCOL.md protocol_check mysqltest minivec/src minivec/third_party Dockerfile .dockerignore
python3 - <<'PYSUMMARY'
import json, os
from pathlib import Path
report = {
    'runtime_commit': os.environ['RUNTIME_COMMIT'],
    'image_tag': os.environ['IMAGE'],
    'image_id': Path('protocol-results/image-id.txt').read_text().strip(),
    'scope': 'current-checkout build and regression verification; not formal scoring',
    'passed': True,
    'checks': {
        'root_docker_build': True,
        'image_official_handshake_and_public_cases': True,
        'source_official_handshake_and_public_cases': True,
        'live_stdin_errors_utf8_fragment_shutdown_restart_commit_sigkill': True,
        'make_test': True,
        'image_binary_json_and_exact_topk_serial_parallel': True,
        'eight_wal_failure_cases_asan_ubsan': True,
        'query_pool_both_modes_asan_ubsan': True,
        'cross_table_spill_asan_ubsan': True,
        'full_binary_json_ubsan': True
    },
    'limitations': ['No hidden formal cases or score.',
                    'Paired performance screening is recorded separately in from-best/screen.json.',
                    'WAL fixture injects short/failed writes; not fdatasync or allocation failure.',
                    'ASan leak detection disabled; memory-safety and undefined-behavior checks enabled.']
}
Path('protocol-results/verification-summary.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
PYSUMMARY
