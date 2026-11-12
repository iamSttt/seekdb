#!/usr/bin/env bash
# Experimental I15 matrix. All variants use this exact commit, including harness.
# Full: three evaluation seeds x five pairs, boundaries initially screen only.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
revision=$(git rev-parse HEAD)
out="$repo/pgo-matrix-results"
test ! -e "$out"
dump_reports() {
  result=$?
  trap - EXIT
  set +e
  python3 minivec/tools/pgo/matrix/report.py collect "$out" "$revision" "$result"
  proof_result=$?
  if [ "$result" -eq 0 ] && [ "$proof_result" -ne 0 ]; then result=$proof_result; fi
  if [ "$proof_result" -ne 0 ]; then
    mkdir -p "$out"
    printf '{"exit_code":%s,"report_collection_exit_code":%s}\n' "$result" "$proof_result" > "$out/collection-error.json"
  fi
  for file in "$out"/*.json; do
    [ -f "$file" ] || continue
    echo "MINIVEC_PGO_MATRIX_REPORT_BEGIN pgo-matrix-results/$(basename "$file")"
    cat "$file"
    echo 'MINIVEC_PGO_MATRIX_REPORT_END'
  done
  exit "$result"
}
trap dump_reports EXIT
# The external CI job should allow 60 min and always upload pgo-matrix-results.
# Leave five minutes for collection/upload; a timeout is incomplete evidence.
timeout --signal=TERM --kill-after=30s 3300 bash minivec/tools/pgo/matrix/run.sh "$repo" "$revision" "$out" "$@"
