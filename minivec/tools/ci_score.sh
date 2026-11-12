#!/usr/bin/env bash
# CI-only driver. Build the immutable submitted checkout, not this driver tree.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT
root_checkout="$PWD"
export RUNTIME_COMMIT=cdfdb7cfe48910e4ef2506a5ffcccd143722587f
export IMAGE=minivec:cdfdb7cf-20261112
driver_commit=$(git rev-parse HEAD)
git fetch --no-tags --depth=1 origin "$RUNTIME_COMMIT"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-verify-XXXXXX")
git worktree add --detach "$scratch/engine" "$RUNTIME_COMMIT"
ln -s "$scratch/engine/protocol-results" "$root_checkout/protocol-results"
cd "$scratch/engine"
dump_reports() {
  result=$?
  set +e
  python3 - <<'PYREPORT'
import json
from pathlib import Path
root = Path('protocol-results')
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

# Measure end-to-end paths for the next optimization
set -euo pipefail
python3 minivec/tools/profile_jsonl.py --image "$IMAGE" --write-rows 2000 --requests 1000 --timeout 600 --output protocol-results/path-profile.json 2>&1 | tee protocol-results/path-profile.log
python3 minivec/tools/benchmark_jsonl.py --image "$IMAGE" --rows 10000 --queries 1000 --seed 7319 --sudo-cgroup --output protocol-results/baseline-workload.json 2>&1 | tee protocol-results/baseline-workload.log

# Material diagnostics are separate; retain their exit code explicitly.
set +e
python3 minivec/tools/validate_material.py 2>&1 | tee protocol-results/material-check.log
material_status=${PIPESTATUS[0]}
printf '%s\n' "$material_status" > protocol-results/material-check-status.txt
set -e

# Verify official fixtures were not changed
git diff --exit-code -- STDIO_PROTOCOL.md protocol_check mysqltest
