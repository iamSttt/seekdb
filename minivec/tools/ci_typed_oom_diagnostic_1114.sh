#!/usr/bin/env bash
# Necessary correctness diagnostic, never a performance or acceptance run.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
out="$MINIVEC_REPORTS/typed-oom-diagnostic"
mkdir "$out"
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/typed-oom-diagnostic-XXXXXX")
compiler='gcc:13.3.0-bookworm@sha256:1d71f0f3450214bef38fe09e6f610fb6cca90cf97b43f4ce845bfc32a4168818'
python3 - <<'PY'
import hashlib,json
from pathlib import Path
p=json.loads(Path('validation/typed-oom-diagnostic-plan-20261114.json').read_bytes())
assert hashlib.sha256(Path('minivec/tests/test_typed_oom.cpp').read_bytes()).hexdigest()==p['diagnostic_test_sha256']
assert hashlib.sha256(Path('minivec/tools/ci_typed_oom_diagnostic_1114.sh').read_bytes()).hexdigest()==p['diagnostic_driver_sha256']
PY
for role in base typed lazy; do
  commit=760f66220f2c7186b4ba295f9e3065650e549f9e
  if [[ $role == typed ]]; then commit=6412cbf862959655f351b4d908c430a98c5eabfe; fi
  if [[ $role == lazy ]]; then commit=9595e1477af1325795b0f9fed52e6a44bb217b1c; fi
  git fetch --no-tags --depth=1 origin "$commit"
  mkdir "$scratch/$role"
  git archive "$commit" Dockerfile .dockerignore minivec/src minivec/third_party | tar -x -C "$scratch/$role"
  docker run --rm -v "$scratch/$role/minivec:/src:ro" -v "$repo/minivec/tests:/harness:ro" -v "$scratch:/out" -w /src "$compiler" \
    g++ -O3 -g -std=c++17 -fno-math-errno -static -pthread -Isrc \
    /harness/test_typed_oom.cpp src/db.cpp src/exec.cpp src/flat_index.cpp src/sql.cpp src/value.cpp -o "/out/$role-oom" \
    > "$out/$role-build.txt" 2>&1
  set +e
  timeout 120 "$scratch/$role-oom" > "$out/$role-stdout.txt" 2> "$out/$role-stderr.txt"
  result=$?
  set -e
  printf '%s\n' "$result" > "$out/$role-exit.txt"
  sha256sum "$scratch/$role-oom" > "$out/$role-harness-sha256.txt"
  python3 - "$out/$role-stderr.txt" "$out/$role-addresses.txt" <<'PY'
import re,sys
from pathlib import Path
text=Path(sys.argv[1]).read_text()
Path(sys.argv[2]).write_text('\n'.join(re.findall(r'\[(0x[0-9a-fA-F]+)\]',text))+'\n')
PY
  mapfile -t addresses < "$out/$role-addresses.txt"
  if [[ -n "${addresses[0]:-}" ]]; then
    docker run --rm -v "$scratch:/out:ro" "$compiler" addr2line -e "/out/$role-oom" -f -C "${addresses[@]}" > "$out/$role-stack.txt"
  fi
done
python3 - "$out" <<'PY'
import json,sys
from pathlib import Path
out=Path(sys.argv[1]);p=json.loads(Path('validation/typed-oom-diagnostic-plan-20261114.json').read_bytes())
roles={name:{'source_commit':commit,'exit_code':int((out/(name+'-exit.txt')).read_text()),'stderr':(out/(name+'-stderr.txt')).read_text()} for name,commit in p['roles'].items()}
report={'status':'diagnostic_finished_not_validation','roles':roles,'performance_pairs':0,'release_gate_passed':False,'source_changes':False,'scope':'Same original sweep and assertions; added no-allocation ordinal log and terminate backtrace only. All failures retained.'}
(out/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
PY
# Do not turn a reproduced crash into a green correctness result.
exit 20
