#!/usr/bin/env bash
# Build-only training is checked before independent-seed paired measurement.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
revision=$(git rev-parse HEAD)
out="$repo/pgo-results"
test ! -e "$out"
dump_reports() {
  result=$?
  trap - EXIT
  set +e
  python3 minivec/tools/pgo/report.py collect "$out" "$revision" "$result"
  proof_result=$?
  if [ "$result" -eq 0 ] && [ "$proof_result" -ne 0 ]; then result=$proof_result; fi
  if [ "$proof_result" -ne 0 ]; then
    # Preserve a machine-readable failure even if evidence itself is malformed.
    mkdir -p "$out"
    printf '{"exit_code":%s,"report_collection_exit_code":%s}\n' "$result" "$proof_result" > "$out/collection-error.json"
  fi
  for file in "$out"/*.json; do
    [ -f "$file" ] || continue
    echo "MINIVEC_PGO_REPORT_BEGIN pgo-results/$(basename "$file")"
    cat "$file"
    echo 'MINIVEC_PGO_REPORT_END'
  done
  exit "$result"
}
trap dump_reports EXIT
bash minivec/tools/pgo/compare.sh "$repo" "$revision" "$out"
