#!/usr/bin/env bash
# CI-only driver. Build the immutable submitted checkout, not this driver tree.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT
root_checkout="$PWD"
export RUNTIME_COMMIT=e8534416dcef2af64847330ffedb12831e45885d
export IMAGE=minivec:final-e853-20261112
driver_commit=$(git rev-parse HEAD)
git fetch --no-tags --depth=1 origin "$RUNTIME_COMMIT"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-verify-XXXXXX")
git worktree add --detach "$scratch/engine" "$RUNTIME_COMMIT"
ln -s "$scratch/engine/protocol-results" "$root_checkout/protocol-results"
cd "$scratch/engine"
export MINIVEC_REPORTS="$PWD/protocol-results"
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


# Record exact checkout and build inputs
set -euo pipefail
test "$(git rev-parse HEAD)" = "$RUNTIME_COMMIT"
test -z "$(git status --porcelain)"
mkdir -p protocol-results
git rev-parse HEAD > protocol-results/runtime-commit.txt
git ls-files -z Dockerfile .dockerignore minivec/src minivec/third_party STDIO_PROTOCOL.md protocol_check mysqltest | xargs -0 sha256sum > protocol-results/input-sha256.txt
lscpu > protocol-results/lscpu.txt
docker version > protocol-results/docker-version.txt
g++ --version > protocol-results/compiler.txt

printf '%s\n' "$driver_commit" > protocol-results/ci-driver-commit.txt

# Build the submitted root Dockerfile
set -euo pipefail
docker build --platform linux/amd64 --progress=plain -t "$IMAGE" . 2>&1 | tee protocol-results/docker-build.log
docker image inspect "$IMAGE" > protocol-results/image-inspect.json
docker image inspect "$IMAGE" --format '{{.Id}}' | tee protocol-results/image-id.txt

# Official image handshake and unchanged 20 public cases
set -euo pipefail
python3 protocol_check/run.py --image "$IMAGE" --handshake-only --output protocol-results/image-handshake 2>&1 | tee protocol-results/image-handshake.log
python3 protocol_check/run.py --image "$IMAGE" --output protocol-results/image-public 2>&1 | tee protocol-results/image-public.log

# Interactive image requests with stdin kept open
set -euo pipefail
python3 - <<'PY' 2>&1 | tee protocol-results/image-interactive.log
import json, os, sys, time
from pathlib import Path
sys.path.insert(0, str(Path('protocol_check').resolve()))
from _transport import Session
cfg = json.loads(Path('protocol_check/config.json').read_text())
s = Session(cfg, Path('protocol-results/image-interactive.stderr.log'), image=os.environ['IMAGE'])
events = []
def request(sql=None, ok=True, rows=None, **kw):
    before = time.monotonic()
    response = s.request(sql=sql, **kw)
    assert response['ok'] is ok, response
    if rows is not None:
        assert response.get('rows') == rows, response
    assert not s.proc.stdin.closed
    events.append({'request': sql or kw.get('op'), 'response': response,
                   'seconds': time.monotonic()-before, 'stdin_open': True})
def shutdown_open():
    request(op='shutdown')
    s.proc.wait(timeout=15)
    assert not s.proc.stdin.closed
    assert s.proc.returncode == 0 and not s.buffer and s.proc.stdout.read() == b''
    events.append({'exit_before_stdin_close': True})
    s.stop()
