#!/usr/bin/env bash
# Combined hot-path candidate versus the previously measured non-LTO revision.
# Results do not isolate the individual changes or calculate an official score.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_SINGLE_THREAD MVDB_QUERY_THREADS
unset MVDB_QUERY_FANOUT_ROWS MVDB_WAKE_US_OVERRIDE MVDB_PARSE_THREADS
unset MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
baseline=4d8de37178eb3d5f36e9df04f8c80740ddfdfba4
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-hotpath-XXXXXX")
test ! -e hotpath-results
mkdir hotpath-results "$scratch/baseline"
stage=baseline-build
dump_reports() {
  result=$?
  set +e
  printf '{"exit_code":%s,"stage":"%s"}\n' "$result" "$stage" > hotpath-results/run-status.json
  for file in hotpath-results/*.json; do
    [ -f "$file" ] || continue
    echo "MINIVEC_HOTPATH_REPORT_BEGIN $file"
    cat "$file"
    echo "MINIVEC_HOTPATH_REPORT_END"
  done
  exit "$result"
}
trap dump_reports EXIT
git archive "$baseline" | tar -x -C "$scratch/baseline"
for side in baseline candidate; do
  stage="$side-build"
  source="$repo"
  if [ "$side" = baseline ]; then source="$scratch/baseline"; fi
  docker build --platform linux/amd64 -t "minivec:hotpath-$side" "$source"
  container=$(docker create "minivec:hotpath-$side")
  docker cp "$container:/minivec" "$scratch/$side.bin"
  docker rm "$container"
  sha256sum "$scratch/$side.bin" | tee "hotpath-results/$side-sha256.txt"
done
export HOTPATH_SCRATCH="$scratch" HOTPATH_BASELINE="$baseline"
stage=paired-benchmark
python3 - <<'PY'
import json, os, platform, subprocess
from pathlib import Path
cpus = sorted(os.sched_getaffinity(0))[:2]
os.sched_setaffinity(0, cpus)
Path('hotpath-results/context.json').write_text(json.dumps({
    'baseline_commit': os.environ['HOTPATH_BASELINE'],
    'candidate_commit': subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
    'tracked_worktree_status': subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],text=True),
    'platform': platform.platform(), 'cpu_affinity': cpus,
    'compiler': 'Both root Dockerfiles: pinned GCC 13.3.0, static -O3, no LTO.',
    'comparison_scope': 'Combined candidate; no individual optimization attribution without an independent ablation.',
    'diagnostics': 'Per-run stderr tails preserve observed policy messages. Settings or affinity alone do not prove fanout execution.',
    'limitations': 'Same-runner measurements with process memory/I/O proxies; no official E/cgroup score.'
},indent=2)+'\n')
common = ['python3','minivec/tools/benchmark_iteration.py', '--suite','full',
          '--baseline',os.environ['HOTPATH_SCRATCH']+'/baseline.bin',
          '--candidate',os.environ['HOTPATH_SCRATCH']+'/candidate.bin',
          '--seeds','7319','8911','13337','--pairs','5']
subprocess.run(common+['--scenario','standard10k','--queries','1000',
                      '--output','hotpath-results/standard.json'],check=True)
boundaries = ['resident30k','resident35k','resident50k','low16','filter60k']
scenarios = [argument for name in boundaries for argument in ('--scenario',name)]
subprocess.run(common+scenarios+['--queries','64','--output','hotpath-results/boundaries.json'],check=True)
PY
stage=complete
