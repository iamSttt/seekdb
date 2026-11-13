#!/usr/bin/env bash
# Exactly one already-built actual root PGO program; neither reference rebuilt.
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS MVDB_PARSE_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(git rev-parse --show-toplevel)
cd "$repo"
binary=$(realpath "$1")
image=$2
reports="$repo/protocol-results"
out="$reports/insert-copy-root"
test ! -e "$out"
mkdir "$out"
finish() {
  local code=$?
  trap - EXIT
  set +e
  python3 minivec/tools/summarize_insert_copy_root.py --output "$out" --reports "$reports" \
    --binary "$binary" --image "$image" --exit-code "$code"
  local report_code=$?
  if [[ "$code" == 0 && "$report_code" != 0 ]]; then code=$report_code;fi
  exit "$code"
}
trap finish EXIT
python3 minivec/tools/prepare_insert_copy_root.py --repo "$repo" --binary "$binary" \
  --image "$image" --output "$out/programs" 2>&1 | tee "$out/prepare.log"
cc --version > "$out/launcher-compiler.txt"
cc -O2 -static minivec/tools/cgroup_exec.c -o "$out/cgroup_exec" 2>&1 | tee "$out/launcher-build.log"
sha256sum minivec/tools/cgroup_exec.c "$out/cgroup_exec" > "$out/launcher-sha256.txt"
lscpu > "$out/lscpu.txt"
# Alternate reference scheduling across CPU counts. Each individual contrast
# separately runs AB/BA; no ratios are multiplied across contrasts or hosts.
for item in retained:1 ee:1 ee:2 retained:2; do
  reference=${item%:*}
  cpus=${item#*:}
  sudo -n env PYTHONDONTWRITEBYTECODE=1 python3 "$repo/minivec/tools/small_compute.py" \
    --tools "$repo/minivec/tools" --identities "$out/programs/$reference-identities.json" \
    --launcher "$out/cgroup_exec" --output "$out/$reference-${cpus}cpu.json" \
    --allow-version-contrast --db-cpu-count "$cpus" --insert-copy-reference "$reference" \
    --query-mode exact --memory-max 2147483648 --swap-max 0 \
    --cpu-quota-us "$((cpus * 100000))" --cpu-period-us 100000 --case-timeout 600 \
    2>&1 | tee "$out/$reference-${cpus}cpu.log"
done