try:
    s.start()
    request('CREATE TABLE probe (id BIGINT PRIMARY KEY, value VARCHAR(32));')
    request("INSERT INTO probe VALUES (1,'first;line');")
    request("INSERT INTO probe VALUES (1,'duplicate');", ok=False)
    request('SELECT id,value FROM probe ORDER BY id;', rows=[[1,'first;line']])
    request(op='unsupported_operation', ok=False)
    request('SELECT id FROM probe;', mode='invalid_mode', ok=False)
    # A valid request split inside a multibyte UTF-8 character must
    # wait for its newline, then return while stdin remains open.
    raw = (json.dumps({'id':'split-utf8','op':'execute',
                       'sql':"SELECT id FROM probe WHERE value='中';",
                       'search_mode':'exact'}, ensure_ascii=False)+'\n').encode()
    cut = raw.index('中'.encode()) + 1
    assert os.write(s.proc.stdin.fileno(), raw[:cut]) == cut
    assert not s.buffer and not s.selector.select(0.05)
    assert os.write(s.proc.stdin.fileno(), raw[cut:]) == len(raw)-cut
    response = s.read(cfg['request_timeout'])
    assert response['id'] == 'split-utf8' and response['ok'] is True and response['rows'] == []
    assert not s.proc.stdin.closed
    events.append({'fragmented_utf8_request':True, 'response':response, 'stdin_open':True})
    request('BEGIN;')
    request("INSERT INTO probe VALUES (2,'rollback');")
    shutdown_open()
    s.start()
    request('SELECT id FROM probe ORDER BY id;', rows=[[1]])
    request('BEGIN;')
    request("INSERT INTO probe VALUES (3,'committed');")
    request('COMMIT;')
    request('BEGIN;')
    request("INSERT INTO probe VALUES (4,'uncommitted');")
    s.crash()
    s.start()
    request('SELECT id FROM probe ORDER BY id;', rows=[[1],[3]])
    shutdown_open()
finally:
    s.close()
    Path('protocol-results/image-interactive.json').write_text(json.dumps(events, ensure_ascii=False, indent=2))
print('PASS: official Session, live stdin, success/error continuation, shutdown before EOF, rollback, crash and restart')
PY

# Existing numeric transaction recovery and sorting regressions
set -euo pipefail
make -C minivec -j2 test 2>&1 | tee protocol-results/source-regressions.log
python3 protocol_check/run.py --command '["./minivec/minivec","--data","{data_dir}"]' --output protocol-results/source-public 2>&1 | tee protocol-results/source-public.log

# Validate the binary extracted from the actual image
set -euo pipefail
container=$(docker create "$IMAGE")
docker cp "$container:/minivec" /tmp/minivec-image-binary
docker rm "$container"
sha256sum /tmp/minivec-image-binary | tee protocol-results/image-binary-sha256.txt
python3 minivec/tests/json_protocol.py --binary /tmp/minivec-image-binary --timeout 15 2>&1 | tee protocol-results/image-binary-json.log
python3 minivec/tests/topk_bounds.py --binary /tmp/minivec-image-binary 2>&1 | tee protocol-results/image-binary-topk.log
MVDB_QUERY_THREADS=2 python3 minivec/tests/topk_bounds.py --binary /tmp/minivec-image-binary 2>&1 | tee protocol-results/image-binary-topk-parallel.log

# Candidate WAL recovery checks; all ordinary regressions ran above.
candidate="$PWD"
reports="$PWD/protocol-results"
export MINIVEC_REPORTS="$reports"
export PREVIOUS_COMMIT=385766a2251595867a546840fe8f41c5db6b1c9e
export CDF_HARNESS_COMMIT=cdfdb7cfe48910e4ef2506a5ffcccd143722587f
export BASELINE_IMAGE=minivec:baseline385-20261112

g++ -std=c++17 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -Iminivec/src minivec/tests/test_recovery_regressions.cpp minivec/src/db.cpp minivec/src/exec.cpp minivec/src/sql.cpp minivec/src/value.cpp minivec/src/flat_index.cpp -o /tmp/recovery-final-san
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 120 /tmp/recovery-final-san --wal-staging 2>&1 | tee "$reports/recovery-final-san.log"

