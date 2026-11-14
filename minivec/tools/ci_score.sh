#!/usr/bin/env bash
# This isolated branch now runs the two separately frozen760f source-factor ablations.
# The unchanged workflow calls this entry; no main EE or retained-R stage runs.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
test ! -e protocol-results
mkdir protocol-results
export MINIVEC_REPORTS="$repo/protocol-results"
dump_reports() {
  result=$?
  set +e
  python3 - <<'PYREPORT'
import json, os
from pathlib import Path
root = Path(os.environ['MINIVEC_REPORTS'])
for path in sorted(root.rglob('*')):
    if path.is_file() and path.suffix in {'.json', '.txt'}:
        print('MINIVEC_REPORT_BEGIN ' + json.dumps(path.relative_to(root).as_posix()))
        print(path.read_text(encoding='utf-8', errors='replace'))
        print('MINIVEC_REPORT_END')
PYREPORT
  exit "$result"
}
trap dump_reports EXIT
bash minivec/tools/ci_batch_factors_1114.sh
