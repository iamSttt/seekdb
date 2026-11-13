#!/usr/bin/env bash
# Same-runner paired evidence; does not calculate the organizer's E score.
set -euo pipefail
unset MVDB_PROTOCOL MVDB_OUT MVDB_QUERY_THREADS MVDB_ANN_PROBE_PERCENT MVDB_ANN_OFF
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
baseline=a90b2b750e40f020793cddefe01d5b66631afc63
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/minivec-paired-XXXXXX")
test ! -e iteration-results
mkdir iteration-results "$scratch/baseline"
stage=baseline-build
dump_reports() {
  result=$?
  set +e
  printf '{"exit_code":%s,"stage":"%s"}\n' "$result" "$stage" > iteration-results/run-status.json
  for file in iteration-results/*.json; do
    [ -f "$file" ] || continue
    echo "MINIVEC_ITERATION_REPORT_BEGIN $file"
    cat "$file"
    echo "MINIVEC_ITERATION_REPORT_END"
  done
  exit "$result"
}
trap dump_reports EXIT
git archive "$baseline" | tar -x -C "$scratch/baseline"
for side in baseline candidate; do
  stage="$side-build"
  source="$repo"
  if [ "$side" = baseline ]; then source="$scratch/baseline"; fi
  docker build --platform linux/amd64 -t "minivec:paired-$side" "$source"
  container=$(docker create "minivec:paired-$side")
  docker cp "$container:/minivec" "$scratch/$side.bin"
  docker rm "$container"
  sha256sum "$scratch/$side.bin" | tee "iteration-results/$side-sha256.txt"
done
export ITERATION_BASELINE="$baseline" ITERATION_CANDIDATE
ITERATION_CANDIDATE=$(git rev-parse HEAD)
export ITERATION_SCRATCH="$scratch"
stage=paired-benchmark
python3 - <<'PY'
import json, os, platform, subprocess
from pathlib import Path
cpus = sorted(os.sched_getaffinity(0))[:2]
os.sched_setaffinity(0, cpus)
context = {
    'baseline_commit': os.environ['ITERATION_BASELINE'],
    'candidate_commit': os.environ['ITERATION_CANDIDATE'],
    'cpu_affinity': cpus,
    'platform': platform.platform(),
    'tracked_worktree_status': subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], text=True),
    'compiler': 'Root Dockerfile pinned GCC 13.3.0, static -O3 for both programs',
    'resources': 'Two-CPU affinity on the same GitHub runner. RSS and /proc I/O are proxies, not official cgroup M/B. No formal E conversion.'
}
Path('iteration-results/context.json').write_text(json.dumps(context, indent=2)+'\n')
common = ['python3', 'minivec/tools/benchmark_iteration.py',
          '--baseline', os.environ['ITERATION_SCRATCH']+'/baseline.bin',
          '--candidate', os.environ['ITERATION_SCRATCH']+'/candidate.bin']
subprocess.run(common+['--suite', 'smoke', '--output', 'iteration-results/smoke.json'], check=True)
subprocess.run(common+['--suite', 'full', '--output', 'iteration-results/paired.json'], check=True)
PY
stage=complete