g++ -std=c++17 -O1 -pthread -fsanitize=undefined -fno-sanitize-recover=all -Iminivec/src minivec/src/*.cpp -o /tmp/minivec-final-ubsan
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 python3 minivec/tests/json_protocol.py --binary /tmp/minivec-final-ubsan --timeout 15 2>&1 | tee "$reports/fullbinary-json-ubsan.log"
printf 'passed\n' > "$reports/current-extra-gates.txt"

# Compare the same candidate recovery fixture against the previous source.
# The strict run is observational: nonzero is not automatically a reproduction,
# and timeout's 124 specifically requires log inspection rather than a PASS.
git fetch --no-tags --depth=1 origin "$PREVIOUS_COMMIT" "$CDF_HARNESS_COMMIT"
git worktree add --detach "$scratch/previous" "$PREVIOUS_COMMIT"
git worktree add --detach "$scratch/cdf" "$CDF_HARNESS_COMMIT"
observe() {
  local name=$1
  shift
  local status=0
  set +e
  "$@" > "$reports/$name.log" 2>&1
  status=$?
  set -e
  cat "$reports/$name.log"
  printf '%s\n' "$status" > "$reports/$name-status.txt"
}
cd "$scratch/previous"
git ls-files -z Dockerfile .dockerignore minivec/src minivec/third_party | xargs -0 sha256sum > "$reports/previous-input-sha256.txt"
g++ -std=c++17 -O2 -pthread -Iminivec/src "$candidate/minivec/tests/test_recovery_regressions.cpp" minivec/src/db.cpp minivec/src/exec.cpp minivec/src/sql.cpp minivec/src/value.cpp minivec/src/flat_index.cpp -o /tmp/recovery-previous385
observe previous-recovery-strict timeout 120 /tmp/recovery-previous385 --wal-staging
timeout 120 /tmp/recovery-previous385 --wal-staging-observe 2>&1 | tee "$reports/previous-recovery-observe.log"
printf '0\n' > "$reports/previous-recovery-observe-status.txt"
python3 - <<'PYOBSERVATION'
import json, os
from pathlib import Path
root = Path(os.environ['MINIVEC_REPORTS'])
strict = int((root/'previous-recovery-strict-status.txt').read_text())
report = {
    'runtime_commit': os.environ['PREVIOUS_COMMIT'],
    'test_source_commit': os.environ['RUNTIME_COMMIT'],
    'strict_shell_exit_code': strict,
    'observation_mode_shell_exit_code': 0,
    'reproduction_conclusion': None,
    'note': 'Inspect the strict log for the specific assertion. A nonzero exit or timeout 124 alone is not proof. The OBSERVED mode verifies recovery while allowing accepted=true; it does not certify the strict rejection semantics.'
}
(root/'previous-recovery-observation.json').write_text(json.dumps(report, indent=2)+'\n')
PYOBSERVATION

docker build --platform linux/amd64 --progress=plain -t "$BASELINE_IMAGE" . 2>&1 | tee "$reports/previous-docker-build.log"
docker image inspect "$BASELINE_IMAGE" > "$reports/previous-image-inspect.json"
git diff --exit-code

# Both real images use the identical CDF-frozen measurement harness and limits.
# Every benchmark contains 1,000 queries per mode, not 1,000 total; profile's
# requests argument is per query class. These are local diagnostics, not E.
cd "$scratch/cdf"
sha256sum minivec/tools/{benchmark_jsonl,profile_jsonl,profile_container}.py protocol_check/{config.json,_transport.py} > "$reports/measurement-harness-sha256.txt"
python3 - <<'PYVERSIONS'
import json, os
from pathlib import Path
report = {'candidate_commit': os.environ['RUNTIME_COMMIT'],
          'candidate_image': os.environ['IMAGE'],
          'previous_commit': os.environ['PREVIOUS_COMMIT'],
          'previous_image': os.environ['BASELINE_IMAGE'],
          'measurement_harness_commit': os.environ['CDF_HARNESS_COMMIT'],
          'seeds': [7319,7320,7321],
          'benchmark': {'rows':10000,'queries_per_mode':1000},
          'profile': {'write_rows':1000,'query_rows':10000,'requests_per_class':500},
          'order': {'7319':['baseline385','candidate'],
                    '7320':['candidate','baseline385'],
                    '7321':['baseline385','candidate']}}
(Path(os.environ['MINIVEC_REPORTS'])/'paired-version-map.json').write_text(json.dumps(report, indent=2)+'\n')
PYVERSIONS
for seed in 7319 7320 7321; do
  case "$seed" in
    7319|7321) order="baseline385 candidate" ;;
    7320) order="candidate baseline385" ;;
  esac
  for version in $order; do
    case "$version" in
      baseline385) measured_image="$BASELINE_IMAGE" ;;
      candidate) measured_image="$IMAGE" ;;
    esac
    python3 minivec/tools/benchmark_jsonl.py --image "$measured_image" --rows 10000 --queries 1000 --seed "$seed" --sudo-cgroup --output "$reports/bench-$version-$seed.json"
    python3 minivec/tools/profile_jsonl.py --image "$measured_image" --write-rows 1000 --query-rows 10000 --requests 500 --seed "$seed" --timeout 600 --output "$reports/profile-$version-$seed.json"
  done
done
git diff --exit-code
cd "$candidate"

# Optional historical failure diagnosis, never a replacement for official
# defaults above: copy startup_timeout=2 only for this native legacy probe.
export LEGACY_COMMIT=ff6a3bdbf6f10a1b17908229b170aa85579f5be9
git fetch --no-tags --depth=1 origin "$LEGACY_COMMIT"
git worktree add --detach "$scratch/legacy" "$LEGACY_COMMIT"
make -C "$scratch/legacy/minivec" -j2
export LEGACY_BINARY="$scratch/legacy/minivec/minivec"
python3 - <<'PYLEGACY' 2>&1 | tee "$reports/legacy-no-ready-diagnostic.log"
import json, os, sys
from pathlib import Path
sys.path.insert(0, str(Path('protocol_check').resolve()))
from _transport import Session
reports = Path(os.environ['MINIVEC_REPORTS'])
official = json.loads(Path('protocol_check/config.json').read_text())
config = dict(official)
config['startup_timeout'] = 2
record = {'scope':'historical-native-startup-diagnostic',
          'runtime_commit':os.environ['LEGACY_COMMIT'],
          'official_startup_timeout_seconds':official['startup_timeout'],
          'diagnostic_startup_timeout_seconds':2,
          'official_limits_were_not_modified':True,
          'note':'This copied two-second diagnostic limit is not the official grading timeout. No SQL or request is sent before waiting for ready.'}
session = Session(config, reports/'legacy-no-ready.stderr.log',
                  command=[os.environ['LEGACY_BINARY'],'--data','{data_dir}'])
process = None
try:
    try:
        session.start()
        record['startup_result'] = 'received_ready'
    except Exception as error:
        record['startup_result'] = type(error).__name__
        record['diagnostic_error'] = str(error)
    process = session.proc
    record['alive_before_cleanup'] = process is not None and process.poll() is None
    record['stdin_open_before_cleanup'] = process is not None and not process.stdin.closed
    record['stdout_buffer_bytes'] = len(session.buffer)
    record['stdout_pending_readable'] = bool(session.selector and session.selector.select(0))
finally:
    session.close()
    record['process_exited_after_cleanup'] = process is None or process.poll() is not None
    (reports/'legacy-no-ready-diagnostic.json').write_text(json.dumps(record, indent=2)+'\n')
print(json.dumps(record, indent=2))
PYLEGACY
cd "$candidate"


# Material diagnostics are separate; retain their exit code explicitly.
set +e
python3 minivec/tools/validate_material.py 2>&1 | tee protocol-results/material-check.log
material_status=${PIPESTATUS[0]}
printf '%s\n' "$material_status" > protocol-results/material-check-status.txt
set -e

# Verify official fixtures were not changed
git diff --exit-code -- STDIO_PROTOCOL.md protocol_check mysqltest
